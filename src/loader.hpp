#pragma once
#include <cstddef>
#include "env.hpp"

class CsvWriter;

struct LoadConfig {
    size_t batch_size = 10'000;
    bool   use_append = true;
    bool   use_cursor = true;
};

void run_bulk_load(EnvHandle& h, const LoadConfig& cfg, CsvWriter& csv,
                   CsvWriter* hist_csv = nullptr);
