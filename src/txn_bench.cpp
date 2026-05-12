#include "txn_bench.hpp"
#include "csv.hpp"
#include "schema.hpp"
#include "stats.hpp"
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace {

// Write `records_per_dbi` random-keyed records for `dbi_idx` inside `txn`.
void write_dbi_records(EnvHandle& h, mdbx::txn_managed& txn,
                       size_t dbi_idx, size_t records_per_dbi,
                       uint64_t txn_i, std::mt19937_64& rng,
                       std::vector<uint8_t>& val_buf,
                       uint8_t* key_buf) {
    const auto& spec = SCHEMA[dbi_idx];
    if (val_buf.size() < spec.val_size) val_buf.resize(spec.val_size);
    for (size_t r = 0; r < records_per_dbi; ++r) {
        uint64_t seq = spec.record_count + txn_i * records_per_dbi + r;
        make_key_seq(key_buf, spec.key_size, seq ^ rng());
        make_val(val_buf.data(), spec.val_size, seq);
        mdbx::slice k(key_buf, spec.key_size);
        mdbx::slice v(val_buf.data(), spec.val_size);
        txn.upsert(h.dbis[dbi_idx], k, v);
    }
}

} // namespace

void run_txn_bench(EnvHandle& h, const TxnBenchConfig& cfg, CsvWriter& csv) {
    const bool per_dbi_layout = (h.cfg.layout == EnvLayout::PerDbi);

    std::cerr << "[txn] atomic=" << (cfg.atomic ? 1 : 0)
              << " txn_count=" << cfg.txn_count
              << " records_per_dbi=" << cfg.records_per_dbi_per_txn
              << " layout=" << to_string(h.cfg.layout) << "\n";
    if (cfg.atomic && per_dbi_layout) {
        std::cerr << "[txn] note: per-DBI atomic is emulated as N independent "
                     "commits (one per env); commits are not atomic across envs.\n";
    }

    Histogram commit_hist;
    std::mt19937_64 rng(0xABCDEFULL ^ (cfg.atomic ? 1ULL : 0ULL));
    alignas(8) uint8_t key_buf[64];
    std::vector<uint8_t> val_buf(1024);

    Timer total;

    if (cfg.atomic) {
        for (size_t i = 0; i < cfg.txn_count; ++i) {
            if (per_dbi_layout) {
                // Emulated atomic: open N txns, write, then commit each.
                // Sum the commit times so the metric reflects "what it costs
                // to make an N-DBI write look atomic-ish across envs".
                std::vector<mdbx::txn_managed> txns;
                txns.reserve(DBI_COUNT);
                for (size_t di = 0; di < DBI_COUNT; ++di)
                    txns.emplace_back(h.envs[di].start_write());
                for (size_t di = 0; di < DBI_COUNT; ++di) {
                    write_dbi_records(h, txns[di], di,
                                      cfg.records_per_dbi_per_txn, i, rng,
                                      val_buf, key_buf);
                }
                Timer ct;
                try {
                    for (auto& t : txns) t.commit();
                } catch (...) {
                    // Abort any txns that haven't been committed yet.
                    for (auto& t : txns) {
                        try { t.abort(); } catch (...) {}
                    }
                    throw;
                }
                commit_hist.record(ct.elapsed_ns());
            } else {
                auto txn = h.envs[0].start_write();
                for (size_t di = 0; di < DBI_COUNT; ++di) {
                    write_dbi_records(h, txn, di,
                                      cfg.records_per_dbi_per_txn, i, rng,
                                      val_buf, key_buf);
                }
                Timer ct; txn.commit(); commit_hist.record(ct.elapsed_ns());
            }
        }
    } else {
        for (size_t i = 0; i < cfg.txn_count; ++i) {
            for (size_t di = 0; di < DBI_COUNT; ++di) {
                auto txn = h.env_for(di).start_write();
                write_dbi_records(h, txn, di,
                                  cfg.records_per_dbi_per_txn, i, rng,
                                  val_buf, key_buf);
                Timer ct; txn.commit(); commit_hist.record(ct.elapsed_ns());
            }
        }
    }

    uint64_t elapsed_ns = total.elapsed_ns();
    double seconds = elapsed_ns / 1.0e9;
    // For non-atomic case, total commits = txn_count * DBI_COUNT; for atomic,
    // txn_count in single layout, txn_count * DBI_COUNT in per-DBI (emulated).
    uint64_t total_commits;
    if (cfg.atomic) {
        total_commits = per_dbi_layout ? cfg.txn_count * DBI_COUNT
                                       : cfg.txn_count;
    } else {
        total_commits = cfg.txn_count * DBI_COUNT;
    }
    double commits_per_sec = (seconds > 0)
        ? static_cast<double>(total_commits) / seconds : 0.0;

    csv.write_row({
        cfg.atomic ? "1" : "0",
        std::to_string(DBI_COUNT),
        std::to_string(cfg.records_per_dbi_per_txn),
        std::to_string(cfg.txn_count),
        std::to_string(commits_per_sec),
        std::to_string(commit_hist.p50_us()),
        std::to_string(commit_hist.p99_us()),
    });
}
