// Shared plumbing for the benchmark suite (bench_*_test.cpp) and the
// objectives reproduction test: environment knobs, percentile and RSS helpers,
// the deterministic navigation sampler, a cached reference bundle, and the
// "restore service after a mutation" protocol.
//
// Output contract: every scenario prints `BENCH <scenario>.<metric>=<number>`
// lines (numbers only, units in the metric suffix: _ms, _us, _mib, _bytes,
// _per_s). scripts/bench_compare.py parses exactly those lines.
//
// Only public APIs are used (database_manager, game_store, game_writer,
// opening_stats::query, opening_tree, import_pipeline) so the suite keeps
// compiling and stays comparable when mutations become incremental.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <chesslib/board/board.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <unistd.h>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/types.hpp"
#include "motif/import/import_pipeline.hpp"
#include "motif/import/logger.hpp"
#include "motif/search/opening_stats.hpp"
#include "motif/search/opening_tree.hpp"
#include "test_helpers.hpp"

namespace motif_bench
{

namespace fs = std::filesystem;

inline constexpr std::size_t navigation_sample_size = 512;
inline constexpr std::uint64_t navigation_sample_seed = 42;
inline constexpr std::uint32_t navigation_max_ply = 20;
inline constexpr std::size_t start_warm_repeats = 30;
inline constexpr std::size_t warm_nav_sweeps = 3;
inline constexpr double bytes_per_mib = 1048576.0;
inline constexpr double kib_per_mib = 1024.0;
inline constexpr double ms_per_second = 1000.0;
inline constexpr double us_per_ms = 1000.0;
inline constexpr double median_quantile = 0.50;
inline constexpr double tail_quantile = 0.99;
inline constexpr std::int32_t manual_white_elo = 2400;
inline constexpr std::int32_t manual_black_elo = 2300;
inline constexpr std::string_view manual_game_key = "manual_game_id=";
inline constexpr std::string_view game_count_key = "game_count=";
inline constexpr std::string_view bundle_lock_name = ".motif.lock";

using clock_type = std::chrono::steady_clock;

// ---- Environment and paths ------------------------------------------------

inline auto env_string(char const* name, std::string fallback) -> std::string
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- test-only environment read
    auto const* const value = std::getenv(name);
    return value != nullptr ? std::string {value} : std::move(fallback);
}

inline auto env_size(char const* name, std::size_t const fallback) -> std::size_t
{
    auto const text = env_string(name, "");
    if (text.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::size_t>(std::stoull(text));
    } catch (...) {
        return fallback;
    }
}

// MOTIF_BENCH_PGN selects the corpus; the default is the 100k "quick" corpus.
inline auto corpus_path() -> fs::path
{
    auto const fallback = (fs::path {MOTIF_PROJECT_SOURCE_DIR} / "bench" / "data" / "twic-100k.pgn").string();
    return env_string("MOTIF_BENCH_PGN", fallback);
}

// Directory that holds the reusable reference bundle (see ensure_base_bundle).
inline auto cache_root() -> fs::path
{
    return env_string("MOTIF_BENCH_CACHE", (fs::temp_directory_path() / "motif-bench-cache").string());
}

// Scratch space for per-scenario bundles. Defaults to a subdirectory of the
// cache root so bundles can be renamed into the cache on the same filesystem.
inline auto work_root() -> fs::path
{
    return env_string("MOTIF_BENCH_WORK", (cache_root() / "work").string());
}

// Removed and recreated empty on construction, deleted on destruction.
class scoped_work_dir
{
  public:
    explicit scoped_work_dir(std::string_view const label)
        : path_ {work_root() / fmt::format("{}-{}", label, ::getpid())}
    {
        std::error_code remove_error;
        fs::remove_all(path_, remove_error);
        fs::create_directories(path_);
    }

    ~scoped_work_dir()
    {
        std::error_code remove_error;
        fs::remove_all(path_, remove_error);
    }

    scoped_work_dir(scoped_work_dir const&) = delete;
    auto operator=(scoped_work_dir const&) -> scoped_work_dir& = delete;
    scoped_work_dir(scoped_work_dir&&) = delete;
    auto operator=(scoped_work_dir&&) -> scoped_work_dir& = delete;

