#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

class CsvWriter;

struct Histogram {
    // Power-of-2 buckets in nanoseconds: bucket i covers [2^i, 2^(i+1)).
    static constexpr size_t kBuckets = 64;
    std::array<uint64_t, kBuckets> buckets{};
    uint64_t total_count = 0;
    uint64_t total_ns    = 0;
    uint64_t min_ns      = UINT64_MAX;
    uint64_t max_ns      = 0;

    void     record(uint64_t ns);
    void     merge(const Histogram& other);
    uint64_t percentile(double p) const; // p in [0,100]
    double   mean_us() const;
    double   p50_us() const { return percentile(50.0) / 1000.0; }
    double   p95_us() const { return percentile(95.0) / 1000.0; }
    double   p99_us() const { return percentile(99.0) / 1000.0; }
    void     write_summary(CsvWriter& w, const std::string& variant) const;
    void     write_buckets(CsvWriter& w, const std::string& variant) const;
};

struct Timer {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now();
    uint64_t elapsed_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count();
    }
    void reset() { start = Clock::now(); }
};

size_t get_rss_kb();
