#pragma once
#include <cstdint>
#include "env.hpp"

class CsvWriter;

struct ReadConfig {
    size_t   reads_per_thread = 100'000;
    int      threads          = 1;
    bool     cross_dbi        = false;
    int      rep              = 0;
    uint64_t base_seed        = 0xC0FFEEULL;
};

struct ReadResult {
    uint64_t total_reads     = 0;
    double   wall_ms         = 0.0;
    double   throughput_rps  = 0.0;
    uint64_t p50_ns          = 0;
    uint64_t p95_ns          = 0;
    uint64_t p99_ns          = 0;
    uint64_t mean_ns         = 0;
    uint64_t min_ns          = 0;
    uint64_t max_ns          = 0;
    uint64_t checksum        = 0;
};

ReadResult run_read_bench(EnvHandle& h, const ReadConfig& cfg);

void write_read_row(CsvWriter& csv, const EnvHandle& h, const ReadConfig& cfg,
                    const ReadResult& r);
