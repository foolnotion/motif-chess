#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "motif/db/database_manager.hpp"

#include <fmt/format.h>
#include <sqlite3.h>
#include <tl/expected.hpp>
#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

#include "motif/chess/chess.hpp"
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/position_postings.hpp"
#include "motif/db/schema.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

// ── Helpers
// ───────────────────────────────────────────────────────────────────

namespace
{
constexpr std::size_t checksum_buffer_bytes = 65'536;
constexpr auto bundle_lock_permissions = mode_t {0600};
#ifndef _WIN32
auto open_bundle_lock_file(std::filesystem::path const& path) noexcept -> int
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- POSIX open creates the advisory-lock file.
    return ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, bundle_lock_permissions);
}
#endif

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto open_sqlite(std::filesystem::path const& db_path) -> result<sqlite3*>
{
    sqlite3* conn = nullptr;
    int const ret = sqlite3_open(db_path.c_str(), &conn);
    if (ret != SQLITE_OK) {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        return tl::unexpected {error_code::io_failure};
    }
    // EXPERIMENT (not yet validated): bulk-import pragma tuning. synchronous=NORMAL
    // is safe under WAL (a crash loses at most the last unfsynced commits, never
    // corrupts the file); cache_size/mmap_size raise the working-set that can stay
    // resident as games.db grows into the multi-GB range; wal_autocheckpoint raised
    // from the 1000-page default reduces mid-import checkpoint stalls at the cost of
    // a larger WAL file between checkpoints.
    sqlite3_exec(conn, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "PRAGMA cache_size = -200000;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "PRAGMA mmap_size = 30000000000;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "PRAGMA wal_autocheckpoint = 10000;", nullptr, nullptr, nullptr);
    return conn;
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto enable_foreign_keys(sqlite3* conn) -> result<void>
{
    int const ret = sqlite3_exec(conn, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(conn, "PRAGMA foreign_keys;", -1, &raw, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> {raw, sqlite3_finalize};
    if (sqlite3_step(guard.get()) != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }
    if (sqlite3_column_int(guard.get(), 0) != 1) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto path_exists(std::filesystem::path const& path) -> result<bool>
{
    std::error_code fs_err;
    auto const exists = std::filesystem::exists(path, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }
    return exists;
}

auto file_checksum(std::filesystem::path const& path) -> result<std::pair<std::uint64_t, std::uint64_t>>
{
    std::error_code fs_err;
    auto const size = std::filesystem::file_size(path, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }
    auto input = std::ifstream {path, std::ios::binary};
    if (!input) {
        return tl::unexpected {error_code::io_failure};
    }
    constexpr auto fnv_offset_basis = std::uint64_t {14695981039346656037ULL};
    constexpr auto fnv_prime = std::uint64_t {1099511628211ULL};
    auto checksum = fnv_offset_basis;
    auto buffer = std::array<char, checksum_buffer_bytes> {};
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0) {
        for (auto const byte : std::span {buffer.data(), static_cast<std::size_t>(input.gcount())}) {
            checksum ^= static_cast<unsigned char>(byte);
            checksum *= fnv_prime;
        }
    }
    if (!input.eof()) {
        return tl::unexpected {error_code::io_failure};
    }
    return std::pair {size, checksum};
}

auto artifact_matches_manifest(std::filesystem::path const& dir, derived_index_manifest_entry const& entry) -> bool
{
    auto const checksum = file_checksum(dir / entry.filename);
    return checksum && checksum->first == entry.file_size && checksum->second == entry.checksum;
}

auto sync_file(std::filesystem::path const& path) -> result<void>
{
#ifdef _WIN32
    auto const handle = CreateFileW(path.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE || FlushFileBuffers(handle) == 0) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        return tl::unexpected {error_code::io_failure};
    }
    CloseHandle(handle);
#else
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- POSIX open is required for fsync durability.
    auto const file_descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_descriptor < 0 || ::fsync(file_descriptor) != 0) {
        if (file_descriptor >= 0) {
            ::close(file_descriptor);
        }
        return tl::unexpected {error_code::io_failure};
    }
    ::close(file_descriptor);
#endif
    return {};
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto narrow_elo(std::optional<std::int32_t> const& elo) -> result<std::optional<std::int16_t>>
{
    if (!elo.has_value()) {
        return std::optional<std::int16_t> {};
    }
    if (*elo < std::numeric_limits<std::int16_t>::min() || *elo > std::numeric_limits<std::int16_t>::max()) {
        return tl::unexpected {error_code::io_failure};
    }
    return std::optional<std::int16_t> {static_cast<std::int16_t>(*elo)};
}

struct elo_bucket_counts
{
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::uint32_t game_count {};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- occurrence filtering and zero-fill preserve SQL aggregation semantics.
auto postings_elo_distribution(game_store const& store,
                               position_postings const& postings,
                               zobrist_hash const hash,
                               std::vector<game_id> allowed_game_ids,
                               int const bucket_width) -> result<std::vector<elo_distribution_row>>
{
    auto matches = postings.occurrences(hash);
    if (!matches) {
        return tl::unexpected {matches.error()};
    }
    std::ranges::sort(allowed_game_ids);
    auto game_ids = std::vector<game_id> {};
    game_ids.reserve(matches->size());
    for (auto const& match : *matches) {
        if (allowed_game_ids.empty() || std::ranges::binary_search(allowed_game_ids, match.game_id)) {
            game_ids.push_back(match.game_id);
        }
    }
    std::ranges::sort(game_ids);
    auto const unique_end = std::ranges::unique(game_ids);
    game_ids.erase(unique_end.begin(), unique_end.end());
    auto contexts = store.get_game_contexts(game_ids);
    if (!contexts) {
        return tl::unexpected {contexts.error()};
    }

    auto buckets = std::map<std::pair<std::uint16_t, std::int32_t>, elo_bucket_counts> {};
    auto seen = std::set<std::pair<game_id, std::uint16_t>> {};
    for (auto const& match : *matches) {
        if ((!allowed_game_ids.empty() && !std::ranges::binary_search(allowed_game_ids, match.game_id)) || !match.white_elo
            || !match.black_elo)
        {
            continue;
        }
        auto const context = contexts->find(match.game_id);
        if (context == contexts->end() || match.ply >= context->second.moves.size()) {
            continue;
        }
        auto const encoded_move = context->second.moves[match.ply];
        if (!seen.emplace(match.game_id, encoded_move).second) {
            continue;
        }
        auto const average_elo = (static_cast<std::int32_t>(*match.white_elo) + static_cast<std::int32_t>(*match.black_elo)) / 2;
        auto const bucket = (average_elo / bucket_width) * bucket_width;
        auto& counts = buckets[{encoded_move, bucket}];
        if (match.result > 0) {
            ++counts.white_wins;
        } else if (match.result < 0) {
            ++counts.black_wins;
        } else {
            ++counts.draws;
        }
        ++counts.game_count;
    }

    auto ranges = std::map<std::uint16_t, std::pair<std::int32_t, std::int32_t>> {};
    for (auto const& [key, counts] : buckets) {
        auto& range = ranges.try_emplace(key.first, key.second, key.second).first->second;
        range.first = std::min(range.first, key.second);
        range.second = std::max(range.second, key.second);
    }
    auto rows = std::vector<elo_distribution_row> {};
    for (auto const& [encoded_move, range] : ranges) {
        for (auto bucket = range.first;; bucket += bucket_width) {
            auto const bucket_entry = buckets.find({encoded_move, bucket});
            auto const counts = bucket_entry == buckets.end() ? elo_bucket_counts {} : bucket_entry->second;
            rows.push_back(elo_distribution_row {.encoded_move = encoded_move,
                                                 .elo_bucket_floor = bucket,
                                                 .white_wins = counts.white_wins,
                                                 .draws = counts.draws,
                                                 .black_wins = counts.black_wins,
                                                 .game_count = counts.game_count});
            if (bucket == range.second) {
                break;
            }
        }
    }
    return rows;
}

struct filtered_edge_aggregate
{
    std::uint16_t root_ply {};
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::int64_t white_elo_sum {};
    std::uint32_t white_elo_count {};
    std::int64_t black_elo_sum {};
    std::uint32_t black_elo_count {};
    double weighted_contrib_sum {};
    double elo_weight_sum {};
    game_id eco_sample_min {};
    game_id eco_sample_max {};
};

// NOLINTBEGIN(readability-function-cognitive-complexity) -- two replay passes preserve separate direct-edge and child-visit dedup domains.
auto postings_filtered_opening_stats(game_store const& store,
                                     position_postings const& postings,
                                     zobrist_hash const root_hash,
                                     std::vector<game_id> allowed_game_ids,
                                     bool const global_child_frequency = false) -> result<std::vector<opening_stat_agg_row>>
{
    std::ranges::sort(allowed_game_ids);
    auto occurrences = postings.occurrences(root_hash);
    if (!occurrences) {
        return tl::unexpected {occurrences.error()};
    }
    auto contexts = store.get_game_contexts(allowed_game_ids);
    if (!contexts) {
        return tl::unexpected {contexts.error()};
    }

    using edge_key = std::pair<std::uint16_t, zobrist_hash>;

    struct direct_contribution
    {
        std::uint16_t root_ply {};
        position_match match;
    };

    auto direct_contributions = std::map<std::tuple<game_id, std::uint16_t, zobrist_hash>, direct_contribution> {};
    for (auto const& occurrence : *occurrences) {
        if (!std::ranges::binary_search(allowed_game_ids, occurrence.game_id)) {
            continue;
        }
        auto const context = contexts->find(occurrence.game_id);
        if (context == contexts->end() || occurrence.ply >= context->second.moves.size()) {
            continue;
        }
        auto child_board = motif::chess::replay(context->second.moves, static_cast<std::uint16_t>(occurrence.ply + 1U));
        if (!child_board) {
            return tl::unexpected {error_code::io_failure};
        }
        auto const encoded_move = context->second.moves[occurrence.ply];
        auto const child_hash = zobrist_hash {child_board->hash()};
        auto const key = std::tuple {occurrence.game_id, encoded_move, child_hash};
        auto [entry, inserted] =
            direct_contributions.try_emplace(key, direct_contribution {.root_ply = occurrence.ply, .match = occurrence});
        if (!inserted && occurrence.ply < entry->second.root_ply) {
            entry->second = direct_contribution {.root_ply = occurrence.ply, .match = occurrence};
        }
    }

    auto aggregates = std::map<edge_key, filtered_edge_aggregate> {};
    auto child_hashes = std::set<zobrist_hash> {};
    for (auto const& [key, contribution] : direct_contributions) {
        auto const& [game_key, encoded_move, child_hash] = key;
        auto const& occurrence = contribution.match;
        auto& aggregate = aggregates[{encoded_move, child_hash}];
        if (aggregate.frequency == 0U) {
            aggregate.root_ply = contribution.root_ply;
            aggregate.eco_sample_min = game_key;
            aggregate.eco_sample_max = game_key;
        }
        aggregate.root_ply = std::min(aggregate.root_ply, contribution.root_ply);
        aggregate.eco_sample_min = std::min(aggregate.eco_sample_min, game_key);
        aggregate.eco_sample_max = std::max(aggregate.eco_sample_max, game_key);
        if (occurrence.result > 0) {
            ++aggregate.white_wins;
        } else if (occurrence.result < 0) {
            ++aggregate.black_wins;
        } else {
            ++aggregate.draws;
        }
        if (occurrence.white_elo) {
            aggregate.white_elo_sum += *occurrence.white_elo;
            ++aggregate.white_elo_count;
        }
        if (occurrence.black_elo) {
            aggregate.black_elo_sum += *occurrence.black_elo;
            ++aggregate.black_elo_count;
        }
        if (occurrence.white_elo && occurrence.black_elo) {
            auto const average_elo = (static_cast<double>(*occurrence.white_elo) + static_cast<double>(*occurrence.black_elo)) / 2.0;
            aggregate.weighted_contrib_sum += static_cast<double>(occurrence.result) * average_elo;
            aggregate.elo_weight_sum += average_elo;
        }
        ++aggregate.frequency;
        child_hashes.insert(child_hash);
    }

    auto child_game_counts = std::map<zobrist_hash, std::uint32_t> {};
    if (global_child_frequency) {
        for (auto const child_hash : child_hashes) {
            auto summary = postings.summary(child_hash);
            if (!summary) {
                return tl::unexpected {summary.error()};
            }
            child_game_counts[child_hash] = summary->value_or(position_postings_summary {}).distinct_game_count;
        }
    } else {
        for (auto const& [game_key, context] : *contexts) {
            auto board = motif::chess::board {};
            auto visited = std::set<zobrist_hash> {zobrist_hash {board.hash()}};
            for (auto const encoded_move : context.moves) {
                motif::chess::apply_encoded_move(board, encoded_move);
                visited.insert(zobrist_hash {board.hash()});
            }
            for (auto const child_hash : child_hashes) {
                if (visited.contains(child_hash)) {
                    ++child_game_counts[child_hash];
                }
            }
        }
    }

    auto rows = std::vector<opening_stat_agg_row> {};
    rows.reserve(aggregates.size());
    for (auto const& [key, aggregate] : aggregates) {
        rows.push_back(
            opening_stat_agg_row {.cont_encoded_move = key.first,
                                  .cont_hash = key.second,
                                  .root_ply = aggregate.root_ply,
                                  .frequency = aggregate.frequency,
                                  .transposition_frequency = child_game_counts[key.second],
                                  .white_wins = aggregate.white_wins,
                                  .draws = aggregate.draws,
                                  .black_wins = aggregate.black_wins,
                                  .avg_white_elo = aggregate.white_elo_count == 0U
                                      ? std::optional<double> {}
                                      : std::optional<double> {static_cast<double>(aggregate.white_elo_sum) / aggregate.white_elo_count},
                                  .avg_black_elo = aggregate.black_elo_count == 0U
                                      ? std::optional<double> {}
                                      : std::optional<double> {static_cast<double>(aggregate.black_elo_sum) / aggregate.black_elo_count},
                                  .eco_sample_min = aggregate.eco_sample_min,
                                  .eco_sample_max = aggregate.eco_sample_max,
                                  .elo_weighted_score = aggregate.elo_weight_sum == 0.0
                                      ? std::optional<double> {}
                                      : std::optional<double> {aggregate.weighted_contrib_sum / aggregate.elo_weight_sum}});
    }
    return rows;
}

// NOLINTEND(readability-function-cognitive-complexity)

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto cleanup_failed_create(std::filesystem::path const& dir,
                           std::filesystem::path const& db_path,
                           std::filesystem::path const& manifest_path,
                           bool created_dir) noexcept -> void
{
    std::error_code fs_err;
    std::filesystem::remove(manifest_path, fs_err);
    fs_err.clear();
    std::filesystem::remove(db_path, fs_err);
    fs_err.clear();
    std::filesystem::remove(db_path.string() + "-wal", fs_err);
    fs_err.clear();
    std::filesystem::remove(db_path.string() + "-shm", fs_err);
    if (created_dir) {
        fs_err.clear();
        std::filesystem::remove(dir, fs_err);
    }
}

}  // namespace

class bundle_lock
{
  public:
    explicit bundle_lock(std::filesystem::path const& path) noexcept
#ifndef _WIN32
        : descriptor_ {open_bundle_lock_file(path)}
#endif
    {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return;
        }
        OVERLAPPED overlapped {};
        if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped) == 0) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return;
        }
