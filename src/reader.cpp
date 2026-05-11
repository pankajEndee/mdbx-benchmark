#include "reader.hpp"
#include "csv.hpp"
#include "schema.hpp"
#include "stats.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

void reader_thread(EnvHandle& h, const ReadConfig& cfg, unsigned tid,
                   size_t ops_this_thread, Histogram& out_hist,
                   uint64_t seed) {
    std::mt19937_64 rng(seed);

    // Build a Zipf CDF per-thread when needed. We use the smallest DBI's
    // record_count as the key universe so we don't sample out-of-range keys.
    uint64_t max_key = SCHEMA[0].record_count;
    for (size_t i = 1; i < DBI_COUNT; ++i)
        if (SCHEMA[i].record_count < max_key) max_key = SCHEMA[i].record_count;

    ZipfGen zipf;
    if (cfg.pattern == KeyPattern::Zipfian) {
        // theta=0.99 is the classic skewed setting.
        zipf.init(max_key, 0.99);
    }

    alignas(8) uint8_t key_buf[64];

    // In single-env mode all DBIs share one read txn. In per-DBI mode each
    // env needs its own read txn; we keep them in a vector indexed by dbi_idx.
    const bool per_dbi = (h.cfg.layout == EnvLayout::PerDbi);
    std::vector<mdbx::txn_managed> txns;
    if (per_dbi) {
        txns.reserve(DBI_COUNT);
        for (size_t i = 0; i < DBI_COUNT; ++i)
            txns.emplace_back(h.envs[i].start_read());
    } else {
        txns.emplace_back(h.envs[0].start_read());
    }
    auto txn_for = [&](size_t dbi_idx) -> mdbx::txn_managed& {
        return per_dbi ? txns[dbi_idx] : txns[0];
    };

    for (size_t op = 0; op < ops_this_thread; ++op) {
        uint64_t key_seq;
        switch (cfg.pattern) {
            case KeyPattern::Sequential: key_seq = op % max_key; break;
            case KeyPattern::Zipfian:    key_seq = zipf.sample(rng); break;
            case KeyPattern::Random:
            default:                     key_seq = rng() % max_key; break;
        }

        static const mdbx::slice kAbsent{};
        if (cfg.cross_dbi) {
            Timer t;
            for (size_t i = 0; i < DBI_COUNT; ++i) {
                size_t ks = SCHEMA[i].key_size;
                make_key_seq(key_buf, ks, key_seq % SCHEMA[i].record_count);
                mdbx::slice k(key_buf, ks);
                (void)txn_for(i).get(h.dbis[i], k, kAbsent);
            }
            out_hist.record(t.elapsed_ns());
        } else {
            // Single-DBI read: rotate DBIs by thread for variety.
            size_t dbi_idx = tid % DBI_COUNT;
            size_t ks = SCHEMA[dbi_idx].key_size;
            make_key_seq(key_buf, ks, key_seq % SCHEMA[dbi_idx].record_count);
            mdbx::slice k(key_buf, ks);
            Timer t;
            (void)txn_for(dbi_idx).get(h.dbis[dbi_idx], k, kAbsent);
            out_hist.record(t.elapsed_ns());
        }

        // Occasionally renew the txn(s) to avoid pinning page reclaim.
        if ((op & 0xFFFF) == 0xFFFF) {
            if (per_dbi) {
                for (size_t i = 0; i < DBI_COUNT; ++i) {
                    txns[i].abort();
                    txns[i] = h.envs[i].start_read();
                }
            } else {
                txns[0].abort();
                txns[0] = h.envs[0].start_read();
            }
        }
    }

    for (auto& t : txns) t.abort();
}

} // namespace

void run_point_reads(EnvHandle& h, const ReadConfig& cfg,
                     const std::string& phase_name,
                     CsvWriter& csv, CsvWriter* hist_csv) {
    std::cerr << "[" << phase_name << "] threads=" << cfg.thread_count
              << " pattern=" << to_string(cfg.pattern)
              << " cross_dbi=" << (cfg.cross_dbi ? 1 : 0)
              << " ops_per_thread=" << cfg.ops_per_thread << "\n";

    size_t total_ops = cfg.ops_per_thread * cfg.thread_count;
    std::vector<Histogram> hists(cfg.thread_count);
    std::vector<std::jthread> threads;
    threads.reserve(cfg.thread_count);

    Timer total;
    for (unsigned t = 0; t < cfg.thread_count; ++t) {
        threads.emplace_back(reader_thread,
                             std::ref(h), std::cref(cfg), t,
                             cfg.ops_per_thread, std::ref(hists[t]),
                             0xC0FFEEULL ^ (uint64_t)t);
    }
    threads.clear(); // jthread joins on destruction; force join now via clear.

    uint64_t elapsed_ns = total.elapsed_ns();

    Histogram merged;
    for (auto& hist : hists) merged.merge(hist);

    double elapsed_ms = elapsed_ns / 1.0e6;
    double ops_per_sec = (elapsed_ns > 0)
        ? (static_cast<double>(total_ops) * 1e9 / static_cast<double>(elapsed_ns))
        : 0.0;

    csv.write_row({
        to_string(cfg.pattern),
        std::to_string(cfg.thread_count),
        cfg.cross_dbi ? "1" : "0",
        std::to_string(total_ops),
        std::to_string(elapsed_ms),
        std::to_string(ops_per_sec),
        std::to_string(merged.p50_us()),
        std::to_string(merged.p95_us()),
        std::to_string(merged.p99_us()),
    });

    if (hist_csv) {
        std::string variant = phase_name + ":" + to_string(cfg.pattern)
            + ":threads=" + std::to_string(cfg.thread_count)
            + ":cross_dbi=" + (cfg.cross_dbi ? "1" : "0");
        merged.write_buckets(*hist_csv, variant);
    }
}
