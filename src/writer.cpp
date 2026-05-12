#include "writer.hpp"
#include "csv.hpp"
#include "schema.hpp"
#include "stats.hpp"
#include <atomic>
#include <iostream>
#include <random>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

void mixed_reader_thread(EnvHandle& h, const MixedConfig& cfg,
                         unsigned tid, std::stop_token st,
                         Histogram& out_hist, uint64_t& out_ops,
                         uint64_t seed) {
    std::mt19937_64 rng(seed);
    uint64_t max_key = SCHEMA[0].record_count;
    for (size_t i = 1; i < DBI_COUNT; ++i)
        if (SCHEMA[i].record_count < max_key) max_key = SCHEMA[i].record_count;
    alignas(8) uint8_t key_buf[64];
    size_t dbi_idx = tid % DBI_COUNT;
    auto txn = h.env_for(dbi_idx).start_read();
    uint64_t ops = 0;

    while (!st.stop_requested()) {
        size_t ks = SCHEMA[dbi_idx].key_size;
        uint64_t key_seq = rng() % max_key;
        uint64_t k_seq = key_seq % SCHEMA[dbi_idx].record_count;
        if (SCHEMA[dbi_idx].flags & MDBX_INTEGERKEY)
            make_key_int(key_buf, ks, k_seq);
        else
            make_key_seq(key_buf, ks, k_seq);
        mdbx::slice k(key_buf, ks);
        static const mdbx::slice kAbsent{};
        Timer t;
        (void)txn.get(h.dbis[dbi_idx], k, kAbsent);
        out_hist.record(t.elapsed_ns());
        ops++;

        if ((ops & 0xFFFF) == 0) {
            txn.abort();
            txn = h.env_for(dbi_idx).start_read();
        }
    }

    txn.abort();
    out_ops = ops;
}

} // namespace

void run_mixed(EnvHandle& h, const MixedConfig& cfg,
               CsvWriter& writer_csv, CsvWriter& reader_csv,
               CsvWriter* hist_csv) {
    std::cerr << "[mixed] readers=" << cfg.reader_threads
              << " writer_batch=" << cfg.writer_batch
              << " write_txns=" << cfg.write_txns
              << " cross_dbi_write=" << (cfg.cross_dbi_write ? 1 : 0) << "\n";

    std::vector<Histogram> reader_hists(cfg.reader_threads);
    std::vector<uint64_t> reader_ops(cfg.reader_threads, 0);

    std::stop_source stop_src;

    std::vector<std::jthread> readers;
    readers.reserve(cfg.reader_threads);
    for (unsigned t = 0; t < cfg.reader_threads; ++t) {
        readers.emplace_back([&, t](std::stop_token st) {
            mixed_reader_thread(h, cfg, t, st,
                                reader_hists[t], reader_ops[t],
                                0xBEEFULL ^ (uint64_t)t);
        });
    }

    // Writer (this thread)
    Histogram commit_hist;
    Timer writer_total;
    {
        std::mt19937_64 rng(0xFACEULL);
        alignas(8) uint8_t key_buf[64];
        std::vector<uint8_t> val_buf(1024);

        const bool per_dbi_layout = (h.cfg.layout == EnvLayout::PerDbi);

        auto upsert_for_dbi = [&](mdbx::txn_managed& txn, size_t dbi_idx,
                                  size_t per_dbi, size_t txn_i) {
            const auto& spec = SCHEMA[dbi_idx];
            if (val_buf.size() < spec.val_size) val_buf.resize(spec.val_size);
            for (size_t r = 0; r < per_dbi; ++r) {
                uint64_t key_seq = spec.record_count + (txn_i * per_dbi + r);
                uint64_t rk = rng();
                if (spec.flags & MDBX_INTEGERKEY)
                    make_key_int(key_buf, spec.key_size, key_seq ^ rk);
                else
                    make_key_seq(key_buf, spec.key_size, key_seq ^ rk);
                make_val(val_buf.data(), spec.val_size, key_seq);
                mdbx::slice k(key_buf, spec.key_size);
                mdbx::slice v(val_buf.data(), spec.val_size);
                try {
                    txn.upsert(h.dbis[dbi_idx], k, v);
                } catch (const mdbx::exception&) {}
            }
        };

        for (size_t txn_i = 0; txn_i < cfg.write_txns; ++txn_i) {
            size_t per_dbi = cfg.cross_dbi_write
                ? std::max<size_t>(1, cfg.writer_batch / DBI_COUNT)
                : cfg.writer_batch;
            size_t dbis_to_touch = cfg.cross_dbi_write ? DBI_COUNT : 1;

            if (per_dbi_layout && cfg.cross_dbi_write) {
                // One write txn per env. Commit each; total time is the sum.
                Timer ct;
                for (size_t di = 0; di < dbis_to_touch; ++di) {
                    auto txn = h.env_for(di).start_write();
                    upsert_for_dbi(txn, di, per_dbi, txn_i);
                    txn.commit();
                }
                commit_hist.record(ct.elapsed_ns());
            } else {
                size_t base_dbi = cfg.cross_dbi_write ? 0 : (txn_i % DBI_COUNT);
                auto txn = h.env_for(base_dbi).start_write();
                for (size_t di = 0; di < dbis_to_touch; ++di) {
                    size_t dbi_idx = cfg.cross_dbi_write ? di : base_dbi;
                    upsert_for_dbi(txn, dbi_idx, per_dbi, txn_i);
                }
                Timer ct;
                txn.commit();
                commit_hist.record(ct.elapsed_ns());
            }
        }
    }
    uint64_t writer_elapsed_ns = writer_total.elapsed_ns();

    // Signal readers to stop and join.
    stop_src.request_stop();
    for (auto& jt : readers) jt.request_stop();
    readers.clear();

    double writer_seconds = writer_elapsed_ns / 1.0e9;
    double commits_per_sec = (writer_seconds > 0)
        ? cfg.write_txns / writer_seconds : 0.0;

    writer_csv.write_row({
        std::to_string(cfg.writer_batch),
        std::to_string(cfg.reader_threads),
        std::to_string(cfg.write_txns),
        std::to_string(commits_per_sec),
        std::to_string(commit_hist.p50_us()),
        std::to_string(commit_hist.p99_us()),
    });

    Histogram reader_merged;
    uint64_t total_read_ops = 0;
    for (size_t t = 0; t < cfg.reader_threads; ++t) {
        reader_merged.merge(reader_hists[t]);
        total_read_ops += reader_ops[t];
    }
    double read_ops_per_sec = (writer_seconds > 0)
        ? static_cast<double>(total_read_ops) / writer_seconds : 0.0;

    reader_csv.write_row({
        std::to_string(cfg.reader_threads),
        std::to_string(cfg.writer_batch),
        std::to_string(total_read_ops),
        std::to_string(read_ops_per_sec),
        std::to_string(reader_merged.p50_us()),
        std::to_string(reader_merged.p99_us()),
    });

    if (hist_csv) {
        std::string variant = "mixed:rt=" + std::to_string(cfg.reader_threads)
            + ":wb=" + std::to_string(cfg.writer_batch);
        commit_hist.write_buckets(*hist_csv, variant + ":commit");
        reader_merged.write_buckets(*hist_csv, variant + ":read");
    }
}