#else
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- POSIX advisory lock coordinates bundle writers.
        // descriptor_ is opened in the initializer so ownership is valid for every return path.
        if (descriptor_ < 0) {
            return;
        }
        struct flock lock {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0, .l_pid = 0};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- fcntl is the portable POSIX advisory-lock API.
        if (::fcntl(descriptor_, F_SETLK, &lock) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            return;
        }
#endif
        locked_ = true;
    }

    ~bundle_lock()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped {};
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            struct flock lock {.l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0, .l_pid = 0};
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- fcntl is the portable POSIX advisory-lock API.
            static_cast<void>(::fcntl(descriptor_, F_SETLK, &lock));
            ::close(descriptor_);
        }
#endif
    }

    bundle_lock(bundle_lock const&) = delete;
    auto operator=(bundle_lock const&) -> bundle_lock& = delete;
    bundle_lock(bundle_lock&&) = delete;
    auto operator=(bundle_lock&&) -> bundle_lock& = delete;

    [[nodiscard]] auto locked() const noexcept -> bool { return locked_; }

  private:
    bool locked_ {false};
#ifdef _WIN32
    HANDLE handle_ {INVALID_HANDLE_VALUE};
#else
    int descriptor_ {-1};
#endif
};

namespace
{
auto acquire_bundle_lock(std::filesystem::path const& dir) -> result<std::unique_ptr<bundle_lock>>
{
    auto lock = std::make_unique<bundle_lock>(dir / ".motif.lock");
    if (!lock->locked()) {
        return tl::unexpected {error_code::io_failure};
    }
    return lock;
}

}  // namespace

