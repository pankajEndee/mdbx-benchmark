#pragma once
#include <cstddef>
#include <string>
#include "env.hpp"

class CsvWriter;

enum class KeyPattern { Sequential, Random, Zipfian };

inline std::string to_string(KeyPattern p) {
    switch (p) {
        case KeyPattern::Sequential: return "sequential";
        case KeyPattern::Random:     return "random";
        case KeyPattern::Zipfian:    return "zipfian";
    }
    return "unknown";
}

struct ReadConfig {
    size_t      ops_per_thread = 1'000'000;
    KeyPattern  pattern        = KeyPattern::Random;
    unsigned    thread_count   = 1;
    bool        cross_dbi      = false;
};

// phase_name selects "hot_read" or "cold_read" output file (caller owns CsvWriter).
void run_point_reads(EnvHandle& h, const ReadConfig& cfg, const std::string& phase_name,
                     CsvWriter& csv, CsvWriter* hist_csv = nullptr);
