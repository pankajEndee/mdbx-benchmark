#include "env.hpp"
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>

static mdbx::env_managed::operate_parameters make_op_params(const EnvConfig& cfg,
                                                            unsigned max_maps) {
    mdbx::env_managed::operate_parameters op;
    op.max_maps    = max_maps;
    op.max_readers = cfg.max_readers;
    op.mode        = mdbx::env_managed::mode::write_mapped_io;
    if (!cfg.writemap) op.mode = mdbx::env_managed::mode::write_file_io;

    if (cfg.sync_mode == "default") {
        op.durability = mdbx::env_managed::durability::robust_synchronous;
    } else if (cfg.sync_mode == "safe_nosync") {
        op.durability = mdbx::env_managed::durability::lazy_weak_tail;
    } else if (cfg.sync_mode == "utterly_nosync") {
        op.durability = mdbx::env_managed::durability::whole_fragile;
    } else {
        op.durability = mdbx::env_managed::durability::robust_synchronous;
    }

    op.options.no_sticky_threads = false;
    op.options.nested_write_transactions = false;
    op.options.exclusive = false;
    op.options.disable_readahead = cfg.nordahead;
    op.options.disable_clear_memory = false;
    op.reclaiming.lifo = cfg.liforeclaim;
    op.reclaiming.coalesce = true;
    return op;
}

static mdbx::env_managed open_one(const std::string& path,
                                  const EnvConfig& cfg,
                                  size_t map_size,
                                  unsigned max_maps) {
    std::filesystem::create_directories(path);
    mdbx::env_managed::create_parameters cp;
    static constexpr size_t kMaxMapSize  = size_t(128) * 1024 * 1024 * 1024;
    static constexpr size_t kInitialSize = size_t(1)   * 1024 * 1024 * 1024;
    static constexpr size_t kGrowthStep  = size_t(1)   * 1024 * 1024 * 1024;
    size_t upper = std::min(map_size, kMaxMapSize);
    cp.geometry.make_dynamic(kInitialSize, upper);
    cp.geometry.size_lower = mdbx::env_managed::geometry::default_value;
    cp.geometry.growth_step = kGrowthStep;
    cp.geometry.pagesize = mdbx::env_managed::geometry::default_value;
    cp.use_subdirectory = true;
    auto op = make_op_params(cfg, max_maps);
    return mdbx::env_managed(path, cp, op);
}

EnvHandle open_env(const EnvConfig& cfg) {
    EnvHandle h;
    h.cfg = cfg;

    if (cfg.layout == EnvLayout::PerDbi) {
        // Default: split map_size evenly across envs (round up).
        // Per-DBI overrides in cfg.dbi_map_sizes take precedence.
        size_t per_env_map = (cfg.map_size + DBI_COUNT - 1) / DBI_COUNT;
        h.envs.reserve(DBI_COUNT);
        for (size_t i = 0; i < DBI_COUNT; ++i) {
            std::string sub = cfg.path + "/" + SCHEMA[i].name;
            auto it = cfg.dbi_map_sizes.find(SCHEMA[i].name);
            size_t this_map = (it != cfg.dbi_map_sizes.end()) ? it->second : per_env_map;
            h.envs.emplace_back(open_one(sub, cfg, this_map, /*max_maps=*/1));

            auto txn = h.envs.back().start_write();
            mdbx::key_mode km = mdbx::key_mode::usual;
            if (SCHEMA[i].flags & MDBX_INTEGERKEY) km = mdbx::key_mode::ordinal;
            mdbx::value_mode vm = mdbx::value_mode::single;
            if (SCHEMA[i].flags & MDBX_DUPSORT) vm = mdbx::value_mode::multi;
            h.dbis[i] = txn.create_map(SCHEMA[i].name, km, vm);
            txn.commit();
        }
    } else {
        h.envs.reserve(1);
        h.envs.emplace_back(open_one(cfg.path, cfg, cfg.map_size, cfg.max_dbi));

        auto txn = h.envs[0].start_write();
        for (size_t i = 0; i < DBI_COUNT; ++i) {
            mdbx::key_mode km = mdbx::key_mode::usual;
            if (SCHEMA[i].flags & MDBX_INTEGERKEY) km = mdbx::key_mode::ordinal;
            mdbx::value_mode vm = mdbx::value_mode::single;
            if (SCHEMA[i].flags & MDBX_DUPSORT) vm = mdbx::value_mode::multi;
            h.dbis[i] = txn.create_map(SCHEMA[i].name, km, vm);
        }
        txn.commit();
    }
    return h;
}