// ── Lifecycle
// ─────────────────────────────────────────────────────────────────

database_manager::~database_manager()
{
    close();
}

database_manager::database_manager(database_manager&& other) noexcept
    : conn_ {std::exchange(other.conn_, nullptr)}
    , store_ {std::move(other.store_)}
    , writer_ {std::move(other.writer_)}
    , manifest_ {std::move(other.manifest_)}
    , dir_ {std::move(other.dir_)}
    , position_postings_ {std::move(other.position_postings_)}
    , opening_tree_index_ {std::move(other.opening_tree_index_)}
    , is_scratch_ {std::exchange(other.is_scratch_, false)}
    , bundle_lock_ {std::move(other.bundle_lock_)}
{
    other.store_.reset();
    other.writer_.reset();
    other.position_postings_.reset();
    other.opening_tree_index_.reset();
}

auto database_manager::operator=(database_manager&& other) noexcept -> database_manager&
{
    if (this != &other) {
        close();
        conn_ = std::exchange(other.conn_, nullptr);
        store_ = std::move(other.store_);
        writer_ = std::move(other.writer_);
        manifest_ = std::move(other.manifest_);
        dir_ = std::move(other.dir_);
        position_postings_ = std::move(other.position_postings_);
        opening_tree_index_ = std::move(other.opening_tree_index_);
        is_scratch_ = std::exchange(other.is_scratch_, false);
        bundle_lock_ = std::move(other.bundle_lock_);
        other.store_.reset();
        other.writer_.reset();
        other.position_postings_.reset();
        other.opening_tree_index_.reset();
    }
    return *this;
}

