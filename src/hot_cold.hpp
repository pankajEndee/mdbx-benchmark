#pragma once
#include <cstddef>
#include <cstdint>
#include "env.hpp"

class CsvWriter;

struct HotColdConfig {
    uint64_t writer_txns           = 1'000;
    size_t   writer_batch          = 10;
    unsigned stale_reader_hold_ms  = 5000;
};

void run_hot_cold(EnvHandle& h, const HotColdConfig& cfg, CsvWriter& csv);