    [[nodiscard]] auto path() const -> fs::path const& { return path_; }

  private:
    fs::path path_;
};

// cp -r semantics for a bundle directory, leaving out the advisory lock file
// so the copy can be opened while the source is open elsewhere.
inline void copy_bundle(fs::path const& src, fs::path const& dst)
{
    fs::create_directories(dst);
    for (auto const& entry : fs::recursive_directory_iterator {src}) {
        if (entry.path().filename() == bundle_lock_name) {
            continue;
        }
        auto const target = dst / fs::relative(entry.path(), src);
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else if (entry.is_regular_file()) {
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
}

// ---- Statistics, timing, RSS ----------------------------------------------

inline auto percentile(std::vector<double> values, double const quantile) -> double
{
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    auto const index = std::min(values.size() - 1, static_cast<std::size_t>(static_cast<double>(values.size()) * quantile));
    return values[index];
}

inline auto ms_since(clock_type::time_point const begin) -> double
{
    return std::chrono::duration<double, std::milli>(clock_type::now() - begin).count();
}

inline auto us_since(clock_type::time_point const begin) -> double
{
    return std::chrono::duration<double, std::micro>(clock_type::now() - begin).count();
}

inline auto proc_status_mib(std::string_view const key) -> double
{
    std::ifstream status {"/proc/self/status"};
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with(key)) {
            return static_cast<double>(std::stoull(line.substr(key.size()))) / kib_per_mib;
        }
    }
    return 0.0;
}

// Process high-water resident set in MiB.
inline auto peak_rss_mib() -> double
{
    return proc_status_mib("VmHWM:");
}

inline auto current_rss_mib() -> double
{
    return proc_status_mib("VmRSS:");
}

// Resets VmHWM to the current RSS so several measurements in one process get
// their own peak. Silently a no-op where /proc/self/clear_refs is unavailable
// (the reported peak is then the process-wide peak so far).
inline void reset_peak_rss()
{
    std::ofstream clear_refs {"/proc/self/clear_refs"};
    if (clear_refs) {
        clear_refs << "5";
    }
}

// ---- BENCH output ---------------------------------------------------------

template<typename T>
void emit(std::string_view const scenario, std::string_view const metric, T const value)
{
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, "BENCH values are plain numbers; use emit_flag for booleans");
    fmt::print("BENCH {}.{}={}\n", scenario, metric, value);
    (void)std::fflush(stdout);
}

inline void emit_flag(std::string_view const scenario, std::string_view const metric, bool const value)
{
    emit(scenario, metric, value ? 1 : 0);
}

inline void emit_latency_us(std::string_view const scenario, std::string const& prefix, std::vector<double> const& samples_us)
{
    emit(scenario, prefix + "_p50_us", percentile(samples_us, median_quantile));
    emit(scenario, prefix + "_p99_us", percentile(samples_us, tail_quantile));
}

// Skips the calling test case unless benchmarks can run here: release-only, no
// sanitizers, corpus present. SKIP throws, so the caller never continues.
inline void skip_unless_benchmarkable()
{
#ifndef NDEBUG
    SKIP("benchmarks run only in release builds");
#else
    if (test_helpers::is_sanitized_build) {
        SKIP("benchmarks are skipped in sanitize builds");
    }
    if (!fs::exists(corpus_path())) {
        SKIP("PGN corpus not available");
    }
#endif
}

// ---- Bundle accounting ----------------------------------------------------

struct bundle_bytes
{
    std::uintmax_t games_db {};
    std::uintmax_t postings {};
    std::uintmax_t tree {};
    // Per-edit chain artifacts (PR 6): sparse metadata after-image runs /
    // checkpoint and signed tree-delta runs.
    std::uintmax_t metadata {};
    std::uintmax_t tree_patches {};
    std::uintmax_t other {};
    std::size_t postings_files {};
    std::size_t tree_files {};
    std::size_t metadata_files {};
    std::size_t tree_patch_files {};

    [[nodiscard]] auto total() const -> std::uintmax_t { return games_db + postings + tree + metadata + tree_patches + other; }
};

