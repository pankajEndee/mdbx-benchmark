#include "csv.hpp"
#include "env.hpp"
#include "loader.hpp"
#include "reader.hpp"
#include "schema.hpp"
#include "stats.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CliOptions {
    std::string db_path     = "./bench_db";
    std::string out_dir     = "./results";
    std::string run_id;
    size_t      map_size_gb = 128;
    int         writemap    = 1;
    int         nordahead   = 1;
    int         liforeclaim = 1;
    std::string sync_mode   = "safe_nosync";
    std::string layout      = "single"; // single|per_dbi
    std::string phase       = "load";   // load|cold_reads|hot_reads
    std::optional<size_t>   batch_size;
    bool        dump_histograms = false;
    // Per-DBI map size overrides for per_dbi layout: "name:size_gb"
    std::vector<std::string> dbi_map_sizes;

    // Read-phase options
    std::vector<int>  threads_sweep   = {1, 4, 8, 16};
    std::vector<int>  cross_dbi_sweep = {0, 1}; // 0=false, 1=true
    size_t            reads_per_thread = 100'000;
    int               reps             = 3;
    uint64_t          base_seed        = 0xC0FFEEULL;
};

void print_help() {
    std::cout <<
        "Usage: mdbx_bench [options]\n"
        "\n"
        "Phases:\n"
        "  --phase load              Bulk-load DBIs (writes). Default.\n"
        "  --phase cold_reads        Drops VM caches before each config; appends to cold_reads.csv.\n"
        "                            REQUIRES root (or write access to /proc/sys/vm/drop_caches).\n"
        "  --phase hot_reads         Calls mdbx_env_warmup before each config; appends to hot_reads.csv.\n"
        "\n"
        "Common options:\n"
        "  --db-path <path>          Default: ./bench_db\n"
        "  --out-dir <path>          Directory for per-phase CSV files. Default: ./results\n"
        "  --run-id <string>         Tag written into the run_id column of every row.\n"
        "                            Default: auto-generated from sync-mode + writemap + timestamp.\n"
        "  --map-size-gb <n>         Default: 128\n"
        "  --dbi-map-size-gb <name:n>  Per-DBI map size override for per_dbi layout.\n"
        "                            Repeatable. E.g. --dbi-map-size-gb vectors:32\n"
        "  --writemap <0|1>          Default: 1\n"
        "  --nordahead <0|1>         Default: 1\n"
        "  --liforeclaim <0|1>       Default: 1\n"
        "  --sync-mode <s>           default|safe_nosync|utterly_nosync  Default: safe_nosync\n"
        "  --layout <s>              single|per_dbi  Default: single\n"
        "\n"
        "Load-phase options:\n"
        "  --batch-size <n>          Writer batch size override\n"
        "  --dump-histograms         Also emit load_hist.csv with one row per histogram bucket\n"
        "\n"
        "Read-phase options (cold_reads / hot_reads):\n"
        "  --threads <list>          Comma-separated thread counts. Default: 1,4,8,16\n"
        "  --cross-dbi <true|false|both>  Default: both\n"
        "  --reads-per-thread <n>    Default: 100000\n"
        "  --reps <n>                Repetitions per config. Default: 3\n"
        "  --seed <u64>              Base RNG seed (per-thread seed = base + tid). Default: 0xC0FFEE\n"
        "  --help\n";
}

std::string next_arg(int& i, int argc, char** argv, const char* flag) {
    if (i + 1 >= argc) {
        std::cerr << "missing value for " << flag << "\n";
        std::exit(2);
    }
    return argv[++i];
}

std::vector<int> parse_int_list(const std::string& s) {
    std::vector<int> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) { out.push_back(std::stoi(cur)); cur.clear(); }
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::stoi(cur));
    return out;
}

