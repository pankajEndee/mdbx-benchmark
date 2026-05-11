#include "csv.hpp"
#include "env.hpp"
#include "hot_cold.hpp"
#include "loader.hpp"
#include "reader.hpp"
#include "schema.hpp"
#include "stats.hpp"
#include "txn_bench.hpp"
#include "writer.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CliOptions {
    std::string db_path     = "./bench_db";
    std::string out_dir     = "./results";
    std::string run_id;
    size_t      map_size_gb = 64;
    int         writemap    = 1;
    int         nordahead   = 1;
    int         liforeclaim = 1;
    std::string sync_mode   = "safe_nosync";
    std::string layout      = "single"; // single|per_dbi
    std::string phase       = "all";
    std::optional<unsigned> threads;
    std::optional<size_t>   batch_size;
    std::optional<size_t>   ops;
    bool        no_load         = false;
    bool        dump_histograms = false;
};

void print_help() {
    std::cout <<
        "Usage: mdbx_bench [options]\n"
        "\n"
        "Options:\n"
        "  --db-path <path>          Default: ./bench_db\n"
        "  --out-dir <path>          Directory for per-phase CSV files. Default: ./results\n"
        "  --run-id <string>         Tag written into the run_id column of every row.\n"
        "                            Default: auto-generated from sync-mode + writemap + timestamp.\n"
        "  --map-size-gb <n>         Default: 64\n"
        "  --writemap <0|1>          Default: 1\n"
        "  --nordahead <0|1>         Default: 1\n"
        "  --liforeclaim <0|1>       Default: 1\n"
        "  --sync-mode <s>           default|safe_nosync|utterly_nosync  Default: safe_nosync\n"
        "  --layout <s>              single|per_dbi  Default: single\n"
        "                            per_dbi opens one MDBX env per DBI; map-size is split evenly.\n"
        "  --phase <name>            load|hot_read|cold_read|mixed|txn|hotcold|all  Default: all\n"
        "  --threads <n>             Reader thread count override\n"
        "  --batch-size <n>          Writer batch size override\n"
        "  --ops <n>                 Read ops per thread override\n"
        "  --no-load                 Skip bulk load (use existing DB)\n"
        "  --dump-histograms         Also emit <phase>_hist.csv with one row per histogram bucket\n"
        "  --help\n";
}

std::string next_arg(int& i, int argc, char** argv, const char* flag) {
    if (i + 1 >= argc) {
        std::cerr << "missing value for " << flag << "\n";
        std::exit(2);
    }
    return argv[++i];
}

CliOptions parse_args(int argc, char** argv) {
    CliOptions o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--db-path")        o.db_path = next_arg(i, argc, argv, "--db-path");
        else if (a == "--out-dir")        o.out_dir = next_arg(i, argc, argv, "--out-dir");
        else if (a == "--run-id")         o.run_id  = next_arg(i, argc, argv, "--run-id");
        else if (a == "--map-size-gb")    o.map_size_gb = std::stoull(next_arg(i, argc, argv, "--map-size-gb"));
        else if (a == "--writemap")       o.writemap = std::stoi(next_arg(i, argc, argv, "--writemap"));
        else if (a == "--nordahead")      o.nordahead = std::stoi(next_arg(i, argc, argv, "--nordahead"));
        else if (a == "--liforeclaim")    o.liforeclaim = std::stoi(next_arg(i, argc, argv, "--liforeclaim"));
        else if (a == "--sync-mode")      o.sync_mode = next_arg(i, argc, argv, "--sync-mode");
        else if (a == "--layout")         o.layout = next_arg(i, argc, argv, "--layout");
        else if (a == "--phase")          o.phase = next_arg(i, argc, argv, "--phase");
        else if (a == "--threads")        o.threads = static_cast<unsigned>(std::stoul(next_arg(i, argc, argv, "--threads")));
        else if (a == "--batch-size")     o.batch_size = std::stoull(next_arg(i, argc, argv, "--batch-size"));
        else if (a == "--ops")            o.ops = std::stoull(next_arg(i, argc, argv, "--ops"));
        else if (a == "--no-load")        o.no_load = true;
        else if (a == "--dump-histograms") o.dump_histograms = true;
        else if (a == "--help" || a == "-h") { print_help(); std::exit(0); }
        else {
            std::cerr << "unknown option: " << a << "\n";
            print_help();
            std::exit(2);
        }
    }
    if (o.layout != "single" && o.layout != "per_dbi") {
        std::cerr << "invalid --layout: " << o.layout
                  << " (expected single|per_dbi)\n";
        std::exit(2);
    }
    if (o.run_id.empty()) {
        std::ostringstream os;
        os << "sync_" << o.sync_mode << "__wm_" << o.writemap
           << "__layout_" << o.layout << "__"
           << std::chrono::system_clock::now().time_since_epoch().count();
        o.run_id = os.str();
    }
    return o;
}