// Sizes by role rather than by file name: generation suffixes and (later)
// per-segment files would otherwise make metric names unstable.
inline auto measure_bundle(fs::path const& dir) -> bundle_bytes
{
    bundle_bytes bytes;
    for (auto const& entry : fs::directory_iterator {dir}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        auto const name = entry.path().filename().string();
        if (name == bundle_lock_name) {
            continue;
        }
        auto const size = entry.file_size();
        if (name.starts_with("games.db")) {
            bytes.games_db += size;
        } else if (name.starts_with("positions.postings")) {
            bytes.postings += size;
            ++bytes.postings_files;
        } else if (name.starts_with("positions.metadata")) {
            bytes.metadata += size;
            ++bytes.metadata_files;
        } else if (name.starts_with("opening_tree.idx")) {
            bytes.tree += size;
            ++bytes.tree_files;
        } else if (name.starts_with("opening_tree.patch")) {
            bytes.tree_patches += size;
            ++bytes.tree_patch_files;
        } else {
            bytes.other += size;
        }
    }
    return bytes;
}

// ---- Queries --------------------------------------------------------------

inline auto start_hash() -> motif::db::zobrist_hash
{
    return motif::db::zobrist_hash {chesslib::board {}.hash()};
}

// nullopt when the query fails (derived indexes unavailable).
inline auto time_query_us(motif::db::database_manager const& manager, motif::db::zobrist_hash const hash) -> std::optional<double>
{
    auto const begin = clock_type::now();
    auto stats = motif::search::opening_stats::query(manager, hash);
    auto const elapsed = us_since(begin);
    if (!stats.has_value()) {
        return std::nullopt;
    }
    return elapsed;
}

// One explorer navigation step: open a lazily expanded tree at the position
// and expand it (the production explorer path; prefetch depth 0 avoids the
// replay-based tree slice). nullopt on failure.
inline auto time_navigation_us(motif::db::database_manager const& manager, motif::db::zobrist_hash const hash) -> std::optional<double>
{
    auto const begin = clock_type::now();
    auto tree = motif::search::opening_tree::open(manager, hash, 0);
    if (!tree.has_value()) {
        return std::nullopt;
    }
    if (!motif::search::opening_tree::expand(manager, tree->root).has_value()) {
        return std::nullopt;
    }
    return us_since(begin);
}

// Positions reached after 1..navigation_max_ply plies of randomly chosen
// games, so the sample covers the shallow, popular positions an opening
// explorer visits. Seeded for repeatability; shared with the objectives test.
inline auto sample_navigation_hashes(motif::db::database_manager const& manager, motif::db::zobrist_hash const start)
    -> std::vector<motif::db::zobrist_hash>
{
    auto game_ids = manager.store().all_game_ids();
    REQUIRE(game_ids.has_value());

    // NOLINTNEXTLINE(cert-msc51-cpp,cert-msc32-c) -- the fixed seed is the point: every run samples the same positions.
    auto rng = std::mt19937_64 {navigation_sample_seed};
    std::ranges::shuffle(*game_ids, rng);
    game_ids->resize(std::min(game_ids->size(), navigation_sample_size * 4));

    auto contexts = manager.store().get_game_contexts(*game_ids);
    REQUIRE(contexts.has_value());

    std::unordered_set<std::uint64_t> seen;
    std::vector<motif::db::zobrist_hash> hashes;
    for (auto const& [game_key, context] : *contexts) {
        if (hashes.size() == navigation_sample_size) {
            break;
        }
        auto const plies = std::min<std::uint32_t>(static_cast<std::uint32_t>(context.moves.size()), navigation_max_ply);
        if (plies == 0) {
            continue;
        }
        auto const ply = static_cast<std::uint16_t>(1 + (rng() % plies));
        auto board = motif::chess::replay(context.moves, ply);
        if (!board) {
            continue;
        }
        auto const hash = motif::db::zobrist_hash {board->hash()};
        if (hash == start || !seen.insert(static_cast<std::uint64_t>(hash)).second) {
            continue;
        }
        hashes.push_back(hash);
    }
    return hashes;
}

