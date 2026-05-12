#include "hot_cold.hpp"
#include "csv.hpp"
#include "schema.hpp"
#include "stats.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <stop_token>
#include <thread>
#include <vector>

void run_hot_cold(EnvHandle& h, const HotColdConfig& cfg, CsvWriter& csv) {
    std::cerr << "[hotcold] writer_txns=" << cfg.writer_txns
              << " writer_batch=" << cfg.writer_batch
              << " hold_ms=" << cfg.stale_reader_hold_ms << "\n";

    // Stale reader thread: keeps cycling read txns held open for `hold_ms`.
    // In per-DBI mode the stale reader holds one read txn per env so that
    // page reclaim is blocked across the whole multi-env set, matching the
    // single-env case where one read txn pins all DBIs.
    std::jthread stale([&](std::stop_token st) {
        while (!st.stop_requested()) {
            try {
                std::vector<mdbx::txn_managed> txns;
                txns.reserve(h.envs.size());
                for (auto& e : h.envs) txns.emplace_back(e.start_read());
                auto until = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(cfg.stale_reader_hold_ms);
                while (!st.stop_requested() &&
                       std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                for (auto& t : txns) t.abort();
            } catch (const mdbx::exception&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    });

    // Writer loop in this thread; sample stats every 1000 commits.
    std::mt19937_64 rng(0xDEAD'BEEFULL);
    alignas(8) uint8_t key_buf[64];
    std::vector<uint8_t> val_buf(1024);

    Histogram window_hist;
    uint64_t sample_idx = 0;
    constexpr uint64_t kSampleEvery = 1000;

    const bool per_dbi_layout = (h.cfg.layout == EnvLayout::PerDbi);

    auto do_dbi_writes = [&](mdbx::txn_managed& txn, size_t di, uint64_t i) {
        const auto& spec = SCHEMA[di];
        if (val_buf.size() < spec.val_size) val_buf.resize(spec.val_size);
        size_t per = std::max<size_t>(1, cfg.writer_batch / DBI_COUNT);
        for (size_t r = 0; r < per; ++r) {
            uint64_t seq = spec.record_count + i * per + r;
            if (spec.flags & MDBX_INTEGERKEY)
                make_key_int(key_buf, spec.key_size, seq ^ rng());
            else
                make_key_seq(key_buf, spec.key_size, seq ^ rng());
            make_val(val_buf.data(), spec.val_size, seq);
            mdbx::slice k(key_buf, spec.key_size);
            mdbx::slice v(val_buf.data(), spec.val_size);
            try { txn.upsert(h.dbis[di], k, v); }
            catch (const mdbx::exception&) {}
        }
    };

    for (uint64_t i = 0; i < cfg.writer_txns; ++i) {
        if (per_dbi_layout) {
            Timer ct;
            for (size_t di = 0; di < DBI_COUNT; ++di) {
                auto txn = h.env_for(di).start_write();
                do_dbi_writes(txn, di, i);
                txn.commit();
            }
            window_hist.record(ct.elapsed_ns());
        } else {
            auto txn = h.env_for(0).start_write();
            for (size_t di = 0; di < DBI_COUNT; ++di) {
                do_dbi_writes(txn, di, i);
            }
            Timer ct; txn.commit(); window_hist.record(ct.elapsed_ns());
        }

        if ((i + 1) % kSampleEvery == 0) {
            uint64_t writer_txnid = 0;
            uint64_t oldest_reader = UINT64_MAX;
            for (auto& e : h.envs) {
                MDBX_envinfo info{};
                mdbx_env_info_ex(e, nullptr, &info, sizeof(info));
                if (info.mi_recent_txnid > writer_txnid)
                    writer_txnid = info.mi_recent_txnid;
                if (info.mi_latter_reader_txnid < oldest_reader)
                    oldest_reader = info.mi_latter_reader_txnid;
            }
            if (oldest_reader == UINT64_MAX) oldest_reader = writer_txnid;
            uint64_t lag = (writer_txnid > oldest_reader)
                ? (writer_txnid - oldest_reader) : 0;
            size_t db_bytes = env_file_size_bytes(h);
            double db_mb = static_cast<double>(db_bytes) / (1024.0 * 1024.0);
            double w99 = window_hist.p99_us();

            csv.write_row({
                std::to_string(sample_idx),
                std::to_string(writer_txnid),
                std::to_string(oldest_reader),
                std::to_string(lag),
                std::to_string(db_mb),
                std::to_string(w99),
            });

            sample_idx++;
            window_hist = Histogram{};
        }
    }

    // Stop stale reader and allow page reclaim; take a few cool-down samples
    // to capture how quickly lag closes.
    stale.request_stop();
    stale.join();

    for (int cd = 0; cd < 5; ++cd) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        uint64_t writer_txnid = 0;
        uint64_t oldest_reader = UINT64_MAX;
        for (auto& e : h.envs) {
            MDBX_envinfo info{};
            mdbx_env_info_ex(e, nullptr, &info, sizeof(info));
            if (info.mi_recent_txnid > writer_txnid)
                writer_txnid = info.mi_recent_txnid;
            if (info.mi_latter_reader_txnid < oldest_reader)
                oldest_reader = info.mi_latter_reader_txnid;
        }
        if (oldest_reader == UINT64_MAX) oldest_reader = writer_txnid;
        uint64_t lag = (writer_txnid > oldest_reader)
            ? (writer_txnid - oldest_reader) : 0;
        size_t db_bytes = env_file_size_bytes(h);
        double db_mb = static_cast<double>(db_bytes) / (1024.0 * 1024.0);
        csv.write_row({
            std::to_string(sample_idx++),
            std::to_string(writer_txnid),
            std::to_string(oldest_reader),
            std::to_string(lag),
            std::to_string(db_mb),
            "0",
        });
    }
}
