#pragma once
#include <cstddef>
#include "env.hpp"

class CsvWriter;

struct TxnBenchConfig {
    size_t txn_count               = 100'000;
    size_t records_per_dbi_per_txn = 10;
    bool   atomic                  = true;
};

void run_txn_bench(EnvHandle& h, const TxnBenchConfig& cfg, CsvWriter& csv);
