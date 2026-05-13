#include "reader.hpp"
#include "csv.hpp"
#include "schema.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct KeyPlan {
    uint16_t dbi_idx;
    uint64_t key_seq;
};

std::vector<KeyPlan> build_plan(int tid, const ReadConfig& cfg) {
    std::mt19937_64 rng(cfg.base_seed + static_cast<uint64_t>(tid));
    std::vector<KeyPlan> plan;
    plan.resize(cfg.reads_per_thread);

    if (cfg.cross_dbi) {
        std::uniform_int_distribution<uint32_t> dbi_dist(0, DBI_COUNT - 1);
        for (size_t i = 0; i < cfg.reads_per_thread; ++i) {
            uint16_t d = static_cast<uint16_t>(dbi_dist(rng));
            uint64_t rc = SCHEMA[d].record_count;
            uint64_t k = std::uniform_int_distribution<uint64_t>(0, rc - 1)(rng);
            plan[i] = {d, k};
        }
    } else {
        uint16_t d = static_cast<uint16_t>(tid % static_cast<int>(DBI_COUNT));
        uint64_t rc = SCHEMA[d].record_count;
        std::uniform_int_distribution<uint64_t> key_dist(0, rc - 1);
        for (size_t i = 0; i < cfg.reads_per_thread; ++i) {
            plan[i] = {d, key_dist(rng)};
        }
    }
    return plan;
}

void check_rc(int rc, const char* op) {
    if (rc != MDBX_SUCCESS) {
        throw std::runtime_error(std::string(op) + " failed: " +
                                 mdbx_strerror(rc));
    }
}

struct ThreadOut {
    std::vector<uint64_t> latencies;
    uint64_t checksum = 0;
    std::chrono::steady_clock::time_point t_start{};
    std::chrono::steady_clock::time_point t_end{};
};

void thread_body(EnvHandle& h, int tid, const ReadConfig& cfg, ThreadOut& out) {
    auto plan = build_plan(tid, cfg);
    out.latencies.assign(cfg.reads_per_thread, 0);

    // Open one read-only txn per env that this thread will touch.
    // In `single` layout, all DBIs share env[0]; one txn suffices.
    // In `per_dbi` layout, we need a txn per env the thread touches.
    // We open lazily: a map from dbi_idx -> txn handle.
    std::vector<MDBX_txn*> txn_by_env(h.envs.size(), nullptr);

    auto get_txn = [&](size_t dbi_idx) -> MDBX_txn* {
        size_t env_idx = (h.cfg.layout == EnvLayout::PerDbi) ? dbi_idx : 0;
        if (!txn_by_env[env_idx]) {
            MDBX_env* env = h.envs[env_idx];
            MDBX_txn* t = nullptr;
            int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &t);
            check_rc(rc, "mdbx_txn_begin(RDONLY)");
            txn_by_env[env_idx] = t;
        }
        return txn_by_env[env_idx];
    };

    alignas(8) uint8_t key_buf[64];
    uint64_t checksum = 0;

    out.t_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < cfg.reads_per_thread; ++i) {
        const auto& p = plan[i];
        const auto& spec = SCHEMA[p.dbi_idx];
        if (spec.flags & MDBX_INTEGERKEY) {
            make_key_int(key_buf, spec.key_size, p.key_seq);
        } else {
            make_key_seq(key_buf, spec.key_size, p.key_seq);
        }
        MDBX_val k{key_buf, spec.key_size};
        MDBX_val v{nullptr, 0};
        MDBX_txn* txn = get_txn(p.dbi_idx);

        auto t0 = std::chrono::steady_clock::now();
        int rc = mdbx_get(txn, h.dbis[p.dbi_idx].dbi, &k, &v);
        auto t1 = std::chrono::steady_clock::now();

        if (rc != MDBX_SUCCESS) {
            throw std::runtime_error(
                std::string("mdbx_get failed (dbi=") + spec.name +
                " key_seq=" + std::to_string(p.key_seq) + "): " +
                mdbx_strerror(rc));
        }
        // XOR-fold value bytes into checksum (defeats DCE).
        const uint8_t* vb = static_cast<const uint8_t*>(v.iov_base);
        size_t n = v.iov_len;
        uint64_t acc = 0;
        size_t j = 0;
        for (; j + 8 <= n; j += 8) {
            uint64_t word;
            std::memcpy(&word, vb + j, 8);
            acc ^= word;
        }
        for (; j < n; ++j) acc ^= static_cast<uint64_t>(vb[j]) << ((j & 7) * 8);
        checksum ^= acc;

        out.latencies[i] =
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    out.t_end = std::chrono::steady_clock::now();
    out.checksum = checksum;

    for (auto* t : txn_by_env) {
        if (t) mdbx_txn_abort(t);
    }
}

uint64_t exact_percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    double idx = (p / 100.0) * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    if (lo >= sorted.size()) lo = sorted.size() - 1;
    return sorted[lo];
}

} // namespace

ReadResult run_read_bench(EnvHandle& h, const ReadConfig& cfg) {
    if (cfg.threads < 1) {
        throw std::runtime_error("run_read_bench: threads must be >= 1");
    }
    std::vector<ThreadOut> outs(cfg.threads);
    std::vector<std::thread> ts;
    ts.reserve(cfg.threads);
    for (int t = 0; t < cfg.threads; ++t) {
        ts.emplace_back([&, t]() { thread_body(h, t, cfg, outs[t]); });
    }
    for (auto& th : ts) th.join();

    size_t total = 0;
    for (auto& o : outs) total += o.latencies.size();
    std::vector<uint64_t> merged;
    merged.reserve(total);
    uint64_t checksum = 0;
    auto min_start = outs[0].t_start;
    auto max_end   = outs[0].t_end;
    for (auto& o : outs) {
        merged.insert(merged.end(), o.latencies.begin(), o.latencies.end());
        checksum ^= o.checksum;
        if (o.t_start < min_start) min_start = o.t_start;
        if (o.t_end   > max_end)   max_end   = o.t_end;
    }
    std::sort(merged.begin(), merged.end());

    uint64_t sum_ns = 0;
    for (auto v : merged) sum_ns += v;

    ReadResult r;
    r.total_reads    = total;
    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        max_end - min_start).count();
    r.wall_ms        = static_cast<double>(wall_ns) / 1.0e6;
    r.throughput_rps = (wall_ns > 0)
        ? (static_cast<double>(total) * 1e9 / static_cast<double>(wall_ns))
        : 0.0;
    r.p50_ns  = exact_percentile(merged, 50.0);
    r.p95_ns  = exact_percentile(merged, 95.0);
    r.p99_ns  = exact_percentile(merged, 99.0);
    r.mean_ns = total ? (sum_ns / total) : 0;
    r.min_ns  = merged.empty() ? 0 : merged.front();
    r.max_ns  = merged.empty() ? 0 : merged.back();
    r.checksum = checksum;
    return r;
}

void write_read_row(CsvWriter& csv, const EnvHandle&, const ReadConfig& cfg,
                    const ReadResult& r) {
    csv.write_row({
        std::to_string(cfg.threads),
        cfg.cross_dbi ? "1" : "0",
        std::to_string(cfg.reads_per_thread),
        std::to_string(cfg.rep),
        std::to_string(r.total_reads),
        std::to_string(r.wall_ms),
        std::to_string(r.throughput_rps),
        std::to_string(r.p50_ns),
        std::to_string(r.p95_ns),
        std::to_string(r.p99_ns),
        std::to_string(r.mean_ns),
        std::to_string(r.min_ns),
        std::to_string(r.max_ns),
        std::to_string(r.checksum),
    });
}
