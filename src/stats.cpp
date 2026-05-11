#include "stats.hpp"
#include "csv.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int floor_log2(uint64_t v) {
    if (v == 0) return 0;
    int n = 0;
    while (v > 1) { v >>= 1; ++n; }
    return n;
}

void Histogram::record(uint64_t ns) {
    int b = floor_log2(ns);
    if (b < 0) b = 0;
    if (b >= static_cast<int>(kBuckets)) b = kBuckets - 1;
    buckets[b]++;
    total_count++;
    total_ns += ns;
    if (ns < min_ns) min_ns = ns;
    if (ns > max_ns) max_ns = ns;
}

void Histogram::merge(const Histogram& other) {
    for (size_t i = 0; i < kBuckets; ++i) buckets[i] += other.buckets[i];
    total_count += other.total_count;
    total_ns    += other.total_ns;
    if (other.min_ns < min_ns) min_ns = other.min_ns;
    if (other.max_ns > max_ns) max_ns = other.max_ns;
}

uint64_t Histogram::percentile(double p) const {
    if (total_count == 0) return 0;
    double target = (p / 100.0) * static_cast<double>(total_count);
    uint64_t cum = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
        cum += buckets[i];
        if (static_cast<double>(cum) >= target) {
            // Return upper edge of bucket as conservative estimate.
            uint64_t lo = (i == 0) ? 0 : (uint64_t(1) << i);
            uint64_t hi = uint64_t(1) << (i + 1);
            (void)lo;
            return hi - 1;
        }
    }
    return max_ns;
}

double Histogram::mean_us() const {
    if (total_count == 0) return 0.0;
    return static_cast<double>(total_ns) / static_cast<double>(total_count) / 1000.0;
}

void Histogram::write_summary(CsvWriter&, const std::string&) const {
    // Phase writers call csv.write_row directly with the columns they need;
    // this helper is unused now that summary stats are inlined per-phase.
}

void Histogram::write_buckets(CsvWriter& w, const std::string& variant) const {
    w.write_raw_header({"variant", "bucket_lo_ns", "bucket_hi_ns", "count"});
    for (size_t i = 0; i < kBuckets; ++i) {
        if (buckets[i] == 0) continue;
        uint64_t lo = (i == 0) ? 0 : (uint64_t(1) << i);
        uint64_t hi = uint64_t(1) << (i + 1);
        w.write_raw_row({
            variant,
            std::to_string(lo),
            std::to_string(hi),
            std::to_string(buckets[i]),
        });
    }
}

size_t get_rss_kb() {
    std::ifstream in("/proc/self/status");
    if (!in) return 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream ss(line.substr(6));
            size_t kb = 0;
            ss >> kb;
            return kb;
        }
    }
    return 0;
}