void database_manager::close() noexcept
{
    auto const lock = std::scoped_lock {generation_mutex_};
    // Refresh game_count and mark clean before tearing down connections.
    // Best-effort — errors are swallowed since close() is noexcept.
    if (!dir_.empty() && store_ && conn_ != nullptr) {
        if (auto count_res = store_->count_games(); count_res) {
            manifest_.game_count = static_cast<std::uint64_t>(*count_res);
        }
        // A clean close is recoverable when checksum-verified postings cover
        // canonical SQLite.
        if (postings_cover_canonical_games()) {
            manifest_.position_index_dirty = false;
        }
        (void)write_manifest(dir_ / "manifest.json", manifest_);
    }

    position_postings_.reset();
    opening_tree_index_.reset();
    writer_.reset();
    store_.reset();
    if (conn_ != nullptr) {
        sqlite3_close(conn_);
        conn_ = nullptr;
    }
    bundle_lock_.reset();

    if (is_scratch_ && !dir_.empty()) {
        std::error_code fs_err;
        std::filesystem::remove_all(dir_, fs_err);
    }
}

// ── Factory methods
// ───────────────────────────────────────────────────────────

auto database_manager::create(std::filesystem::path const& dir, std::string const& name) -> result<database_manager>
{
    auto const db_path = dir / "games.db";
    auto const manifest_path = dir / "manifest.json";

    std::error_code fs_err;
    auto const created_dir = std::filesystem::create_directories(dir, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }

    auto bundle_lock = acquire_bundle_lock(dir);
    if (!bundle_lock) {
        if (created_dir) {
            fs_err.clear();
            std::filesystem::remove(dir, fs_err);
        }
        return tl::unexpected {bundle_lock.error()};
    }

    auto db_exists = path_exists(db_path);
    if (!db_exists || *db_exists) {
        if (created_dir) {
            fs_err.clear();
            std::filesystem::remove(dir, fs_err);
        }
        return tl::unexpected {db_exists ? error {error_code::io_failure} : db_exists.error()};
    }

    auto conn_res = open_sqlite(db_path);
    if (!conn_res) {
        if (created_dir) {
            fs_err.clear();
            std::filesystem::remove(dir, fs_err);
        }
        return tl::unexpected {conn_res.error()};
    }
    sqlite3* conn = *conn_res;

    auto init_res = schema::initialize(conn);
    if (!init_res) {
        sqlite3_close(conn);
        cleanup_failed_create(dir, db_path, manifest_path, created_dir);
        return tl::unexpected {init_res.error()};
    }

    auto new_manifest = make_manifest(name);
    auto write_res = write_manifest(manifest_path, new_manifest);
    if (!write_res) {
        sqlite3_close(conn);
        cleanup_failed_create(dir, db_path, manifest_path, created_dir);
        return tl::unexpected {write_res.error()};
    }

    database_manager mgr;
    mgr.conn_ = conn;
    mgr.store_.emplace(conn);
    mgr.writer_.emplace(conn);
    mgr.manifest_ = std::move(new_manifest);
    mgr.dir_ = dir;
    mgr.bundle_lock_ = std::move(*bundle_lock);

    // Publish a trivial empty (0-game) postings generation immediately so
    // postings_cover_canonical_games() is trivially true for the lifetime of
    // an unimported bundle. The first import republishes the real
    // generation once games exist (see rebuild_position_postings()).
    if (auto postings_res = mgr.rebuild_position_postings(); !postings_res) {
        mgr.close();
        cleanup_failed_create(dir, db_path, manifest_path, created_dir);
        return tl::unexpected {postings_res.error()};
    }

    // Mark in-use so that a crash before the first close() triggers a rebuild.
    mgr.manifest_.position_index_dirty = true;
    if (auto dirty_write_res = write_manifest(manifest_path, mgr.manifest_); !dirty_write_res) {
        mgr.close();
        cleanup_failed_create(dir, db_path, manifest_path, created_dir);
        return tl::unexpected {dirty_write_res.error()};
    }

    return mgr;
}

auto database_manager::open(std::filesystem::path const& dir) -> result<database_manager>
{
    auto const db_path = dir / "games.db";
    auto const manifest_path = dir / "manifest.json";

    auto db_exists = path_exists(db_path);
    if (!db_exists) {
        return tl::unexpected {db_exists.error()};
    }
    if (!*db_exists) {
        return tl::unexpected {error_code::not_found};
    }
    auto manifest_exists = path_exists(manifest_path);
    if (!manifest_exists) {
        return tl::unexpected {manifest_exists.error()};
    }
    if (!*manifest_exists) {
        return tl::unexpected {error_code::not_found};
    }

    auto bundle_lock = acquire_bundle_lock(dir);
    if (!bundle_lock) {
        return tl::unexpected {bundle_lock.error()};
    }

    auto conn_res = open_sqlite(db_path);
    if (!conn_res) {
        return tl::unexpected {conn_res.error()};
    }
    sqlite3* conn = *conn_res;

    auto fk_res = enable_foreign_keys(conn);
    if (!fk_res) {
        sqlite3_close(conn);
        return tl::unexpected {fk_res.error()};
    }

    auto ver_res = schema::version(conn);
    if (!ver_res) {
        sqlite3_close(conn);
        return tl::unexpected {ver_res.error()};
    }
    if (*ver_res > schema::current_version) {
        sqlite3_close(conn);
        return tl::unexpected {error_code::schema_mismatch};
    }
    if (*ver_res < schema::current_version) {
        auto mig_res = schema::migrate(conn, *ver_res);
        if (!mig_res) {
            sqlite3_close(conn);
            return tl::unexpected {mig_res.error()};
        }
    }

    auto mf_res = read_manifest(manifest_path);
    if (!mf_res) {
        sqlite3_close(conn);
        return tl::unexpected {mf_res.error()};
    }

    database_manager mgr;
    mgr.conn_ = conn;
    mgr.store_.emplace(conn);
    mgr.writer_.emplace(conn);
    mgr.manifest_ = std::move(*mf_res);
    mgr.dir_ = dir;
    mgr.bundle_lock_ = std::move(*bundle_lock);

    // Load checksum-verified immutable derived indexes before deciding
    // whether a rebuild is needed at all.
    if (mgr.manifest_.position_postings && artifact_matches_manifest(dir, *mgr.manifest_.position_postings)) {
        auto postings = position_postings {dir / mgr.manifest_.position_postings->filename};
        if (postings.open()) {
            mgr.position_postings_ = std::move(postings);
        }
    }
    if (mgr.manifest_.opening_tree_index && artifact_matches_manifest(dir, *mgr.manifest_.opening_tree_index)) {
        auto opening_index = opening_tree_index::open(dir / mgr.manifest_.opening_tree_index->filename);
        if (opening_index) {
            mgr.opening_tree_index_ = std::move(*opening_index);
        }
    }

    // Missing, corrupt, or predates-postings (one-release migration case: a
    // legacy bundle with games.db plus a leftover positions.duckdb and no
    // postings): rebuild directly from canonical SQLite. positions.duckdb,
    // if present, is never read or deleted by this or any other path -- it
    // is simply ignored. Rebuild failure fails open() closed; the attempt is
    // safe to retry on a later open() because it only ever writes to a
    // staging file that has never been published (see
    // rebuild_position_postings()).
    if (!mgr.postings_cover_canonical_games()) {
        if (auto rebuild_res = mgr.rebuild_position_postings(); !rebuild_res) {
            mgr.close();
            return tl::unexpected {rebuild_res.error()};
        }
    }

    // Mark in-use: a crash before close() will leave this set and trigger
    // a rebuild on the next open().
    mgr.manifest_.position_index_dirty = true;
    if (auto write_res = write_manifest(manifest_path, mgr.manifest_); !write_res) {
        mgr.close();
        return tl::unexpected {write_res.error()};
    }

    return mgr;
}

