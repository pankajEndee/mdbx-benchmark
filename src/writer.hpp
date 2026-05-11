#pragma once
#include <cstddef>
#include "env.hpp"
#include "reader.hpp"

class CsvWriter;

struct MixedConfig {
    unsigned   reader_threads  = 8;
    unsigned   writer_batch    = 1'000;
    size_t     write_txns      = 10'000;
    bool       cross_dbi_write = true;
    KeyPattern read_pattern    = KeyPattern::Random;
};

void run_mixed(EnvHandle& h, const MixedConfig& cfg,
               CsvWriter& writer_csv, CsvWriter& reader_csv,
               CsvWriter* hist_csv = nullptr);
