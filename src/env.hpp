#pragma once
#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include "mdbx.h++"
#include "schema.hpp"

enum class EnvLayout { Single, PerDbi };

inline std::string to_string(EnvLayout l) {
    return l == EnvLayout::PerDbi ? "per_dbi" : "single";
}

struct EnvConfig {
    std::string  path         = "./bench_db";
    size_t       map_size     = size_t(64) * 1024 * 1024 * 1024;
    unsigned     max_readers  = 128;
    unsigned     max_dbi      = 16;
    bool         writemap     = true;
    bool         nordahead    = true;
    bool         liforeclaim  = true;
    std::string  sync_mode    = "safe_nosync"; // default|safe_nosync|utterly_nosync
    EnvLayout    layout       = EnvLayout::Single;
    // Per-DBI map size overrides (per_dbi layout only). Key = DBI name, value = bytes.
    std::unordered_map<std::string, size_t> dbi_map_sizes;
};

struct EnvHandle {
    EnvConfig                              cfg;
    // Single-env mode: envs.size() == 1, all DBIs live in envs[0].
    // Per-DBI mode:    envs.size() == DBI_COUNT, dbis[i] lives in envs[i].
    std::vector<mdbx::env_managed>         envs;
    std::array<mdbx::map_handle, DBI_COUNT> dbis{};

    mdbx::env_managed& env_for(size_t dbi_idx) {
        return cfg.layout == EnvLayout::PerDbi ? envs[dbi_idx] : envs[0];
    }
    const mdbx::env_managed& env_for(size_t dbi_idx) const {
        return cfg.layout == EnvLayout::PerDbi ? envs[dbi_idx] : envs[0];
    }
};

EnvHandle open_env(const EnvConfig& cfg);
void      close_env(EnvHandle& h);
void      print_env_info(EnvHandle& h);
void      print_dbi_stats(EnvHandle& h);

// Total file size on disk in bytes, summed across all envs.
size_t    env_file_size_bytes(const EnvHandle& h);