auto database_manager::create_scratch() -> result<database_manager>
{
    // static: distinguishes create_scratch() calls that land in the same
    // steady_clock tick (e.g. back-to-back calls in a tight test loop).
    static std::atomic<std::uint64_t> scratch_sequence {0};
    auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto const sequence = scratch_sequence.fetch_add(1, std::memory_order_relaxed);
    auto const scratch_dir = std::filesystem::temp_directory_path() / fmt::format("motif_scratch_{}_{}", tick, sequence);

    auto created = create(scratch_dir, "scratch");
    if (!created) {
        std::error_code fs_err;
        std::filesystem::remove_all(scratch_dir, fs_err);
        return created;
    }
    created->is_scratch_ = true;
    return created;
}

// ── Accessors
// ─────────────────────────────────────────────────────────────────

auto database_manager::store() const noexcept -> game_store const&
{
    assert(store_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return *store_;
}

auto database_manager::writer() noexcept -> game_writer&
{
    assert(writer_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return *writer_;
}

auto database_manager::insert_game(game const& src_game) -> result<game_id>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    if (auto stale_res = prepare_canonical_mutation(); !stale_res) {
        return tl::unexpected {stale_res.error()};
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store_ presence is checked at function entry.
    return store_->insert(src_game);
}

auto database_manager::set_manual_game_provenance(game_id const game_key,
                                                  std::optional<std::string> const& source_label,
                                                  std::string const& review_status) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    return store_->set_manual_provenance(game_key, source_label, review_status);
}

auto database_manager::manifest() const noexcept -> db_manifest const&
{
    return manifest_;
}

auto database_manager::dir() const noexcept -> std::filesystem::path const&
{
    return dir_;
}

auto database_manager::has_valid_derived_index(derived_index_manifest_entry const& entry) const -> bool
{
    // Immutable generation-qualified artifacts are checksummed before their
    // reader is installed. Query paths only need to verify source identity;
    // rescanning a multi-gigabyte file per lookup defeats the index.
    return !dir_.empty() && entry.source_generation == manifest_.source_generation;
}

// Caller must hold generation_mutex_ (see database_manager.hpp).
auto database_manager::postings_cover_canonical_games() const -> bool
{
    if (!position_postings_ || !manifest_.position_postings || !has_valid_derived_index(*manifest_.position_postings)) {
        return false;
    }
    // store_ is initialized by every successful factory.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto const game_count = store_->count_games();
    return game_count && *game_count >= 0 && static_cast<std::uint64_t>(*game_count) == position_postings_->indexed_game_count();
}

auto database_manager::mark_derived_indexes_stale() -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (dir_.empty()) {
        return {};
    }
    auto const stale_postings = manifest_.position_postings;
    auto const stale_tree = manifest_.opening_tree_index;
    auto candidate = manifest_;
    ++candidate.source_generation;
    candidate.position_postings.reset();
    candidate.opening_tree_index.reset();
    auto const write_res = write_manifest(dir_ / "manifest.json", candidate);
    if (!write_res.was_published()) {
        return tl::unexpected {write_res.error()};
    }
    manifest_ = std::move(candidate);
    position_postings_.reset();
    opening_tree_index_.reset();
    // Only now that the manifest durably no longer references these
    // generation-qualified files is it safe to delete them -- best-effort,
    // since a rebuild will overwrite/skip an orphan by filename anyway.
    std::error_code fs_err;
    if (stale_postings) {
        std::filesystem::remove(dir_ / stale_postings->filename, fs_err);
    }
    if (stale_tree) {
        fs_err.clear();
        std::filesystem::remove(dir_ / stale_tree->filename, fs_err);
    }
    if (!write_res) {
        return tl::unexpected {write_res.error()};
    }
    return {};
}

auto database_manager::lock_generation() const -> std::unique_lock<std::recursive_mutex>
{
    return std::unique_lock {generation_mutex_};
}

auto database_manager::prepare_canonical_mutation() -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!manifest_.position_postings && !manifest_.opening_tree_index) {
        return {};
    }
    return mark_derived_indexes_stale();
}