CliOptions parse_args(int argc, char** argv) {
    CliOptions o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--db-path")        o.db_path = next_arg(i, argc, argv, "--db-path");
        else if (a == "--out-dir")        o.out_dir = next_arg(i, argc, argv, "--out-dir");
        else if (a == "--run-id")         o.run_id  = next_arg(i, argc, argv, "--run-id");
        else if (a == "--map-size-gb")    o.map_size_gb = std::stoull(next_arg(i, argc, argv, "--map-size-gb"));
        else if (a == "--dbi-map-size-gb") o.dbi_map_sizes.push_back(next_arg(i, argc, argv, "--dbi-map-size-gb"));
        else if (a == "--writemap")       o.writemap = std::stoi(next_arg(i, argc, argv, "--writemap"));
        else if (a == "--nordahead")      o.nordahead = std::stoi(next_arg(i, argc, argv, "--nordahead"));
        else if (a == "--liforeclaim")    o.liforeclaim = std::stoi(next_arg(i, argc, argv, "--liforeclaim"));
        else if (a == "--sync-mode")      o.sync_mode = next_arg(i, argc, argv, "--sync-mode");
        else if (a == "--layout")         o.layout = next_arg(i, argc, argv, "--layout");
        else if (a == "--phase")          o.phase = next_arg(i, argc, argv, "--phase");
        else if (a == "--batch-size")     o.batch_size = std::stoull(next_arg(i, argc, argv, "--batch-size"));
        else if (a == "--dump-histograms") o.dump_histograms = true;
        else if (a == "--threads")        o.threads_sweep = parse_int_list(next_arg(i, argc, argv, "--threads"));
        else if (a == "--cross-dbi") {
            std::string v = next_arg(i, argc, argv, "--cross-dbi");
            if      (v == "true"  || v == "1") o.cross_dbi_sweep = {1};
            else if (v == "false" || v == "0") o.cross_dbi_sweep = {0};
            else if (v == "both")              o.cross_dbi_sweep = {0, 1};
            else { std::cerr << "invalid --cross-dbi: " << v << "\n"; std::exit(2); }
        }
        else if (a == "--reads-per-thread") o.reads_per_thread = std::stoull(next_arg(i, argc, argv, "--reads-per-thread"));
        else if (a == "--reps")           o.reps = std::stoi(next_arg(i, argc, argv, "--reps"));
        else if (a == "--seed")           o.base_seed = std::stoull(next_arg(i, argc, argv, "--seed"), nullptr, 0);
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
    if (o.phase != "load" && o.phase != "cold_reads" && o.phase != "hot_reads") {
        std::cerr << "invalid --phase: " << o.phase
                  << " (expected load|cold_reads|hot_reads)\n";
        std::exit(2);
    }
    if (o.run_id.empty()) {
        std::ostringstream os;
        os << "sync_" << o.sync_mode << "__wm_" << o.writemap
           << "__layout_" << o.layout << "__phase_" << o.phase << "__"
           << std::chrono::system_clock::now().time_since_epoch().count();
        o.run_id = os.str();
    }
    return o;
}