static mdbx::env_managed open_one_readonly(const std::string& path,
                                           const EnvConfig& cfg,
                                           unsigned max_maps) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("read-only open: path does not exist: " + path);
    }
    mdbx::env_managed::operate_parameters op;
    op.max_maps    = max_maps;
    op.max_readers = cfg.max_readers;
    op.mode        = mdbx::env_managed::mode::readonly;
    op.durability  = mdbx::env_managed::durability::robust_synchronous;
    op.options.no_sticky_threads     = true;
    op.options.nested_write_transactions = false;
    op.options.exclusive             = false;
    op.options.disable_readahead     = cfg.nordahead;
    op.options.disable_clear_memory  = false;
    op.reclaiming.lifo               = cfg.liforeclaim;
    op.reclaiming.coalesce           = true;
    return mdbx::env_managed(path, op, /*accede=*/true);
}

EnvHandle open_env_readonly(const EnvConfig& cfg) {
    EnvHandle h;
    h.cfg = cfg;

    if (cfg.layout == EnvLayout::PerDbi) {
        h.envs.reserve(DBI_COUNT);
        for (size_t i = 0; i < DBI_COUNT; ++i) {
            std::string sub = cfg.path + "/" + SCHEMA[i].name;
            h.envs.emplace_back(open_one_readonly(sub, cfg, /*max_maps=*/1));
            auto txn = h.envs.back().start_read();
            mdbx::key_mode km = mdbx::key_mode::usual;
            if (SCHEMA[i].flags & MDBX_INTEGERKEY) km = mdbx::key_mode::ordinal;
            mdbx::value_mode vm = mdbx::value_mode::single;
            if (SCHEMA[i].flags & MDBX_DUPSORT) vm = mdbx::value_mode::multi;
            h.dbis[i] = txn.open_map(SCHEMA[i].name, km, vm);
            txn.abort();
        }
    } else {
        h.envs.reserve(1);
        h.envs.emplace_back(open_one_readonly(cfg.path, cfg, cfg.max_dbi));
        auto txn = h.envs[0].start_read();
        for (size_t i = 0; i < DBI_COUNT; ++i) {
            mdbx::key_mode km = mdbx::key_mode::usual;
            if (SCHEMA[i].flags & MDBX_INTEGERKEY) km = mdbx::key_mode::ordinal;
            mdbx::value_mode vm = mdbx::value_mode::single;
            if (SCHEMA[i].flags & MDBX_DUPSORT) vm = mdbx::value_mode::multi;
            h.dbis[i] = txn.open_map(SCHEMA[i].name, km, vm);
        }
        txn.abort();
    }
    return h;
}

void close_env(EnvHandle& h) {
    for (auto& e : h.envs) e.close();
    h.envs.clear();
}

void print_env_info(EnvHandle& h) {
    for (size_t i = 0; i < h.envs.size(); ++i) {
        MDBX_envinfo info{};
        mdbx_env_info_ex(h.envs[i], nullptr, &info, sizeof(info));
        std::cerr << "[env";
        if (h.cfg.layout == EnvLayout::PerDbi) std::cerr << ":" << SCHEMA[i].name;
        std::cerr << "] map_size=" << info.mi_geo.upper
                  << " last_pgno=" << info.mi_last_pgno
                  << " recent_txnid=" << info.mi_recent_txnid
                  << " latter_reader_txnid=" << info.mi_latter_reader_txnid
                  << " num_readers=" << info.mi_numreaders
                  << "\n";
    }
}

void print_dbi_stats(EnvHandle& h) {
    for (size_t i = 0; i < DBI_COUNT; ++i) {
        auto& env = h.env_for(i);
        auto txn = env.start_read();
        MDBX_stat st{};
        mdbx_dbi_stat(txn, h.dbis[i].dbi, &st, sizeof(st));
        std::cerr << "[dbi:" << SCHEMA[i].name
                  << "] entries=" << st.ms_entries
                  << " depth=" << st.ms_depth
                  << " branch_pages=" << st.ms_branch_pages
                  << " leaf_pages=" << st.ms_leaf_pages
                  << " overflow_pages=" << st.ms_overflow_pages
                  << "\n";
        txn.abort();
    }
}

static size_t dir_or_file_size(const std::filesystem::path& p) {
    size_t total = 0;
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        for (auto& entry : std::filesystem::directory_iterator(p, ec)) {
            if (entry.is_regular_file(ec)) total += entry.file_size(ec);
            else if (entry.is_directory(ec)) total += dir_or_file_size(entry.path());
        }
    } else if (std::filesystem::is_regular_file(p, ec)) {
        total = std::filesystem::file_size(p, ec);
    }
    return total;
}

size_t env_file_size_bytes(const EnvHandle& h) {
    return dir_or_file_size(std::filesystem::path(h.cfg.path));
}