EnvConfig make_env_cfg(const CliOptions& o) {
    EnvConfig cfg;
    cfg.path        = o.db_path;
    cfg.map_size    = o.map_size_gb * size_t(1024) * 1024 * 1024;
    cfg.writemap    = (o.writemap != 0);
    cfg.nordahead   = (o.nordahead != 0);
    cfg.liforeclaim = (o.liforeclaim != 0);
    cfg.sync_mode   = o.sync_mode;
    cfg.layout      = (o.layout == "per_dbi") ? EnvLayout::PerDbi : EnvLayout::Single;
    return cfg;
}

RunMeta make_run_meta(const CliOptions& o) {
    RunMeta m;
    m.run_id        = o.run_id;
    m.timestamp_iso = iso8601_utc_now();
    m.sync_mode     = o.sync_mode;
    m.writemap      = o.writemap;
    m.nordahead     = o.nordahead;
    m.liforeclaim   = o.liforeclaim;
    m.layout        = o.layout;
    return m;
}

std::unique_ptr<CsvWriter> maybe_hist(const CliOptions& o, const RunMeta& meta,
                                      const std::string& phase) {
    if (!o.dump_histograms) return nullptr;
    return std::make_unique<CsvWriter>(o.out_dir, phase + "_hist", meta);
}

void run_load_phase(EnvHandle& h, const CliOptions& o, const RunMeta& meta) {
    CsvWriter csv(o.out_dir, "load", meta);
    auto hist = maybe_hist(o, meta, "load");
    struct V { size_t batch; bool append; };
    std::vector<V> variants = {
        {1,      false},
        // {1000,   false},
        // {10000,  true},
        // {100000, true},
    };
    if (o.batch_size) variants = { {*o.batch_size, true} };
    for (auto& v : variants) {
        LoadConfig cfg;
        cfg.batch_size = v.batch;
        cfg.use_append = v.append;
        run_bulk_load(h, cfg, csv, hist.get());
    }
}