// One pass over the sample; every query must succeed.
inline auto navigation_pass_us(motif::db::database_manager const& manager, std::vector<motif::db::zobrist_hash> const& sample)
    -> std::vector<double>
{
    std::vector<double> latencies;
    latencies.reserve(sample.size());
    for (auto const hash : sample) {
        auto const micros = time_navigation_us(manager, hash);
        REQUIRE(micros.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- REQUIREd just above
        latencies.push_back(*micros);
    }
    return latencies;
}

// Pooled latencies of several passes after the first (cache-warm).
inline auto warm_navigation_us(motif::db::database_manager const& manager, std::vector<motif::db::zobrist_hash> const& sample)
    -> std::vector<double>
{
    (void)navigation_pass_us(manager, sample);
    std::vector<double> pooled;
    for (std::size_t pass = 0; pass < warm_nav_sweeps; ++pass) {
        auto latencies = navigation_pass_us(manager, sample);
        pooled.insert(pooled.end(), latencies.begin(), latencies.end());
    }
    return pooled;
}

inline auto warm_start_us(motif::db::database_manager const& manager) -> std::vector<double>
{
    std::vector<double> latencies;
    for (std::size_t i = 0; i < start_warm_repeats; ++i) {
        auto const micros = time_query_us(manager, start_hash());
        REQUIRE(micros.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- REQUIREd just above
        latencies.push_back(*micros);
    }
    return latencies;
}

// Whether the opened bundle currently serves the opening tree: derived from the
// coverage check (the tree must cover the current mutation sequence), never from
// the mere presence of a descriptor, which a stale snapshot keeps referencing.
inline auto tree_in_service(motif::db::database_manager const& manager) -> bool
{
    auto const status = manager.derived_index_status();
    REQUIRE(status.has_value());
    return status->tree_usable;
}

// ---- Restoring service after a mutation -----------------------------------

struct readiness
{
    double ready_ms {};
    double rebuild_ms {};
    double rebuild_postings_ms {};
    double rebuild_tree_ms {};
    bool incremental {true};
};

// Every index-relevant canonical mutation advances the mutation sequence, so
// the previously published snapshot stops covering the bundle and queries fail
// until they are rebuilt. This function never assumes that: it probes the
// start-position query and derived_index_status() (never the mere presence of a
// manifest entry, which a stale snapshot keeps), calls the manager's rebuild
// entry points only when something is actually unusable, and reports whether it
// had to. After incremental maintenance lands it finds nothing to do and
// `incremental` is true.
//
// `tree_expected` records whether the bundle carried an opening tree index
// before the mutation; a tree that disappeared is rebuilt so the bundle
// returns to the same service level (millisecond start-position queries)
// rather than the postings-only fallback.
inline auto restore_service(motif::db::database_manager& manager, bool const tree_expected) -> readiness
{
    readiness report;
    auto const probe = time_query_us(manager, start_hash());
    if (!probe.has_value()) {
        report.incremental = false;
        auto const begin = clock_type::now();
        auto const rebuilt = manager.rebuild_position_postings();
        REQUIRE(rebuilt.has_value());
        report.rebuild_postings_ms = ms_since(begin);
    }
    // A descriptor that merely remains referenced after a mutation is not a
    // usable tree: only derived_index_status() says whether the tree covers the
    // current mutation sequence (a stale descriptor would otherwise falsely look
    // "present" and the benchmark would compare against the postings fallback).
    auto const status = manager.derived_index_status();
    REQUIRE(status.has_value());
    if (tree_expected && !status->tree_usable) {
        report.incremental = false;
        auto const begin = clock_type::now();
        auto const rebuilt = manager.rebuild_opening_tree_index();
        REQUIRE(rebuilt.has_value());
        report.rebuild_tree_ms = ms_since(begin);
    }
    report.rebuild_ms = report.rebuild_postings_ms + report.rebuild_tree_ms;
    // Whatever path got us here, the start position must now be answerable.
    REQUIRE(time_query_us(manager, start_hash()).has_value());
    return report;
}

// Wall time from `mutation_start` until the start-position query and a full
// pass of explorer navigation over the sample are answerable again.
inline auto await_ready(motif::db::database_manager& manager,
                        bool const tree_expected,
                        std::vector<motif::db::zobrist_hash> const& sample,
                        clock_type::time_point const mutation_start) -> readiness
{
    auto report = restore_service(manager, tree_expected);
    (void)navigation_pass_us(manager, sample);
    report.ready_ms = ms_since(mutation_start);
    return report;
}

inline void emit_readiness(std::string_view const scenario, std::string const& prefix, readiness const& report)
{
    emit(scenario, prefix + "ready_ms", report.ready_ms);
    emit(scenario, prefix + "rebuild_ms", report.rebuild_ms);
    emit(scenario, prefix + "rebuild_postings_ms", report.rebuild_postings_ms);
    emit(scenario, prefix + "rebuild_tree_ms", report.rebuild_tree_ms);
    emit_flag(scenario, prefix + "incremental", report.incremental);
}

// ---- Games ----------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a sample size and an RNG seed; every call site names both.
inline auto sample_game_ids(motif::db::database_manager const& manager, std::size_t const count, std::uint64_t const seed)
    -> std::vector<motif::db::game_id>
{
    auto game_ids = manager.store().all_game_ids();
    REQUIRE(game_ids.has_value());
    auto rng = std::mt19937_64 {seed};
    std::ranges::shuffle(*game_ids, rng);
    game_ids->resize(std::min(game_ids->size(), count));
    return std::move(*game_ids);
}

inline auto load_games(motif::db::database_manager const& manager, std::vector<motif::db::game_id> const& ids)
    -> std::vector<motif::db::game>
{
    std::vector<motif::db::game> games;
    games.reserve(ids.size());
    for (auto const identifier : ids) {
        auto loaded = manager.store().get(identifier);
        REQUIRE(loaded.has_value());
        games.push_back(std::move(*loaded));
    }
    return games;
}

// A copy of `source` whose event name is unique, so it never collides with a
// game already in the destination bundle on the identity key. The moves (and
// therefore the postings and tree contribution) stay realistic.
inline auto make_unique_game(motif::db::game source, std::string_view const tag, std::uint64_t const serial) -> motif::db::game
{
    auto name = fmt::format("motif-bench {} {}", tag, serial);
    if (source.event_details.has_value()) {
        source.event_details->name = std::move(name);
    } else {
        source.event_details = motif::db::event {.name = std::move(name), .site = std::nullopt, .date = std::nullopt};
    }
    source.provenance = motif::db::game_provenance {};
    return source;
}

// ---- Reference bundle -----------------------------------------------------

// The reference bundle is the corpus imported with postings and the opening
// tree, plus one user-added ("manual") game so the edit scenarios have a game
// they are allowed to patch and remove. It is built once per
// (MOTIF_BENCH_CACHE, corpus) and then only read or copied; never mutate it.
struct base_bundle
{
    fs::path dir;
    motif::db::game_id manual_game {};
    std::uint64_t game_count {};
};

inline auto base_root(fs::path const& pgn) -> fs::path
{
    std::error_code size_error;
    auto const size = fs::file_size(pgn, size_error);
    return cache_root() / fmt::format("{}-{}", pgn.stem().string(), size_error ? 0 : size);
}

inline auto load_base_bundle(fs::path const& pgn) -> std::optional<base_bundle>
{
    auto const root = base_root(pgn);
    std::ifstream ready {root / "READY"};
    if (!ready) {
        return std::nullopt;
    }
    base_bundle base;
    base.dir = root / "bundle";
    std::string line;
    while (std::getline(ready, line)) {
        if (line.starts_with(manual_game_key)) {
            base.manual_game = motif::db::game_id {static_cast<std::uint32_t>(std::stoul(line.substr(manual_game_key.size())))};
        } else if (line.starts_with(game_count_key)) {
            base.game_count = std::stoull(line.substr(game_count_key.size()));
        }
    }
    if (base.manual_game.value == 0 || !fs::exists(base.dir / "games.db")) {
        return std::nullopt;
    }
    return base;
}

// Import `pgn` into a new bundle at `bundle` with postings and the tree, then
// close it. Logging is initialised for the duration of the import.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- corpus, destination bundle and log directory; every call site names all three.
inline auto import_corpus(fs::path const& pgn, fs::path const& bundle, fs::path const& log_dir) -> motif::import::import_summary
{
    auto manager = motif::db::database_manager::create(bundle, "bench");
    REQUIRE(manager.has_value());
    auto init_log = motif::import::initialize_logging({.log_dir = log_dir});
    REQUIRE(init_log.has_value());

    motif::import::import_config config {};
    config.build_opening_tree_index_after_import = true;

    motif::import::import_pipeline pipeline {*manager};
    auto summary = pipeline.run(pgn, config);
    REQUIRE(summary.has_value());
    REQUIRE(summary->committed > 0);

    REQUIRE(motif::import::shutdown_logging().has_value());
    manager->close();
    return *summary;
}

// Inserts one user-added game cloned from the first game, so the edit scenarios
// have a manual game they are allowed to patch and remove.
inline auto add_manual_game(motif::db::database_manager& manager) -> motif::db::game_id
{
    auto ids = manager.store().all_game_ids();
    REQUIRE((ids.has_value() && !ids->empty()));
    auto donor = manager.store().get(ids->front());
    REQUIRE(donor.has_value());
    auto manual = make_unique_game(std::move(*donor), "manual", 0);
    manual.white.elo = manual_white_elo;
    manual.black.elo = manual_black_elo;
    auto inserted = manager.insert_game(manual);
    REQUIRE(inserted.has_value());
    REQUIRE(manager.set_manual_game_provenance(*inserted, std::string {"bench"}, "new").has_value());
    return *inserted;
}

// Replaces `root` with a directory holding `built` as its "bundle" child. A rename
// is tried first; across filesystems the bundle is copied and the source removed.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- source bundle and cache root; the only call site names both.
inline void move_bundle_into_cache(fs::path const& built, fs::path const& root)
{
    std::error_code remove_error;
    fs::remove_all(root, remove_error);
    fs::create_directories(root);
    std::error_code rename_error;
    fs::rename(built, root / "bundle", rename_error);
    if (rename_error) {
        copy_bundle(built, root / "bundle");
        fs::remove_all(built, rename_error);
    }
}

struct ready_record
{
    motif::db::game_id manual_game {};
    std::uint64_t game_count {};
    std::string corpus;
};

inline void write_ready_file(fs::path const& root, ready_record const& ready_info)
{
    std::ofstream ready {root / "READY"};
    ready << manual_game_key << ready_info.manual_game.value << "\n"
          << game_count_key << ready_info.game_count << "\n"
          << "corpus=" << ready_info.corpus << "\n";
}

// Adds the manual game, makes sure derived indexes are in place, and moves
// the bundle into the cache. `built` must be a closed, freshly imported bundle
// on the cache's filesystem.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- freshly built bundle and the corpus it came from; every call site names both.
inline auto install_base_bundle(fs::path const& built, fs::path const& pgn) -> base_bundle
{
    auto manager = motif::db::database_manager::open(built);
    REQUIRE(manager.has_value());
    bool const tree_expected = tree_in_service(*manager);

    auto const manual_game = add_manual_game(*manager);
    (void)restore_service(*manager, tree_expected);

    auto const count = manager->store().count_games();
    REQUIRE(count.has_value());
    manager->close();

    auto const root = base_root(pgn);
    move_bundle_into_cache(built, root);
    write_ready_file(
        root,
        ready_record {.manual_game = manual_game, .game_count = static_cast<std::uint64_t>(*count), .corpus = pgn.filename().string()});
    auto base = load_base_bundle(pgn);
    REQUIRE(base.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- REQUIREd just above
    return *base;
}

// The cached reference bundle, importing the corpus first when absent.
inline auto ensure_base_bundle() -> base_bundle
{
    auto const pgn = corpus_path();
    if (auto cached = load_base_bundle(pgn); cached.has_value()) {
        return *cached;
    }
    scoped_work_dir const work {"base"};
    (void)import_corpus(pgn, work.path() / "bundle", work.path() / "logs");
    return install_base_bundle(work.path() / "bundle", pgn);
}

}  // namespace motif_bench