// Builds a candidate manifest around a fully-synced generation-qualified
// artifact. A pre-rename manifest failure leaves the candidate unpublished;
// a post-rename directory-sync failure reports that publication happened so
// callers retain the artifact and install the matching in-memory reader.
auto database_manager::publish_derived_index(std::string const& filename, bool const is_postings, std::uint64_t const next_build_seq)
    -> manifest_write_result
{
    if (auto sync_res = sync_file(dir_ / filename); !sync_res) {
        return manifest_write_result {.state = manifest_write_state::not_published, .failure = sync_res.error()};
    }
    auto checksum = file_checksum(dir_ / filename);
    if (!checksum) {
        return manifest_write_result {.state = manifest_write_state::not_published, .failure = checksum.error()};
    }
    assert(store_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    auto game_count = store_->count_games();
    if (!game_count || *game_count < 0) {
        return manifest_write_result {.state = manifest_write_state::not_published, .failure = error {error_code::io_failure}};
    }
    auto entry = derived_index_manifest_entry {.filename = filename,
                                               .source_generation = manifest_.source_generation,
                                               .game_count = static_cast<std::uint64_t>(*game_count),
                                               .file_size = checksum->first,
                                               .checksum = checksum->second};
    auto candidate = manifest_;
    candidate.derived_index_build_seq = next_build_seq;
    if (is_postings) {
        candidate.position_postings = std::move(entry);
    } else {
        candidate.opening_tree_index = std::move(entry);
    }
    auto write_res = write_manifest(dir_ / "manifest.json", candidate);
    if (write_res.was_published()) {
        manifest_ = std::move(candidate);
    }
    return write_res;
}

auto database_manager::query_position_matches(zobrist_hash const hash, std::size_t const limit, std::size_t const offset) const
    -> result<std::vector<position_match>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }

    if (!position_postings_ || !manifest_.position_postings || !has_valid_derived_index(*manifest_.position_postings)) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return tl::unexpected {error_code::io_failure};
    }

    auto matches = position_postings_->occurrences(hash, limit, offset);
    if (!matches) {
        return tl::unexpected {matches.error()};
    }
    return matches;
}

auto database_manager::query_position_first_matches(zobrist_hash const hash, std::span<game_id const> const game_ids) const
    -> result<std::vector<position_match>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    if (!position_postings_ || !manifest_.position_postings || !has_valid_derived_index(*manifest_.position_postings)) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return tl::unexpected {error_code::io_failure};
    }
    auto matches = position_postings_->first_occurrences(hash, game_ids);
    if (!matches) {
        return tl::unexpected {matches.error()};
    }
    return matches;
}

auto database_manager::position_summary(zobrist_hash const hash) const -> result<std::optional<position_postings_summary>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    if (!position_postings_ || !manifest_.position_postings || !has_valid_derived_index(*manifest_.position_postings)) {
        return std::optional<position_postings_summary> {};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return std::optional<position_postings_summary> {};
    }
    return position_postings_->summary(hash);
}

// Builds the replacement postings file to a generation-qualified filename
// that has never been published (manifest_.derived_index_build_seq, bumped
// only on a successful publish -- see publish_derived_index()), validates it
// by reopening it there, and only publishes the manifest entry once that
// succeeds. Unlike renaming a fixed "positions.postings" name over itself,
// the new bytes never occupy the currently-published artifact's path, so a
// failure at any point -- including a crash -- leaves the previously
// published generation's file, manifest entry, and in-memory
// manifest_/position_postings_ readers untouched and fully usable. The old
// generation's file is only removed after the new one is durably published.
auto database_manager::rebuild_position_postings(position_postings_build_metrics* const metrics) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (dir_.empty() || !store_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto const build_seq = manifest_.derived_index_build_seq;
    auto const new_filename = fmt::format("positions.postings.{}", build_seq);
    auto const new_path = dir_ / new_filename;
    auto const staging_path = dir_ / (new_filename + ".building");

    std::error_code fs_err;
    std::filesystem::remove(staging_path, fs_err);
    // Clears any orphan left by a prior attempt that crashed after this exact
    // (unpublished) filename was written but before the process could clean
    // it up -- manifest_.derived_index_build_seq is only advanced by a
    // successful publish, so a retry always recomputes the same filename.
    fs_err.clear();
    std::filesystem::remove(new_path, fs_err);

    // store_ is initialized by every successful persistent factory.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if (auto build_res = position_postings::build(*store_, staging_path, position_postings::default_spill_threshold, metrics); !build_res) {
        fs_err.clear();
        std::filesystem::remove(staging_path, fs_err);
        return tl::unexpected {build_res.error()};
    }
    {
        auto staged = position_postings {staging_path};
        if (auto open_res = staged.open(); !open_res) {
            fs_err.clear();
            std::filesystem::remove(staging_path, fs_err);
            return tl::unexpected {open_res.error()};
        }
    }

    std::filesystem::rename(staging_path, new_path, fs_err);
    if (fs_err) {
        fs_err.clear();
        std::filesystem::remove(staging_path, fs_err);
        return tl::unexpected {error_code::io_failure};
    }

    auto live_reader = position_postings {new_path};
    if (auto open_res = live_reader.open(); !open_res) {
        // The staged copy validated successfully immediately before the
        // rename, so a failure to reopen the very same bytes at their new
        // path is unexpected; manifest_/position_postings_ stay untouched
        // and the checksum gate keeps queries safe regardless.
        fs_err.clear();
        std::filesystem::remove(new_path, fs_err);
        return tl::unexpected {open_res.error()};
    }

    auto const previous_entry = manifest_.position_postings;
    auto const publish_res = publish_derived_index(new_filename, /*is_postings=*/true, build_seq + 1);
    if (!publish_res.was_published()) {
        fs_err.clear();
        std::filesystem::remove(new_path, fs_err);
        return tl::unexpected {publish_res.error()};
    }
    position_postings_ = std::move(live_reader);
    if (!publish_res) {
        return tl::unexpected {publish_res.error()};
    }

    // Only remove the previous generation's file after the new one is
    // durably published -- if this process crashes before this point, the
    // old file (still what the manifest points to) survives untouched.
    if (previous_entry && previous_entry->filename != new_filename) {
        fs_err.clear();
        std::filesystem::remove(dir_ / previous_entry->filename, fs_err);
    }
    return {};
}

