#include "csv.hpp"
#include "env.hpp"
#include "loader.hpp"
#include "schema.hpp"
#include "stats.hpp"

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
    size_t      map_size_gb = 128;
    int         writemap    = 1;
    int         nordahead   = 1;
    int         liforeclaim = 1;
    std::string sync_mode   = "safe_nosync";
    std::string layout      = "single"; // single|per_dbi
    std::optional<size_t>   batch_size;
    bool        dump_histograms = false;
    // Per-DBI map size overrides for per_dbi layout: "name:size_gb"
    std::vector<std::string> dbi_map_sizes;
};

void print_help() {
    std::cout <<
        "Usage: mdbx_bench [options]\n"
        "\n"
        "Runs the bulk-load phase only.\n"
        "\n"
        "Options:\n"
        "  --db-path <path>          Default: ./bench_db\n"
        "  --out-dir <path>          Directory for per-phase CSV files. Default: ./results\n"
        "  --run-id <string>         Tag written into the run_id column of every row.\n"
        "                            Default: auto-generated from sync-mode + writemap + timestamp.\n"
        "  --map-size-gb <n>         Default: 128\n"
        "  --dbi-map-size-gb <name:n>  Per-DBI map size override for per_dbi layout.\n"
        "                            Repeatable. E.g. --dbi-map-size-gb vectors:32\n"
        "                            Unspecified DBIs fall back to even split of --map-size-gb.\n"
        "  --writemap <0|1>          Default: 1\n"
        "  --nordahead <0|1>         Default: 1\n"
        "  --liforeclaim <0|1>       Default: 1\n"
        "  --sync-mode <s>           default|safe_nosync|utterly_nosync  Default: safe_nosync\n"
        "  --layout <s>              single|per_dbi  Default: single\n"
        "                            per_dbi opens one MDBX env per DBI; map-size is split evenly.\n"
        "  --batch-size <n>          Writer batch size override\n"
        "  --dump-histograms         Also emit load_hist.csv with one row per histogram bucket\n"
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
        else if (a == "--dbi-map-size-gb") o.dbi_map_sizes.push_back(next_arg(i, argc, argv, "--dbi-map-size-gb"));
        else if (a == "--writemap")       o.writemap = std::stoi(next_arg(i, argc, argv, "--writemap"));
        else if (a == "--nordahead")      o.nordahead = std::stoi(next_arg(i, argc, argv, "--nordahead"));
        else if (a == "--liforeclaim")    o.liforeclaim = std::stoi(next_arg(i, argc, argv, "--liforeclaim"));
        else if (a == "--sync-mode")      o.sync_mode = next_arg(i, argc, argv, "--sync-mode");
        else if (a == "--layout")         o.layout = next_arg(i, argc, argv, "--layout");
        else if (a == "--batch-size")     o.batch_size = std::stoull(next_arg(i, argc, argv, "--batch-size"));
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

} // namespace

int main(int argc, char** argv) try {
    CliOptions o = parse_args(argc, argv);
    EnvConfig env_cfg = make_env_cfg(o);
    RunMeta meta = make_run_meta(o);

    std::cerr << "[mdbx_bench] run_id=" << o.run_id
              << " phase=load"
              << " layout=" << o.layout
              << " db=" << o.db_path
              << " out=" << o.out_dir << "\n";

    EnvHandle h = open_env(env_cfg);
    print_env_info(h);

    run_load_phase(h, o, meta);

    print_env_info(h);
    close_env(h);
    return 0;
} catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
}