void run_read_phase(EnvHandle& h, const CliOptions& o, const RunMeta& meta,
                    const std::string& phase_name) {
    CsvWriter csv(o.out_dir, phase_name, meta);
    auto hist = maybe_hist(o, meta, phase_name);
    struct V { KeyPattern pat; unsigned threads; bool cross_dbi; };
    std::vector<V> variants = {
        {KeyPattern::Random,   1,  false},
        {KeyPattern::Random,   4,  false},
        {KeyPattern::Random,  16,  false},
        {KeyPattern::Zipfian,  8,  false},
        {KeyPattern::Random,   8,  true},
    };
    if (o.threads) {
        variants = { {KeyPattern::Random, *o.threads, false} };
    }
    for (auto& v : variants) {
        ReadConfig cfg;
        cfg.pattern        = v.pat;
        cfg.thread_count   = v.threads;
        cfg.cross_dbi      = v.cross_dbi;
        cfg.ops_per_thread = o.ops.value_or(1'000);
        run_point_reads(h, cfg, phase_name, csv, hist.get());
    }
}

void run_mixed_phase(EnvHandle& h, const CliOptions& o, const RunMeta& meta) {
    CsvWriter wcsv(o.out_dir, "mixed_writer", meta);
    CsvWriter rcsv(o.out_dir, "mixed_reader", meta);
    auto hist = maybe_hist(o, meta, "mixed");
    struct V { unsigned readers; unsigned wbatch; };
    std::vector<V> variants = {
        {0,  1},
        {4,  1},
        {8,  1},
        {16, 1},
        {8,   1},
        {8, 1},
    };
    if (o.threads || o.batch_size) {
        variants = { {o.threads.value_or(8), static_cast<unsigned>(o.batch_size.value_or(1000))} };
    }
    for (auto& v : variants) {
        MixedConfig cfg;
        cfg.reader_threads  = v.readers;
        cfg.writer_batch    = v.wbatch;
        cfg.write_txns      = 1000;
        cfg.cross_dbi_write = true;
        run_mixed(h, cfg, wcsv, rcsv, hist.get());
    }
}

void run_txn_phase(EnvHandle& h, const CliOptions& o, const RunMeta& meta) {
    CsvWriter csv(o.out_dir, "txn", meta);
    for (bool atomic : {true, false}) {
        TxnBenchConfig cfg;
        cfg.atomic    = atomic;
        cfg.txn_count = 1400 / DBI_COUNT; // keep total work bounded
        cfg.records_per_dbi_per_txn = 10;
        run_txn_bench(h, cfg, csv);
    }
}

void run_hotcold_phase(EnvHandle& h, const CliOptions& o, const RunMeta& meta) {
    CsvWriter csv(o.out_dir, "hotcold", meta);
    HotColdConfig cfg;
    run_hot_cold(h, cfg, csv);
}

} // namespace

int main(int argc, char** argv) try {
    CliOptions o = parse_args(argc, argv);
    EnvConfig env_cfg = make_env_cfg(o);
    RunMeta meta = make_run_meta(o);

    std::cerr << "[mdbx_bench] run_id=" << o.run_id
              << " phase=" << o.phase
              << " layout=" << o.layout
              << " db=" << o.db_path
              << " out=" << o.out_dir << "\n";

    EnvHandle h = open_env(env_cfg);
    print_env_info(h);

    auto run_one = [&](const std::string& p) {
        if (p == "load")       run_load_phase(h, o, meta);
        else if (p == "hot_read")  run_read_phase(h, o, meta, "hot_read");
        else if (p == "cold_read") run_read_phase(h, o, meta, "cold_read");
        else if (p == "mixed")     run_mixed_phase(h, o, meta);
        else if (p == "txn")       run_txn_phase(h, o, meta);
        else if (p == "hotcold")   run_hotcold_phase(h, o, meta);
        else {
            std::cerr << "unknown phase: " << p << "\n";
            std::exit(2);
        }
    };

    if (o.phase == "all") {
        if (!o.no_load) run_load_phase(h, o, meta);
        run_read_phase(h, o, meta, "hot_read");

        std::cerr << "\n=== Cold-read setup ===\n"
                     "To make the cold-read phase genuinely cold, run as root:\n"
                     "    sync && echo 3 > /proc/sys/vm/drop_caches\n"
                     "Press Enter to continue with cold reads...\n";
        std::string line;
        std::getline(std::cin, line);

        close_env(h);
        h = open_env(env_cfg);
        run_read_phase(h, o, meta, "cold_read");

        run_mixed_phase(h, o, meta);
        run_txn_phase(h, o, meta);
        run_hotcold_phase(h, o, meta);
    } else {
        if (o.phase != "load" && !o.no_load) {
            // If the user asked for a non-load phase but the DB might be empty,
            // do nothing here — they can pass --phase load separately.
        }
        run_one(o.phase);
    }

    print_env_info(h);
    close_env(h);
    return 0;
} catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
}