// Same generation-qualified-filename staging/validate/publish discipline as
// rebuild_position_postings() (see its comment for the failure-safety
// rationale). opening_tree_index::build() already writes to a temp file and
// renames it over whatever path it is given, so passing it a ".building"
// staging path keeps that path's eventual target -- a filename that has
// never been published -- untouched until the freshly built replacement has
// been read back successfully.
auto database_manager::rebuild_opening_tree_index(opening_tree_index_build_metrics* const metrics) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (dir_.empty() || !store_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (!position_postings_ || !manifest_.position_postings || !has_valid_derived_index(*manifest_.position_postings)) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto const build_seq = manifest_.derived_index_build_seq;
    auto const new_filename = fmt::format("opening_tree.idx.{}", build_seq);
    auto const new_path = dir_ / new_filename;
    auto const staging_path = dir_ / (new_filename + ".building");

    std::error_code fs_err;
    std::filesystem::remove(staging_path, fs_err);
    fs_err.clear();
    std::filesystem::remove(new_path, fs_err);

    // store_ is initialized by every successful persistent factory.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto child_counts = opening_tree_child_count_stream {
        [this](opening_tree_child_count_visitor const& visitor) -> result<void>
        {
            return position_postings_->for_each_summary(
                [&visitor](zobrist_hash const hash, position_postings_summary const& summary) -> result<void>
                { return visitor(hash, summary.distinct_game_count); });
        }};
    if (auto build_res = opening_tree_index::build(*store_, staging_path, {}, metrics, std::move(child_counts)); !build_res) {
        fs_err.clear();
        std::filesystem::remove(staging_path, fs_err);
        return tl::unexpected {build_res.error()};
    }
    if (auto staged = opening_tree_index::open(staging_path); !staged) {
        fs_err.clear();
        std::filesystem::remove(staging_path, fs_err);
        return tl::unexpected {staged.error()};
    }

    std::filesystem::rename(staging_path, new_path, fs_err);
    if (fs_err) {
        fs_err.clear();
        std::filesystem::remove(staging_path, fs_err);
        return tl::unexpected {error_code::io_failure};
    }

    auto live_index = opening_tree_index::open(new_path);
    if (!live_index) {
        fs_err.clear();
        std::filesystem::remove(new_path, fs_err);
        return tl::unexpected {live_index.error()};
    }

    auto const previous_entry = manifest_.opening_tree_index;
    auto const publish_res = publish_derived_index(new_filename, /*is_postings=*/false, build_seq + 1);
    if (!publish_res.was_published()) {
        fs_err.clear();
        std::filesystem::remove(new_path, fs_err);
        return tl::unexpected {publish_res.error()};
    }
    opening_tree_index_ = std::move(*live_index);
    if (!publish_res) {
        return tl::unexpected {publish_res.error()};
    }

    if (previous_entry && previous_entry->filename != new_filename) {
        fs_err.clear();
        std::filesystem::remove(dir_ / previous_entry->filename, fs_err);
    }
    return {};
}

auto database_manager::position_game_ids(zobrist_hash const hash) const -> result<std::vector<game_id>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    if (!position_postings_ || !manifest_.position_postings || !has_valid_derived_index(*manifest_.position_postings)) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return tl::unexpected {error_code::io_failure};
    }
    return position_postings_->distinct_game_ids(hash);
}

auto database_manager::patch_game_metadata(game_id const game_key, game_patch const& patch) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }

    // Validate and narrow elo values before touching any state.
    auto new_white = std::optional<std::int16_t> {};
    if (patch.white_elo) {
        auto res = narrow_elo(patch.white_elo);
        if (!res) {
            return tl::unexpected {res.error()};
        }
        new_white = *res;
    }
    auto new_black = std::optional<std::int16_t> {};
    if (patch.black_elo) {
        auto res = narrow_elo(patch.black_elo);
        if (!res) {
            return tl::unexpected {res.error()};
        }
        new_black = *res;
    }

    auto const affects_derived = new_white || new_black || patch.result;
    if (affects_derived) {
        if (auto stale_res = prepare_canonical_mutation(); !stale_res) {
            return tl::unexpected {stale_res.error()};
        }
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store_ presence is checked at function entry.
    return store_->patch_metadata(game_key, patch);
}

auto database_manager::remove_game(game_id const game_key) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    if (auto stale_res = prepare_canonical_mutation(); !stale_res) {
        return tl::unexpected {stale_res.error()};
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store_ presence is checked at function entry.
    return store_->remove(game_key);
}

auto database_manager::remove_user_game(game_id const game_key) -> result<void>
{
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const provenance = store_->get_provenance(game_key);
    if (!provenance) {
        return tl::unexpected {provenance.error()};
    }
    if (provenance->source_type != "manual") {
        return tl::unexpected {error_code::not_editable};
    }

    return remove_game(game_key);
}

auto database_manager::query_elo_distribution(zobrist_hash const hash, search_filter const& filter, int const bucket_width) const
    -> result<std::vector<elo_distribution_row>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }

    if (bucket_width <= 0) {
        return tl::unexpected {error_code::invalid_argument};
    }

    bool const has_metadata = filter.player_name.has_value() || filter.min_elo.has_value() || filter.max_elo.has_value()
        || filter.result.has_value() || filter.eco_prefix.has_value();

    auto const game_count = store_->count_games();
    auto const postings_valid = position_postings_ && manifest_.position_postings && has_valid_derived_index(*manifest_.position_postings)
        && game_count && *game_count >= 0 && static_cast<std::uint64_t>(*game_count) == position_postings_->indexed_game_count();
    if (!postings_valid) {
        return tl::unexpected {error_code::io_failure};
    }

    if (!has_metadata) {
        return postings_elo_distribution(*store_, *position_postings_, hash, {}, bucket_width);
    }

    auto all_ids_res = position_game_ids(hash);
    if (!all_ids_res) {
        return tl::unexpected {all_ids_res.error()};
    }
    if (all_ids_res->empty()) {
        return std::vector<elo_distribution_row> {};
    }

    auto meta_filter = filter;
    meta_filter.position = std::nullopt;

    auto filtered_ids_res = store_->find_game_ids_with_filter(*all_ids_res, meta_filter);
    if (!filtered_ids_res) {
        return tl::unexpected {filtered_ids_res.error()};
    }
    if (filtered_ids_res->empty()) {
        return std::vector<elo_distribution_row> {};
    }

    return postings_elo_distribution(*store_, *position_postings_, hash, std::move(*filtered_ids_res), bucket_width);
}