EnvConfig make_env_cfg(const CliOptions& o) {
    EnvConfig cfg;
    cfg.path        = o.db_path;
    cfg.map_size    = o.map_size_gb * size_t(1024) * 1024 * 1024;
    cfg.max_readers = 256; // Headroom for per_dbi x 16-thread sweeps.
    cfg.writemap    = (o.writemap != 0);
    cfg.nordahead   = (o.nordahead != 0);
    cfg.liforeclaim = (o.liforeclaim != 0);
    cfg.sync_mode   = o.sync_mode;
    cfg.layout      = (o.layout == "per_dbi") ? EnvLayout::PerDbi : EnvLayout::Single;
    for (const auto& spec : o.dbi_map_sizes) {
        auto colon = spec.find(':');
        if (colon == std::string::npos) {
            std::cerr << "invalid --dbi-map-size-gb value '" << spec
                      << "': expected name:size_gb\n";
            std::exit(2);
        }
        std::string name = spec.substr(0, colon);
        size_t gb = std::stoull(spec.substr(colon + 1));
        cfg.dbi_map_sizes[name] = gb * size_t(1024) * 1024 * 1024;
    }
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

void run_load_phase(EnvHandle& h, const CliOptions& o, const RunMeta& meta) {
    CsvWriter csv(o.out_dir, "load", meta);
    std::unique_ptr<CsvWriter> hist;
    if (o.dump_histograms) hist = std::make_unique<CsvWriter>(o.out_dir, "load_hist", meta);
    struct V { size_t batch; bool append; };
    std::vector<V> variants = {
        {10000, false}
    };
    if (o.batch_size) variants = { {*o.batch_size, true} };
    for (auto& v : variants) {
        LoadConfig cfg;
        cfg.batch_size = v.batch;
        cfg.use_append = v.append;
        run_bulk_load(h, cfg, csv, hist.get());
    }
}

// Drops the OS page cache. Throws (exits with clear message) on failure so
// the cold benchmark cannot silently produce hot-cache numbers.
void drop_caches_or_die() {
    std::fflush(nullptr);
    ::sync();
    FILE* f = std::fopen("/proc/sys/vm/drop_caches", "w");
    if (!f) {
        std::cerr << "FATAL: cannot open /proc/sys/vm/drop_caches for write. "
                  << "Run cold_reads phase as root (sudo).\n";
        std::exit(3);
    }
    if (std::fputs("3\n", f) == EOF) {
        std::fclose(f);
        std::cerr << "FATAL: failed to write to /proc/sys/vm/drop_caches.\n";
        std::exit(3);
    }
    std::fclose(f);
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

void warmup_envs_or_die(EnvHandle& h) {
    for (size_t i = 0; i < h.envs.size(); ++i) {
        // MDBX 0.13.x doesn't expose a `touch` flag; `force` is the equivalent
        // "load all pages into RAM" warmup. `oomsafe` makes the call skip
        // pages it can't safely load instead of risking OOM, which is the
        // correct behavior for a benchmark warmup.
        int rc = mdbx_env_warmup(h.envs[i],
                                 /*txn=*/nullptr,
                                 static_cast<MDBX_warmup_flags_t>(
                                     MDBX_warmup_force | MDBX_warmup_oomsafe),
                                 /*timeout_seconds_16dot16=*/0);
        if (rc != MDBX_SUCCESS) {
            throw std::runtime_error(std::string("mdbx_env_warmup failed: ") +
                                     mdbx_strerror(rc));
        }
    }
}

void print_progress(const std::string& phase, const std::string& layout,
                    const ReadConfig& cfg, const ReadResult& r) {
    std::cerr << "[" << phase << "] layout=" << layout
              << " threads=" << cfg.threads
              << " cross_dbi=" << (cfg.cross_dbi ? "1" : "0")
              << " rep=" << cfg.rep
              << " reads=" << r.total_reads
              << " wall=" << r.wall_ms << "ms"
              << " tput=" << static_cast<uint64_t>(r.throughput_rps) << "rps"
              << " p50=" << r.p50_ns << "ns"
              << " p99=" << r.p99_ns << "ns"
              << " ck=" << std::hex << r.checksum << std::dec
              << "\n";
}

void run_read_sweep(const CliOptions& o, const EnvConfig& env_cfg,
                    const RunMeta& meta) {
    const bool cold = (o.phase == "cold_reads");
    const std::string phase = o.phase;
    CsvWriter csv(o.out_dir, phase, meta);

    for (int threads : o.threads_sweep) {
        for (int cd : o.cross_dbi_sweep) {
            for (int rep = 0; rep < o.reps; ++rep) {
                if (cold) {
                    drop_caches_or_die();
                }
                EnvHandle h = open_env_readonly(env_cfg);
                if (!cold) {
                    warmup_envs_or_die(h);
                }
                ReadConfig rc;
                rc.threads          = threads;
                rc.cross_dbi        = (cd != 0);
                rc.reads_per_thread = o.reads_per_thread;
                rc.rep              = rep;
                rc.base_seed        = o.base_seed;

                ReadResult res = run_read_bench(h, rc);
                write_read_row(csv, h, rc, res);
                print_progress(phase, o.layout, rc, res);

                close_env(h);
            }
        }
    }
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

    if (o.phase == "load") {
        EnvHandle h = open_env(env_cfg);
        print_env_info(h);
        run_load_phase(h, o, meta);
        print_env_info(h);
        close_env(h);
    } else {
        // cold_reads / hot_reads — opens/closes envs internally per config.
        run_read_sweep(o, env_cfg, meta);
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
}