auto database_manager::query_unfiltered_opening_stats(zobrist_hash const hash) const -> result<std::vector<opening_stat_agg_row>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const postings_valid = position_postings_ && manifest_.position_postings && has_valid_derived_index(*manifest_.position_postings);
    if (!postings_valid) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const tree_valid = opening_tree_index_ && manifest_.opening_tree_index && has_valid_derived_index(*manifest_.opening_tree_index);
    if (!tree_valid || static_cast<std::uint64_t>(*game_count) != opening_tree_index_->source_game_count()) {
        auto game_ids = position_postings_->distinct_game_ids(hash);
        if (!game_ids) {
            return tl::unexpected {game_ids.error()};
        }
        return postings_filtered_opening_stats(*store_, *position_postings_, hash, std::move(*game_ids), /*global_child_frequency=*/true);
    }
    // A complete node (currently only the canonical starting position, see
    // opening_tree_index.cpp's format_version v5 comment) was aggregated from
    // every occurrence of hash in every game, not only occurrences within
    // max_root_ply, so it needs no summary max_ply cross-check.
    if (opening_tree_index_->is_complete(hash)) {
        return opening_tree_index_->query_opening_stats(hash);
    }
    auto const summary = position_postings_->summary(hash);
    if (!summary || !summary->has_value()) {
        return std::vector<opening_stat_agg_row> {};
    }
    if (summary->value().max_ply > opening_tree_index_->max_root_ply()) {
        auto game_ids = position_postings_->distinct_game_ids(hash);
        if (!game_ids) {
            return tl::unexpected {game_ids.error()};
        }
        return postings_filtered_opening_stats(*store_, *position_postings_, hash, std::move(*game_ids), /*global_child_frequency=*/true);
    }
    return opening_tree_index_->query_opening_stats(hash);
}

auto database_manager::query_filtered_opening_stats(zobrist_hash const hash, std::vector<game_id> const& game_ids) const
    -> result<std::vector<opening_stat_agg_row>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    bool const postings_valid = position_postings_ && manifest_.position_postings && has_valid_derived_index(*manifest_.position_postings);
    if (!postings_valid) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return tl::unexpected {error_code::io_failure};
    }
    return postings_filtered_opening_stats(*store_, *position_postings_, hash, game_ids);
}

auto database_manager::query_tree_slice(zobrist_hash const root_hash,
                                        std::uint16_t const max_depth,
                                        std::vector<game_id> const& game_ids) const -> result<std::vector<tree_position_row>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    bool const postings_valid = position_postings_ && manifest_.position_postings && has_valid_derived_index(*manifest_.position_postings);
    if (!postings_valid) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return tl::unexpected {error_code::io_failure};
    }

    auto allowed_game_ids = game_ids;
    std::ranges::sort(allowed_game_ids);
    auto occurrences = position_postings_->occurrences(root_hash);
    if (!occurrences) {
        return tl::unexpected {occurrences.error()};
    }
    auto context_ids = std::vector<game_id> {};
    context_ids.reserve(occurrences->size());
    for (auto const& occurrence : *occurrences) {
        if (allowed_game_ids.empty() || std::ranges::binary_search(allowed_game_ids, occurrence.game_id)) {
            context_ids.push_back(occurrence.game_id);
        }
    }
    std::ranges::sort(context_ids);
    auto const unique_end = std::ranges::unique(context_ids);
    context_ids.erase(unique_end.begin(), unique_end.end());
    auto contexts = store_->get_game_contexts(context_ids);
    if (!contexts) {
        return tl::unexpected {contexts.error()};
    }

    auto rows = std::vector<tree_position_row> {};
    for (auto const& occurrence : *occurrences) {
        if (!allowed_game_ids.empty() && !std::ranges::binary_search(allowed_game_ids, occurrence.game_id)) {
            continue;
        }
        auto const context = contexts->find(occurrence.game_id);
        if (context == contexts->end()) {
            continue;
        }
        auto board = motif::chess::replay(context->second.moves, occurrence.ply);
        if (!board) {
            return tl::unexpected {error_code::io_failure};
        }
        auto const last_ply = std::min(context->second.moves.size(), static_cast<std::size_t>(occurrence.ply) + max_depth);
        for (auto ply = static_cast<std::size_t>(occurrence.ply); ply < last_ply; ++ply) {
            auto const encoded_move = context->second.moves[ply];
            motif::chess::apply_encoded_move(*board, encoded_move);
            rows.push_back(tree_position_row {.game_id = occurrence.game_id,
                                              .root_ply = occurrence.ply,
                                              .depth = static_cast<std::uint16_t>(ply - occurrence.ply + 1U),
                                              .encoded_move = encoded_move,
                                              .child_hash = zobrist_hash {board->hash()},
                                              .result = occurrence.result,
                                              .white_elo = occurrence.white_elo,
                                              .black_elo = occurrence.black_elo});
        }
    }
    return rows;
}

auto database_manager::find_games(search_filter const& filter) -> result<game_list_result>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }

    if (!filter.position.has_value()) {
        return store_->find_games(filter);
    }

    if (!position_postings_ || !manifest_.position_postings || !has_valid_derived_index(*manifest_.position_postings)) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != position_postings_->indexed_game_count()) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const has_metadata_filter = filter.player_name || filter.result || filter.eco_prefix || filter.min_elo || filter.max_elo;
    auto const fast_path_eligible = !has_metadata_filter && filter.sort_column == game_sort_column::id && filter.sort_ascending;
    if (fast_path_eligible) {
        auto const summary = position_postings_->summary(*filter.position);
        if (!summary) {
            return tl::unexpected {summary.error()};
        }
        if (!*summary) {
            return game_list_result {};
        }
        auto page_game_ids = position_postings_->distinct_game_ids(*filter.position, filter.limit, filter.offset);
        if (!page_game_ids) {
            return tl::unexpected {page_game_ids.error()};
        }
        auto metadata_filter = filter;
        metadata_filter.position.reset();
        metadata_filter.offset = 0U;
        auto page = store_->find_games_with_ids(*page_game_ids, metadata_filter);
        if (!page) {
            return tl::unexpected {page.error()};
        }
        page->total_count = static_cast<std::int64_t>((*summary)->distinct_game_count);
        return page;
    }

    auto game_ids = position_game_ids(*filter.position);
    if (!game_ids) {
        return tl::unexpected {game_ids.error()};
    }

    auto metadata_filter = filter;
    metadata_filter.position.reset();
    return store_->find_games_with_ids(*game_ids, metadata_filter);
}

}  // namespace motif::db
