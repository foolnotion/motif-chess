#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
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
#    include <cstdio>  // <cstdio> pulls in glibc's stdio.h, which declares renameat2

#    include <fcntl.h>
#    include <sys/file.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

#include <spdlog/spdlog.h>

#include "motif/chess/chess.hpp"
#include "motif/db/error.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/position_postings.hpp"
#include "motif/db/postings_metadata_patch.hpp"
#include "motif/db/published_snapshot.hpp"
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

// A bundle's canonical database must already exist when opening: with
// create=false a racing deletion cannot make SQLite silently create a fresh,
// empty games.db in its place.
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto open_sqlite(std::filesystem::path const& db_path, bool const create) -> result<sqlite3*>
{
    sqlite3* conn = nullptr;
    int const flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    int const ret = sqlite3_open_v2(db_path.c_str(), &conn, flags, nullptr);
    if (ret != SQLITE_OK) {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        return tl::unexpected {error_code::io_failure};
    }
    // Bulk-import pragma tuning. synchronous=NORMAL is crash-safe against
    // application and OS-process failure under WAL (no corruption), but a
    // power failure or OS crash can lose the most recent commits. Canonical
    // batch commits are therefore NOT promised power-loss durable; only
    // snapshot-bearing transactions are raised to FULL (see
    // synchronous_guard), which seals every canonical commit preceding them.
    // cache_size/mmap_size raise the working-set that can stay
    // resident as games.db grows into the multi-GB range; wal_autocheckpoint raised
    // from the 1000-page default reduces mid-import checkpoint stalls at the cost of
    // a larger WAL file between checkpoints.
    sqlite3_exec(conn, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    // cache_size is applied per bundle from the process-wide governor (see
    // apply_sqlite_cache_lease); until then SQLite's default cache applies.
    sqlite3_exec(conn, "PRAGMA mmap_size = 30000000000;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "PRAGMA wal_autocheckpoint = 10000;", nullptr, nullptr, nullptr);
    return conn;
}

// Applies a governor grant as the connection's page-cache size (negative
// cache_size means KiB).
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto apply_sqlite_cache_lease(sqlite3* conn, sqlite_cache_lease const& lease) -> void
{
    auto const pragma = fmt::format("PRAGMA cache_size = -{};", lease.bytes() / 1024U);
    sqlite3_exec(conn, pragma.c_str(), nullptr, nullptr, nullptr);
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

// Fault-injection convenience: true when the hook asks the guarded step to fail.
// The connection, when given, is visible to the hook (fault::hook_connection()).
auto injected(fault::point const where,
              fault::artifact_kind const kind = fault::artifact_kind::none,
              std::uint64_t const generation = 0,
              sqlite3* const conn = nullptr) -> bool
{
    return fault::inject(where, kind, generation, conn) == fault::fault_action::fail_io;
}

auto sync_file(std::filesystem::path const& path) -> result<void>
{
    if (!sync_filesystem_path(path, /*directory=*/false)) {
        return tl::unexpected {error {error_code::io_failure, "fsync failed"}};
    }
    return {};
}

auto sync_directory(std::filesystem::path const& path) -> result<void>
{
    if (!sync_filesystem_path(path, /*directory=*/true)) {
        return tl::unexpected {error {error_code::io_failure, "directory fsync failed"}};
    }
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
    raw_edge_stats stats;
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
        if (aggregate.stats.frequency == 0) {
            aggregate.root_ply = contribution.root_ply;
            aggregate.eco_sample_min = game_key;
            aggregate.eco_sample_max = game_key;
        }
        aggregate.root_ply = std::min(aggregate.root_ply, contribution.root_ply);
        aggregate.eco_sample_min = std::min(aggregate.eco_sample_min, game_key);
        aggregate.eco_sample_max = std::max(aggregate.eco_sample_max, game_key);
        // One game's contribution through the shared definition (rating sums,
        // counts, doubled weighted sums), added with overflow checks.
        auto const contribution_stats = make_game_stats(occurrence.result, occurrence.white_elo, occurrence.black_elo);
        if (!contribution_stats) {
            return tl::unexpected {contribution_stats.error()};
        }
        auto const summed = add_stats(aggregate.stats, *contribution_stats);
        if (!summed) {
            return tl::unexpected {error {error_code::io_failure, "filtered edge statistic overflow"}};
        }
        aggregate.stats = *summed;
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
        auto row = make_stat_row(key.first,
                                 key.second.value,
                                 aggregate.root_ply,
                                 aggregate.stats,
                                 aggregate.eco_sample_min.value,
                                 aggregate.eco_sample_max.value,
                                 child_game_counts[key.second]);
        if (!row) {
            return tl::unexpected {row.error()};
        }
        rows.push_back(*row);
    }
    return rows;
}

// NOLINTEND(readability-function-cognitive-complexity)

// Removes exactly the paths a create() attempt recorded in its ownership
// ledger and, when create() made the directory, the directory if it is now
// empty. The directory is NEVER scanned: it may predate create() and hold
// unrelated files whose names merely resemble ours (positions.postings.keepme,
// backups, ...).
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto cleanup_failed_create(std::filesystem::path const& dir, bool created_dir, std::vector<std::filesystem::path> const& owned) noexcept
    -> void
{
    std::error_code fs_err;
    for (auto const& path : owned) {
        fs_err.clear();
        std::filesystem::remove(path, fs_err);
    }
    if (created_dir) {
        // remove() on a directory succeeds only when it is empty.
        fs_err.clear();
        std::filesystem::remove(dir, fs_err);
    }
}

// Claims path for the ledger only if it does not exist yet: an existing file
// is not something this create() made. An unreadable status claims nothing.
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto claim_if_absent(std::vector<std::filesystem::path>& ledger, std::filesystem::path path) -> void
{
    std::error_code fs_err;
    auto const status = std::filesystem::symlink_status(path, fs_err);
    // Judged by the reported type, not by fs_err: standard libraries differ on
    // whether a missing file also sets the error code. file_type::none means the
    // status could not be read at all.
    if (status.type() == std::filesystem::file_type::not_found) {
        ledger.push_back(std::move(path));
    }
}

// Names this subsystem owns inside a bundle directory: a derived-artifact
// prefix (postings segment, postings metadata chain member, tree checkpoint,
// tree patch run), a token (decimal for legacy "positions.postings.7", 32 hex
// digits for v7), and then any number of builder/staging suffix components
// (".building", ".tmp", ".dirspool", ".spill<digits>"). Only these are ever
// garbage-collected; positions.duckdb, look-alikes such as
// "opening_tree.patch.keepme" and every other file are never matched.
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto is_owned_artifact_name(std::string_view const name) -> bool
{
    for (auto const prefix :
         {postings_artifact_prefix, tree_artifact_prefix, postings_metadata_artifact_prefix, tree_patch_artifact_prefix})
    {
        if (!name.starts_with(prefix)) {
            continue;
        }
        auto const rest = name.substr(prefix.size());
        auto const token_end = rest.find('.');
        auto const token = rest.substr(0, token_end);
        auto suffix = token_end == std::string_view::npos ? std::string_view {} : rest.substr(token_end);
        auto const hex_token = !token.empty()
            && std::ranges::all_of(token, [](char const chr) -> bool { return (chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'f'); });
        if (!hex_token) {
            return false;
        }
        while (!suffix.empty()) {
            auto const next_dot = suffix.find('.', 1);
            auto const component = suffix.substr(0, next_dot);
            auto const known = component == ".building" || component == ".tmp" || component == ".dirspool"
                || (component.starts_with(".spill")
                    && std::ranges::all_of(component.substr(6), [](char const chr) -> bool { return chr >= '0' && chr <= '9'; }));
            if (!known) {
                return false;
            }
            suffix = next_dot == std::string_view::npos ? std::string_view {} : suffix.substr(next_dot);
        }
        return true;
    }
    return false;
}

// Installs from at target without ever replacing an existing file: a collision
// fails (false) and leaves the existing file untouched.
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto rename_no_replace(std::filesystem::path const& from, std::filesystem::path const& target) -> bool
{
#ifdef _WIN32
    // Without MOVEFILE_REPLACE_EXISTING, MoveFileEx fails when the target exists.
    return MoveFileExW(from.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
#    ifdef __linux__
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) == 0) {
        return true;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return false;
    }
#    endif
    // Filesystems without RENAME_NOREPLACE: link() fails with EEXIST rather
    // than replacing, then the staging name is dropped.
    if (::link(from.c_str(), target.c_str()) != 0) {
        return false;
    }
    static_cast<void>(::unlink(from.c_str()));
    return true;
#endif
}

// A reserved, exclusively created staging name for a not-yet-published
// artifact. The final name is prefix + random 128-bit id; nothing existing is
// ever overwritten to make room for it.
struct staged_artifact
{
    std::string id;
    std::string filename;
    std::filesystem::path staging_path;
    std::filesystem::path final_path;
    // True once rename_no_replace() installed final_path: only then is that
    // file ours to delete on a failed publication.
    bool installed {false};
};

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto reserve_staged_artifact(std::filesystem::path const& dir, std::string_view const prefix) -> result<staged_artifact>
{
    constexpr auto max_attempts = 16;
    for (auto attempt = 0; attempt < max_attempts; ++attempt) {
        auto candidate = staged_artifact {};
        candidate.id = generate_random_id();
        candidate.filename = std::string {prefix} + candidate.id;
        candidate.final_path = dir / candidate.filename;
        candidate.staging_path = dir / (candidate.filename + ".building");
        std::error_code fs_err;
        if (std::filesystem::exists(candidate.final_path, fs_err) || fs_err) {
            continue;
        }
#ifdef _WIN32
        auto const handle =
            CreateFileW(candidate.staging_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            return candidate;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            return tl::unexpected {error_code::io_failure};
        }
#else
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- POSIX open with O_EXCL reserves the name.
        auto const descriptor = ::open(candidate.staging_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, bundle_lock_permissions);
        if (descriptor >= 0) {
            ::close(descriptor);
            return candidate;
        }
        if (errno != EEXIST) {
            return tl::unexpected {error_code::io_failure};
        }
#endif
    }
    return tl::unexpected {error {error_code::io_failure, "could not reserve a unique artifact name"}};
}

// Removes this attempt's own unreferenced files unless release()d. Only paths
// this attempt created are ever listed, so a failure can never delete an
// existing file or another snapshot's artifact.
class attempt_cleanup
{
  public:
    explicit attempt_cleanup(staged_artifact& artifact) noexcept
        : artifact_ {artifact}
    {
    }

    ~attempt_cleanup()
    {
        if (released_) {
            return;
        }
        std::error_code fs_err;
        std::filesystem::remove(artifact_.staging_path, fs_err);
        if (artifact_.installed) {
            fs_err.clear();
            std::filesystem::remove(artifact_.final_path, fs_err);
        }
    }

    void release() noexcept { released_ = true; }

    attempt_cleanup(attempt_cleanup const&) = delete;
    auto operator=(attempt_cleanup const&) -> attempt_cleanup& = delete;
    attempt_cleanup(attempt_cleanup&&) = delete;
    auto operator=(attempt_cleanup&&) -> attempt_cleanup& = delete;

  private:
    staged_artifact& artifact_;
    bool released_ {false};
};

// Raises PRAGMA synchronous to FULL for exactly one snapshot-bearing commit and
// restores the prior level afterwards. SQLite fixes the commit's sync level
// when the transaction begins, so this must be entered OUTSIDE a transaction.
// Batch ingest stays at NORMAL; only snapshot publication pays for FULL.
class synchronous_guard
{
  public:
    explicit synchronous_guard(sqlite3* conn) noexcept
        : conn_ {conn}
    {
    }

    ~synchronous_guard() { restore(); }

    synchronous_guard(synchronous_guard const&) = delete;
    auto operator=(synchronous_guard const&) -> synchronous_guard& = delete;
    synchronous_guard(synchronous_guard&&) = delete;
    auto operator=(synchronous_guard&&) -> synchronous_guard& = delete;

    [[nodiscard]] auto enter(std::uint64_t const generation) -> result<void>
    {
        if (sqlite3_get_autocommit(conn_) == 0) {
            return tl::unexpected {error {error_code::invalid_argument, "snapshot publication needs autocommit mode"}};
        }
        auto journal = pragma_text("PRAGMA journal_mode;");
        if (!journal) {
            return tl::unexpected {journal.error()};
        }
        if (*journal != "wal") {
            return tl::unexpected {error {error_code::io_failure, "snapshot publication requires WAL journal mode"}};
        }
        auto previous = pragma_int("PRAGMA synchronous;");
        if (!previous) {
            return tl::unexpected {previous.error()};
        }
        if (fault::inject(fault::point::before_synchronous_full, fault::artifact_kind::none, generation, conn_)
            == fault::fault_action::fail_io)
        {
            return tl::unexpected {error {error_code::io_failure, "injected synchronous=FULL failure"}};
        }
        constexpr auto full_level = 2;
        if (sqlite3_exec(conn_, "PRAGMA synchronous = FULL;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn_)}};
        }
        previous_ = *previous;
        active_ = true;
        auto const readback = pragma_int("PRAGMA synchronous;");
        if (!readback || *readback != full_level) {
            restore();
            return tl::unexpected {error {error_code::io_failure, "synchronous=FULL did not take effect"}};
        }
        generation_ = generation;
        return {};
    }

    // Called after COMMIT (or on failure). A failure to restore leaves the
    // connection at FULL -- slower, never less durable -- and never undoes a
    // completed commit.
    void restore() noexcept
    {
        if (!active_) {
            return;
        }
        active_ = false;
        if (fault::inject(fault::point::before_synchronous_restore, fault::artifact_kind::none, generation_, conn_)
            == fault::fault_action::fail_io)
        {
            log_degraded();
            return;
        }
        auto const sql = fmt::format("PRAGMA synchronous = {};", previous_);
        if (sqlite3_exec(conn_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
            log_degraded();
        }
    }

  private:
    static void log_degraded() noexcept
    {
        if (auto const log = spdlog::get("motif.db"); log != nullptr) {
            log->warn("could not restore PRAGMA synchronous after a snapshot publication; the connection stays at FULL");
        }
    }

    auto pragma_text(char const* sql) -> result<std::string>
    {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(conn_, sql, -1, &raw, nullptr) != SQLITE_OK) {
            return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn_)}};
        }
        auto const stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> {raw, sqlite3_finalize};
        if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
            return tl::unexpected {error_code::io_failure};
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(stmt.get(), 0));
        if (text == nullptr) {
            return tl::unexpected {error_code::io_failure};
        }
        return std::string {text};
    }

    auto pragma_int(char const* sql) -> result<int>
    {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(conn_, sql, -1, &raw, nullptr) != SQLITE_OK) {
            return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn_)}};
        }
        auto const stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> {raw, sqlite3_finalize};
        if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
            return tl::unexpected {error_code::io_failure};
        }
        return sqlite3_column_int(stmt.get(), 0);
    }

    sqlite3* conn_;
    int previous_ {1};
    bool active_ {false};
    std::uint64_t generation_ {};
};

// A descriptor is current only if the file on disk still has the recorded size
// and checksum.
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto artifact_matches(std::filesystem::path const& dir, artifact_descriptor const& artifact) -> bool
{
    auto const checksum = file_checksum(dir / artifact.filename);
    return checksum && checksum->first == artifact.size_bytes && checksum->second == artifact.checksum;
}

// Records a diagnostic from a noexcept cleanup path. Never throws and never
// fails the caller: a lost log line costs nothing, an escaping exception would.
void log_db_warning(char const* const message) noexcept
{
    if (auto const log = spdlog::get("motif.db"); log != nullptr) {
        log->warn("{}", message);
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
        // flock(2) locks belong to the open file description, so a second open of
        // the same bundle in this process is refused too; fcntl(F_SETLK) record
        // locks are per-process and would admit it.
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
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
            static_cast<void>(::flock(descriptor_, LOCK_UN));
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
    , manifest_ {std::move(other.manifest_)}
    , dir_ {std::move(other.dir_)}
    , position_postings_ {std::move(other.position_postings_)}
    , composite_tree_ {std::move(other.composite_tree_)}
    , is_scratch_ {std::exchange(other.is_scratch_, false)}
    , created_bundle_ {std::exchange(other.created_bundle_, false)}
    , created_dir_ {std::exchange(other.created_dir_, false)}
    , created_paths_ {std::move(other.created_paths_)}
    , manifest_cache_repair_pending_ {std::exchange(other.manifest_cache_repair_pending_, false)}
    , bundle_lock_ {std::move(other.bundle_lock_)}
    , cache_lease_ {std::move(other.cache_lease_)}
    , chain_limits_ {other.chain_limits_}
{
    other.store_.reset();
    other.position_postings_.reset();
    other.composite_tree_.reset();
}

auto database_manager::operator=(database_manager&& other) noexcept -> database_manager&
{
    if (this != &other) {
        close();
        conn_ = std::exchange(other.conn_, nullptr);
        store_ = std::move(other.store_);
        manifest_ = std::move(other.manifest_);
        dir_ = std::move(other.dir_);
        position_postings_ = std::move(other.position_postings_);
        composite_tree_ = std::move(other.composite_tree_);
        is_scratch_ = std::exchange(other.is_scratch_, false);
        created_bundle_ = std::exchange(other.created_bundle_, false);
        created_dir_ = std::exchange(other.created_dir_, false);
        created_paths_ = std::move(other.created_paths_);
        other.created_paths_.clear();
        manifest_cache_repair_pending_ = std::exchange(other.manifest_cache_repair_pending_, false);
        bundle_lock_ = std::move(other.bundle_lock_);
        cache_lease_ = std::move(other.cache_lease_);
        chain_limits_ = other.chain_limits_;
        other.store_.reset();
        other.position_postings_.reset();
        other.composite_tree_.reset();
    }
    return *this;
}

void database_manager::close() noexcept
{
    auto const lock = std::scoped_lock {generation_mutex_};
    // manifest.json is only a cache: close() rewrites it solely to retry a
    // mirror that failed earlier (best-effort -- errors are swallowed since
    // close() is noexcept). A matching cache is never rewritten.
    if (manifest_cache_repair_pending_ && !dir_.empty() && store_ && conn_ != nullptr) {
        mirror_manifest();
    }
    manifest_cache_repair_pending_ = false;

    position_postings_.reset();
    composite_tree_.reset();
    store_.reset();
    if (conn_ != nullptr) {
        sqlite3_close(conn_);
        conn_ = nullptr;
    }
    cache_lease_ = sqlite_cache_lease {};
    bundle_lock_.reset();

    if (is_scratch_ && !dir_.empty()) {
        std::error_code fs_err;
        std::filesystem::remove_all(dir_, fs_err);
    }
}

void database_manager::discard_created_bundle() noexcept
{
    auto const dir = dir_;
    auto const owns_bundle = created_bundle_;
    auto const remove_dir = created_dir_;
    auto const owned = std::move(created_paths_);
    created_paths_.clear();
    close();
    if (!owns_bundle || dir.empty()) {
        return;
    }
    created_bundle_ = false;
    created_dir_ = false;
    dir_.clear();
    cleanup_failed_create(dir, remove_dir, owned);
}

// ── Factory methods
// ───────────────────────────────────────────────────────────

auto database_manager::create(std::filesystem::path const& dir, std::string const& name) -> result<database_manager>
{
    auto const db_path = dir / "games.db";

    std::error_code fs_err;
    auto const created_dir = std::filesystem::create_directories(dir, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }

    // Ledger of everything this attempt puts on disk (see created_paths_).
    // A path is claimed only if it did not exist before: a pre-existing
    // look-alike is the user's, not ours. The lock file is created on demand
    // by acquire_bundle_lock(), so it is claimed before that call.
    auto ledger = std::vector<std::filesystem::path> {};
    claim_if_absent(ledger, dir / ".motif.lock");
    auto bundle_lock = acquire_bundle_lock(dir);
    if (!bundle_lock) {
        cleanup_failed_create(dir, created_dir, ledger);
        return tl::unexpected {bundle_lock.error()};
    }

    auto db_exists = path_exists(db_path);
    if (!db_exists || *db_exists) {
        cleanup_failed_create(dir, created_dir, ledger);
        return tl::unexpected {db_exists ? error {error_code::io_failure} : db_exists.error()};
    }

    // Claimed before SQLite opens the files so a failure at any later point
    // removes exactly these.
    ledger.push_back(db_path);  // verified absent above
    for (auto const* suffix : {"-wal", "-shm", "-journal"}) {
        claim_if_absent(ledger, std::filesystem::path {db_path.string() + suffix});
    }
    claim_if_absent(ledger, dir / "manifest.json.tmp");
    claim_if_absent(ledger, dir / "manifest.json");

    auto conn_res = open_sqlite(db_path, /*create=*/true);
    if (!conn_res) {
        cleanup_failed_create(dir, created_dir, ledger);
        return tl::unexpected {conn_res.error()};
    }
    sqlite3* conn = *conn_res;

    // The schema, the mutation sequence and the unbuilt snapshot row (bundle
    // identity, no artifacts) commit as one transaction with user_version. A
    // crash after this point and before the first publication is recoverable
    // by building empty postings, never by trusting a manifest.
    auto unbuilt = build_manifest(
        0,
        std::nullopt,
        0,
        snapshot_payload {
            .bundle = {.id = generate_random_id(), .name = name, .created_at = now_iso8601()}, .segments = {}, .tree = std::nullopt});
    if (!unbuilt) {
        sqlite3_close(conn);
        cleanup_failed_create(dir, created_dir, ledger);
        return tl::unexpected {unbuilt.error()};
    }
    auto const bootstrap = schema::snapshot_bootstrap {
        .payload_version = unbuilt->payload_version, .payload_json = unbuilt->payload_json, .payload_checksum = unbuilt->payload_checksum};
    if (auto init_res = schema::initialize(conn, &bootstrap); !init_res) {
        sqlite3_close(conn);
        cleanup_failed_create(dir, created_dir, ledger);
        return tl::unexpected {init_res.error()};
    }

    database_manager mgr;
    mgr.conn_ = conn;
    mgr.store_.emplace(conn);
    mgr.manifest_ = std::move(*unbuilt);
    mgr.dir_ = dir;
    mgr.created_bundle_ = true;
    mgr.created_dir_ = created_dir;
    mgr.created_paths_ = std::move(ledger);
    mgr.bundle_lock_ = std::move(*bundle_lock);
    mgr.cache_lease_ = resource_governor::process().acquire_sqlite_cache();
    apply_sqlite_cache_lease(conn, mgr.cache_lease_);

    // Publish a trivial empty (0-game) postings generation immediately so the
    // bundle is queryable at sequence 0. The first import republishes the real
    // generation once games exist (see rebuild_position_postings()). Failures
    // here precede the snapshot COMMIT, so removing the files create() owns is
    // correct; once that COMMIT succeeds a manifest.json failure is only a
    // pending cache repair and cannot reach this cleanup.
    if (auto postings_res = mgr.rebuild_position_postings(); !postings_res) {
        mgr.close();
        cleanup_failed_create(dir, created_dir, mgr.created_paths_);
        return tl::unexpected {postings_res.error()};
    }

    return mgr;
}

namespace
{

// Migrates a pre-v7 bundle in place. The caller closes the connection when this
// fails.
auto migrate_legacy_bundle(sqlite3* const conn, std::filesystem::path const& dir, std::uint32_t const from_version) -> result<void>
{
    // The legacy manifest is consulted only here: to refuse a bundle written by
    // a newer build BEFORE any in-place migration (otherwise the migration
    // could not be undone), and to recover the bundle's name and creation time.
    // Its derived-artifact entries are never adopted: the old artifacts carry no
    // per-game content digest, so nothing short of a rebuild could prove them
    // equal to canonical SQLite. The migration therefore seeds an unbuilt
    // snapshot (sequence 0, null coverage) and the normal recovery in open()
    // rebuilds once.
    auto const legacy = read_legacy_manifest(dir / "manifest.json");
    if (legacy && legacy->schema_version > schema::current_version) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    // Verified or transitioned under the bundle lock BEFORE the schema
    // version changes: a rollback-journal bundle must never end up migrated
    // to v7 and then be unable to publish. If the transition is impossible
    // the open fails with the bundle still at its old version.
    if (auto wal_res = schema::ensure_wal(conn); !wal_res) {
        return tl::unexpected {wal_res.error()};
    }
    auto fallback_name = dir.filename().string();
    if (fallback_name.empty()) {
        fallback_name = dir.parent_path().filename().string();
    }
    auto identity = bundle_identity {.id = generate_random_id(),
                                     .name = legacy && !legacy->name.empty() ? legacy->name : fallback_name,
                                     .created_at = legacy && !legacy->created_at.empty() ? legacy->created_at : now_iso8601()};
    auto unbuilt =
        build_manifest(0, std::nullopt, 0, snapshot_payload {.bundle = std::move(identity), .segments = {}, .tree = std::nullopt});
    if (!unbuilt) {
        return tl::unexpected {unbuilt.error()};
    }
    auto const bootstrap = schema::snapshot_bootstrap {
        .payload_version = unbuilt->payload_version, .payload_json = unbuilt->payload_json, .payload_checksum = unbuilt->payload_checksum};
    // The v7 bootstrap and migration is a durability boundary like a
    // snapshot publication: it commits under synchronous=FULL (entered
    // before BEGIN, restored afterwards by the guard) so a formerly NORMAL
    // canonical baseline and the v7 tables are joined durably. The guard
    // also refuses a non-WAL database, which ensure_wal() above ruled out.
    auto sync = synchronous_guard {conn};
    if (auto entered = sync.enter(/*generation=*/0); !entered) {
        return tl::unexpected {entered.error()};
    }
    return schema::migrate(conn, from_version, &bootstrap);
}

// Brings a freshly opened existing connection to the current schema: foreign
// keys on, a newer schema refused, a pre-v7 bundle migrated in place and a v7
// bundle required to be in WAL mode. The caller closes the connection when this
// fails.
auto prepare_existing_connection(sqlite3* const conn, std::filesystem::path const& dir) -> result<void>
{
    if (auto fk_res = enable_foreign_keys(conn); !fk_res) {
        return tl::unexpected {fk_res.error()};
    }
    auto const ver_res = schema::version(conn);
    if (!ver_res) {
        return tl::unexpected {ver_res.error()};
    }
    if (*ver_res > schema::current_version) {
        return tl::unexpected {error_code::schema_mismatch};
    }
    if (*ver_res < schema::current_version) {
        return migrate_legacy_bundle(conn, dir, *ver_res);
    }
    // A v7 bundle is accepted only in WAL mode: snapshot publication and
    // its FULL-commit guarantee assume it, and failing here is cheaper
    // than failing at the first publication.
    if (auto wal_res = schema::ensure_wal(conn); !wal_res) {
        return tl::unexpected {wal_res.error()};
    }
    return {};
}

}  // namespace

auto database_manager::open(std::filesystem::path const& dir) -> result<database_manager>
{
    auto const db_path = dir / "games.db";

    auto db_exists = path_exists(db_path);
    if (!db_exists) {
        return tl::unexpected {db_exists.error()};
    }
    if (!*db_exists) {
        return tl::unexpected {error_code::not_found};
    }

    auto bundle_lock = acquire_bundle_lock(dir);
    if (!bundle_lock) {
        return tl::unexpected {bundle_lock.error()};
    }

    // Existing canonical storage only: never create games.db here.
    auto conn_res = open_sqlite(db_path, /*create=*/false);
    if (!conn_res) {
        return tl::unexpected {conn_res.error()};
    }
    sqlite3* conn = *conn_res;

    if (auto prepared = prepare_existing_connection(conn, dir); !prepared) {
        sqlite3_close(conn);
        return tl::unexpected {prepared.error()};
    }

    database_manager mgr;
    mgr.conn_ = conn;
    mgr.store_.emplace(conn);
    mgr.dir_ = dir;
    mgr.bundle_lock_ = std::move(*bundle_lock);
    mgr.cache_lease_ = resource_governor::process().acquire_sqlite_cache();
    apply_sqlite_cache_lease(conn, mgr.cache_lease_);

    // SQLite is authoritative. A malformed or checksum-bad snapshot row is not
    // a trustworthy file inventory, so it fails closed instead of being
    // replaced by whatever files happen to be in the directory.
    auto snapshot = read_published_snapshot(conn);
    if (!snapshot) {
        mgr.close();
        return tl::unexpected {snapshot.error()};
    }
    mgr.manifest_ = std::move(*snapshot);

    // An interrupted raw import left game_identity_lookup dropped and possibly
    // duplicate rows. Reconcile them now (sequence-aware: this advances the
    // sequence when it changes anything) so coverage is never accepted or
    // built against unreconciled rows.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store_ was emplaced above.
    auto const identity_index = mgr.store_->writer().identity_index_exists();
    if (!identity_index) {
        mgr.close();
        return tl::unexpected {identity_index.error()};
    }
    if (!*identity_index) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store_ was emplaced above.
        if (auto repair_res = mgr.store_->writer().deduplicate(); !repair_res) {
            mgr.close();
            return tl::unexpected {repair_res.error()};
        }
    }

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- store_ was emplaced above.
    auto const current_seq = mgr.store_->mutation_seq();
    if (!current_seq) {
        mgr.close();
        return tl::unexpected {current_seq.error()};
    }
    if (mgr.manifest_.covered_mutation_seq && *mgr.manifest_.covered_mutation_seq > *current_seq) {
        // A snapshot cannot cover a sequence canonical SQLite never reached:
        // the database was restored from an older copy or edited externally.
        mgr.close();
        return tl::unexpected {error {error_code::io_failure, "published snapshot is ahead of the canonical mutation sequence"}};
    }

    if (auto recovered = mgr.recover_derived_state(*current_seq, dir / "manifest.json"); !recovered) {
        mgr.close();
        return tl::unexpected {recovered.error()};
    }
    return mgr;
}

auto database_manager::recover_derived_state(std::uint64_t const current_mutation_seq, std::filesystem::path const& manifest_path)
    -> result<void>
{
    // Verify and load only what could be current. A snapshot that already lags
    // the sequence is going to be replaced, so its multi-gigabyte artifacts are
    // not checksummed for nothing.
    auto load_outcome = load_readers(current_mutation_seq);

    // Missing, corrupt, stale (sequence mismatch -- the expected state after
    // any index-relevant mutation in this version), unbuilt (new or migrated
    // bundle), or predates postings (a leftover positions.duckdb is ignored,
    // never read or deleted): rebuild directly from canonical SQLite. A
    // failed rebuild fails open() closed and is safely retryable: nothing is
    // referenced until a snapshot commit succeeds.
    auto coverage = coverage_locked();
    if (!coverage) {
        return tl::unexpected {coverage.error()};
    }
    if (!coverage->postings_usable && load_outcome.metadata_chain_invalid && load_outcome.baseline) {
        // The postings segment verified but its metadata chain did not. Never
        // serve the embedded (older) metadata as current: repair narrowly from
        // canonical SQLite without replaying any move; if the baseline
        // membership no longer matches (or the repair fails) the full rebuild
        // below takes over.
        if (repair_metadata_chain(std::move(*load_outcome.baseline))) {
            coverage = coverage_locked();
            if (!coverage) {
                return tl::unexpected {coverage.error()};
            }
        }
    }
    if (!coverage->postings_usable) {
        if (auto rebuild_res = rebuild_position_postings(); !rebuild_res) {
            return tl::unexpected {rebuild_res.error()};
        }
    } else if (manifest_.tree() != nullptr && !coverage->tree_usable) {
        // The optional tree (checkpoint, or ANY of its patch runs) is missing,
        // corrupt or stale: the WHOLE composite tree is disabled -- never just
        // the bad run or node -- while exact postings keep serving, and the
        // descriptor is cleared without advancing the mutation sequence.
        clear_unusable_tree();
    }

    // The cache is rewritten only when it differs from SQLite's row.
    if (!manifest_cache_current(manifest_path, manifest_)) {
        mirror_manifest();
    }
    collect_garbage();
    return {};
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
    // NOLINTNEXTLINE(misc-static-assert,hicpp-static-assert,cert-dcl03-c) -- runtime invariant, not a constant
    assert(store_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return *store_;
}

auto database_manager::writer() noexcept -> game_writer&
{
    // NOLINTNEXTLINE(misc-static-assert,hicpp-static-assert,cert-dcl03-c) -- runtime invariant, not a constant
    assert(store_.has_value());
    // The store owns the connection's only writer; see game_writer.hpp.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return store_->writer();
}

auto database_manager::insert_game(game const& src_game) -> result<game_id>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
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

auto database_manager::lock_generation() const -> std::unique_lock<std::recursive_mutex>
{
    return std::unique_lock {generation_mutex_};
}

// ── Coverage ─────────────────────────────────────────────────────────────────

// The one coverage computation behind every position-dependent API:
//
//   postings_usable = a verified postings reader is installed
//                     && the snapshot covers some sequence
//                     && covered == mutation_state.mutation_seq (one indexed-row read)
//                     && no uncommitted index-relevant change is pending
//   tree_usable     = postings_usable && a verified tree reader is installed
//                     && tree.covered == mutation_state.mutation_seq
//
// A SQLite read error is an error, not "false". Counts play no part: they are
// build-time validation only, and cost O(rows) per query.
// Caller must hold generation_mutex_.
auto database_manager::coverage_locked() const -> result<derived_index_report>
{
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const seq = store_->mutation_seq();
    if (!seq) {
        return tl::unexpected {seq.error()};
    }
    auto report = derived_index_report {};
    report.mutation_seq = *seq;
    report.covered_mutation_seq = manifest_.covered_mutation_seq;
    report.pending_canonical_change = store_->has_pending_canonical_change();
    report.publication_generation = manifest_.publication_generation;
    report.postings_usable = position_postings_.has_value() && manifest_.postings() != nullptr && manifest_.covered_mutation_seq.has_value()
        && *manifest_.covered_mutation_seq == *seq && !report.pending_canonical_change;
    auto const* tree = manifest_.tree();
    report.tree_usable = report.postings_usable && composite_tree_.has_value() && tree != nullptr && tree->covered_mutation_seq == *seq;
    return report;
}

auto database_manager::require_postings_usable() const -> result<derived_index_report>
{
    auto report = coverage_locked();
    if (!report) {
        return tl::unexpected {report.error()};
    }
    if (!report->postings_usable) {
        return tl::unexpected {error_code::io_failure};
    }
    return report;
}

auto database_manager::data_revision() const noexcept -> std::uint64_t
{
    return store_ ? store_->data_revision() : 0U;
}

auto database_manager::derived_index_status() const -> result<derived_index_report>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    return coverage_locked();
}

// ── Publication ──────────────────────────────────────────────────────────────

namespace
{

// Executes the COMMIT -- or, for the ambiguous-COMMIT test seam, makes the
// outcome real first (applied or discarded) and reports an error although the
// transaction is already gone. True when the COMMIT is reported as failed.
auto execute_commit(sqlite3* const conn, fault::fault_action const action) -> bool
{
    switch (action) {
        case fault::fault_action::commit_error_after_commit:
            sqlite3_exec(conn, "COMMIT;", nullptr, nullptr, nullptr);
            return true;
        case fault::fault_action::commit_error_after_rollback:
            sqlite3_exec(conn, "ROLLBACK;", nullptr, nullptr, nullptr);
            return true;
        case fault::fault_action::proceed:
        case fault::fault_action::fail_io:
            break;
    }
    return sqlite3_exec(conn, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK;
}

// Asks SQLite whether the snapshot row of `next` is the stored one after a
// COMMIT that reported an error although its transaction is gone. An error
// means the answer is unknown; never decide from the COMMIT's error code alone.
auto snapshot_row_landed(sqlite3* const conn, db_manifest const& next, fault::artifact_kind const kind, std::uint64_t const generation)
    -> result<bool>
{
    if (injected(fault::point::before_commit_reconcile, kind, generation, conn)) {
        return tl::unexpected {error {error_code::io_failure, "injected reconciliation read failure"}};
    }
    auto const stored = read_published_snapshot(conn);
    if (!stored) {
        return tl::unexpected {stored.error()};
    }
    return stored->publication_generation == next.publication_generation && stored->payload_checksum == next.payload_checksum;
}

}  // namespace

// The snapshot-bearing SQLite transaction. The caller has already built,
// validated, fsynced, renamed and directory-fsynced the candidate artifact, so
// everything here is a few row writes: no chess replay, file I/O or checksum
// scan runs inside the transaction. FULL synchronous is raised before BEGIN
// (SQLite fixes the sync level when the transaction starts) and restored after
// COMMIT. COMMIT is the only publication point.
auto database_manager::commit_snapshot(db_manifest const& next,
                                       std::uint64_t const expected_mutation_seq,
                                       publication_kind const publication,
                                       bool& outcome_unknown,
                                       canonical_edit_steps const* const edit) -> result<void>
{
    outcome_unknown = false;
    auto const kind = [publication]() -> fault::artifact_kind
    {
        switch (publication) {
            case publication_kind::tree:
                return fault::artifact_kind::tree;
            case publication_kind::chain:
                return fault::artifact_kind::chain;
            case publication_kind::edit:
                return fault::artifact_kind::edit;
            case publication_kind::postings:
                break;
        }
        return fault::artifact_kind::postings;
    }();
    auto const generation = next.publication_generation;

    auto sync = synchronous_guard {conn_};
    if (auto entered = sync.enter(generation); !entered) {
        return tl::unexpected {entered.error()};
    }
    if (injected(fault::point::before_snapshot_begin, kind, generation, conn_)) {
        return tl::unexpected {error {error_code::io_failure, "injected failure before snapshot transaction"}};
    }
    if (sqlite3_exec(conn_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return tl::unexpected {error {error_code::io_failure, sqlite3_errmsg(conn_)}};
    }
    auto const rollback = [this](error err) -> result<void>
    {
        sqlite3_exec(conn_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return tl::unexpected {std::move(err)};
    };
    // The edit's sequence-advancing half: with the write lock held (BEGIN
    // IMMEDIATE), the sequence the artifacts were prepared against must still be
    // current; then the canonical UPDATE(s), then exactly one advance. The
    // snapshot row below then covers base + 1.
    auto snapshot_sequence = expected_mutation_seq;
    if (edit != nullptr) {
        if (auto edited = apply_canonical_edit(*edit, expected_mutation_seq, kind, generation); !edited) {
            return rollback(edited.error());
        }
        snapshot_sequence = expected_mutation_seq + 1U;
    }
    if (auto replaced = replace_published_snapshot(conn_, next, snapshot_sequence); !replaced) {
        return rollback(replaced.error());
    }
    if (injected(fault::point::after_snapshot_row_write, kind, generation, conn_)) {
        return rollback(error {error_code::io_failure, "injected failure after snapshot row write"});
    }
    auto const commit_fault = fault::inject(fault::point::before_sqlite_commit, kind, generation, conn_);
    if (commit_fault == fault::fault_action::fail_io) {
        return rollback(error {error_code::io_failure, "injected failure before COMMIT"});
    }
    if (execute_commit(conn_, commit_fault)) {
        auto failure = error {error_code::io_failure, sqlite3_errmsg(conn_)};
        if (sqlite3_get_autocommit(conn_) == 0) {
            // The transaction is still open: roll it back and keep the old
            // references; the commit definitely did not happen.
            return rollback(std::move(failure));
        }
        // The transaction is gone but COMMIT reported an error (an I/O failure
        // after the WAL write, say). Whether the row landed is unknown until
        // SQLite is asked.
        auto const landed = snapshot_row_landed(conn_, next, kind, generation);
        if (!landed) {
            outcome_unknown = true;
            return tl::unexpected {std::move(failure)};
        }
        if (!*landed) {
            return tl::unexpected {std::move(failure)};
        }
        // It did commit after all; fall through as a success.
    }
    sync.restore();
    // A hook cannot turn a committed publication into a failed one; the point
    // exists so a forked test child can die exactly here.
    static_cast<void>(fault::inject(fault::point::after_sqlite_commit, kind, generation, conn_));
    return {};
}

auto database_manager::apply_canonical_edit(canonical_edit_steps const& edit,
                                            std::uint64_t const expected_mutation_seq,
                                            fault::artifact_kind const kind,
                                            std::uint64_t const generation) const -> result<void>
{
    auto const current = read_mutation_seq(conn_);
    if (!current) {
        return tl::unexpected {current.error()};
    }
    if (*current != edit.base_mutation_seq || edit.base_mutation_seq != expected_mutation_seq) {
        return tl::unexpected {error {error_code::io_failure, "canonical state changed during the edit"}};
    }
    if (auto applied = edit.apply(); !applied) {
        return tl::unexpected {applied.error()};
    }
    if (injected(fault::point::after_canonical_update, kind, generation, conn_)) {
        return tl::unexpected {error {error_code::io_failure, "injected failure after the canonical update"}};
    }
    if (auto advanced = edit.advance(); !advanced) {
        return tl::unexpected {advanced.error()};
    }
    if (injected(fault::point::after_sequence_bump, kind, generation, conn_)) {
        return tl::unexpected {error {error_code::io_failure, "injected failure after the sequence advance"}};
    }
    return {};
}

void database_manager::install_snapshot(db_manifest next,
                                        std::optional<position_postings> postings,
                                        std::optional<composite_opening_tree> tree,
                                        position_metadata_slot const* const metadata_slot) noexcept
{
    auto const generation = next.publication_generation;
    // Post-commit: the hooks here can only crash or be observed.
    static_cast<void>(fault::inject(fault::point::before_snapshot_install, fault::artifact_kind::none, generation));
    // The edit's verified metadata slot: scalar assignment into the resident
    // table, with no search, allocation or I/O. Readers are excluded by the
    // generation lock the caller holds.
    if (metadata_slot != nullptr && position_postings_) {
        position_postings_->install_metadata_update(*metadata_slot);
    }
    manifest_ = std::move(next);
    if (postings) {
        position_postings_ = std::move(*postings);
    }
    if (manifest_.tree() == nullptr) {
        composite_tree_.reset();
    } else if (tree) {
        composite_tree_ = std::move(*tree);
    }
    static_cast<void>(fault::inject(fault::point::after_snapshot_install, fault::artifact_kind::none, generation));
}

// manifest.json is a cache of SQLite's row: its failure never fails a
// publication or an open, serving continues from the SQLite-sourced snapshot
// already in memory, and the next open (or clean close) retries.
void database_manager::mirror_manifest() noexcept
{
    if (dir_.empty()) {
        return;
    }
    try {
        // Claimed before the write: a failed or interrupted write can leave
        // either file behind, and both are names create() put there.
        note_created_path(dir_ / "manifest.json");
        note_created_path(dir_ / "manifest.json.tmp");
        auto const written = write_manifest(dir_ / "manifest.json", manifest_);
        manifest_cache_repair_pending_ = !written.has_value();
        if (!written.has_value()) {
            if (auto const log = spdlog::get("motif.db"); log != nullptr) {
                log->warn("manifest.json cache could not be fully written; it will be repaired on the next open");
            }
        }
    } catch (...) {
        manifest_cache_repair_pending_ = true;
    }
}

void database_manager::note_created_path(std::filesystem::path path)
{
    if (!created_bundle_ || std::ranges::find(created_paths_, path) != created_paths_.end()) {
        return;
    }
    created_paths_.push_back(std::move(path));
}

// Deletes only files this subsystem owns (see is_owned_artifact_name) that the
// installed snapshot does not reference: superseded generations, a failed
// attempt's staging leftovers, and the artifacts of a pre-v7 bundle once the
// first v7 snapshot has committed. positions.duckdb and every other file are
// never matched. The generation lock held by the caller is what makes deleting
// the previous generation safe: no query can still be reading it.
void database_manager::collect_garbage() noexcept
{
    if (dir_.empty()) {
        return;
    }
    try {
        // Every descriptor of the installed payload is live: the segment, the
        // metadata chain, the tree checkpoint and every patch run. Liveness is
        // derived from SQLite's row only, never from manifest.json.
        auto const referenced_names = referenced_artifact_filenames(manifest_.payload);
        auto const referenced = std::set<std::string> {referenced_names.begin(), referenced_names.end()};
        auto victims = std::vector<std::filesystem::path> {};
        std::error_code fs_err;
        for (auto entries = std::filesystem::directory_iterator {dir_, fs_err};
             !fs_err && entries != std::filesystem::directory_iterator {};
             entries.increment(fs_err))
        {
            std::error_code entry_err;
            // symlink_status: never follow a link out of the bundle.
            if (!std::filesystem::is_regular_file(entries->symlink_status(entry_err)) || entry_err) {
                continue;
            }
            auto const name = entries->path().filename().string();
            if (is_owned_artifact_name(name) && !referenced.contains(name)) {
                victims.push_back(entries->path());
            }
        }
        for (auto const& victim : victims) {
            if (injected(fault::point::before_cleanup)) {
                return;
            }
            fs_err.clear();
            std::filesystem::remove(victim, fs_err);
            static_cast<void>(fault::inject(fault::point::after_cleanup_unlink));
        }
    } catch (...) {
        // Cleanup failure leaks disk space, never correctness.
        log_db_warning("artifact garbage collection failed; superseded files stay until a later publication or open");
    }
}

// Publishes a snapshot with the tree descriptor cleared, for a tree that is
// missing, corrupt or stale while valid postings remain. The reader stays
// disabled even if this commit fails.
void database_manager::clear_unusable_tree() noexcept
{
    try {
        composite_tree_.reset();
        if (manifest_.tree() == nullptr || !manifest_.covered_mutation_seq || !store_ || store_->has_pending_canonical_change()) {
            return;
        }
        auto const seq = store_->mutation_seq();
        if (!seq || manifest_.publication_generation >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return;
        }
        auto payload = manifest_.payload;
        payload.tree.reset();
        auto next = build_manifest(
            manifest_.publication_generation + 1, manifest_.covered_mutation_seq, manifest_.canonical_game_count, std::move(payload));
        if (!next) {
            return;
        }
        auto outcome_unknown = false;
        if (!commit_snapshot(*next, *seq, publication_kind::tree, outcome_unknown)) {
            return;
        }
        install_snapshot(std::move(*next), std::nullopt, std::nullopt);
        mirror_manifest();
        collect_garbage();
    } catch (...) {
        log_db_warning("could not publish a snapshot without the unusable tree; the tree stays disabled in this process");
    }
}

namespace
{

// Decoded-memory bound for the active runs of one chain at open.
constexpr auto chain_resident_cap = max_chain_resident_bytes;

struct loaded_snapshot_readers
{
    std::optional<position_postings> postings;
    std::optional<composite_opening_tree> tree;
    bool metadata_chain_invalid {false};
    // Set only when the segment verified but its metadata chain did not.
    std::optional<position_postings> baseline;
};

// Loads, verifies and merges the metadata chain into one sorted override list.
// Every artifact must match its descriptor's size and checksum, decode strictly,
// carry this bundle's identity and exactly its descriptor's interval and kind;
// no record may outnumber the documents (a document count bound on decoded
// memory) and the active runs' decoded size is capped.
auto load_metadata_overlay(std::filesystem::path const& dir,
                           postings_metadata_chain const& chain,
                           postings_segment const& segment,
                           metadata_artifact_identity const& identity) -> result<std::vector<position_metadata_after_image>>
{
    auto loaded = std::vector<metadata_artifact> {};
    loaded.reserve(chain.runs.size() + 1U);
    auto run_decoded = std::uint64_t {0};
    auto const load_one = [&](patch_run_descriptor const& descriptor, metadata_artifact_kind const kind) -> result<void>
    {
        auto artifact = load_metadata_artifact(dir / descriptor.artifact.filename,
                                               descriptor.artifact,
                                               metadata_artifact_expectation {.identity = identity,
                                                                              .kind = kind,
                                                                              .from_exclusive_seq = descriptor.from_exclusive_seq,
                                                                              .to_inclusive_seq = descriptor.to_inclusive_seq});
        if (!artifact) {
            return tl::unexpected {artifact.error()};
        }
        if (artifact->records.size() > segment.document_count) {
            return tl::unexpected {error {error_code::io_failure, "metadata artifact holds more records than documents"}};
        }
        if (kind == metadata_artifact_kind::run) {
            run_decoded += artifact->decoded_bytes();
            if (run_decoded > chain_resident_cap) {
                return tl::unexpected {error {error_code::io_failure, "metadata runs exceed the resident memory cap"}};
            }
        }
        loaded.push_back(std::move(*artifact));
        return {};
    };
    if (chain.checkpoint) {
        if (auto one = load_one(*chain.checkpoint, metadata_artifact_kind::checkpoint); !one) {
            return tl::unexpected {one.error()};
        }
    }
    for (auto const& run : chain.runs) {
        if (auto one = load_one(run, metadata_artifact_kind::run); !one) {
            return tl::unexpected {one.error()};
        }
    }
    auto pointers = std::vector<metadata_artifact const*> {};
    pointers.reserve(loaded.size());
    for (auto const& artifact : loaded) {
        pointers.push_back(&artifact);
    }
    return merge_metadata_records(pointers);
}

// Loads the composite tree: checkpoint checksum, every run (checksum, strict
// decode, identity and interval), then the merged-invariant validation. All or
// nothing.
auto load_composite_tree(std::filesystem::path const& dir,
                         db_manifest const& manifest,
                         tree_descriptor const& tree,
                         identity_bytes const& bundle_id) -> result<composite_opening_tree>
{
    if (!artifact_matches(dir, tree.checkpoint)) {
        return tl::unexpected {error {error_code::io_failure, "tree checkpoint does not match its descriptor"}};
    }
    auto runs = std::vector<tree_patch_run_ptr> {};
    runs.reserve(tree.patch_runs.size());
    auto decoded = std::uint64_t {0};
    for (auto const& descriptor : tree.patch_runs) {
        auto run = load_tree_patch_run(dir / descriptor.artifact.filename,
                                       descriptor.artifact,
                                       tree_patch_expectation {.identity = {.bundle_id = bundle_id,
                                                                            .index_semantics = index_semantics_version,
                                                                            .canonical_start_hash = opening_tree_start_hash(),
                                                                            .max_root_ply = tree.max_root_ply},
                                                               .from_exclusive_seq = descriptor.from_exclusive_seq,
                                                               .to_inclusive_seq = descriptor.to_inclusive_seq});
        if (!run) {
            return tl::unexpected {run.error()};
        }
        decoded += run->decoded_bytes();
        if (decoded > chain_resident_cap) {
            return tl::unexpected {error {error_code::io_failure, "tree patch runs exceed the resident memory cap"}};
        }
        runs.push_back(std::make_shared<tree_patch_run const>(std::move(*run)));
    }
    auto opened =
        composite_opening_tree::open(dir / tree.checkpoint.filename, std::move(runs), tree.child_frequency_mode, chain_resident_cap);
    if (!opened) {
        return tl::unexpected {opened.error()};
    }
    if (opened->source_game_count() != manifest.canonical_game_count || opened->max_root_ply() != tree.max_root_ply) {
        return tl::unexpected {error {error_code::io_failure, "tree disagrees with the snapshot"}};
    }
    return opened;
}

// Verifies and opens a snapshot's artifacts. Only a snapshot that already covers
// the current sequence is worth the full-file checksum; a stale one is about to
// be replaced. A reader is produced only for an artifact (and chain) whose size
// and FNV checksum match its descriptor, whose structure validates, and whose
// own game count matches the snapshot's build-time canonical count. Postings
// need segment AND metadata chain; the tree needs its checkpoint AND every run.
auto load_snapshot_readers(std::filesystem::path const& dir, db_manifest const& manifest, std::uint64_t const current_mutation_seq)
    -> loaded_snapshot_readers
{
    auto outcome = loaded_snapshot_readers {};
    auto const* segment = manifest.postings();
    if (segment == nullptr || !manifest.covered_mutation_seq || *manifest.covered_mutation_seq != current_mutation_seq) {
        return outcome;
    }
    if (!artifact_matches(dir, segment->artifact)) {
        return outcome;
    }
    auto postings = position_postings {dir / segment->artifact.filename};
    if (!postings.open() || postings.indexed_game_count() != segment->document_count) {
        return outcome;
    }
    auto const bundle_id = identity_from_hex(manifest.payload.bundle.id);
    if (!bundle_id) {
        return outcome;
    }

    if (!manifest.payload.metadata.empty()) {
        auto overlay =
            load_metadata_overlay(dir,
                                  manifest.payload.metadata,
                                  *segment,
                                  metadata_artifact_identity {.bundle_id = *bundle_id, .index_semantics = index_semantics_version});
        // Never serve the embedded baseline as if it were current, and never
        // apply only a surviving suffix: a bad chain disables postings.
        if (!overlay || !postings.apply_metadata_overlay(*overlay)) {
            outcome.metadata_chain_invalid = true;
            // apply_metadata_overlay is all-or-nothing, so the reader is still
            // the pure baseline when only the overlay failed.
            if (overlay) {
                postings = position_postings {dir / segment->artifact.filename};
                if (!postings.open()) {
                    return outcome;
                }
            }
            outcome.baseline = std::move(postings);
        } else {
            outcome.postings = std::move(postings);
        }
    } else {
        outcome.postings = std::move(postings);
    }

    auto const* tree = manifest.tree();
    if (tree == nullptr || tree->covered_mutation_seq != current_mutation_seq) {
        return outcome;
    }
    // The tree does not depend on the metadata chain: a bad chain must not take
    // a healthy tree with it, and a bad tree must not take healthy postings.
    if (auto opened = load_composite_tree(dir, manifest, *tree, *bundle_id); opened) {
        outcome.tree = std::move(*opened);
    }
    return outcome;
}

}  // namespace

auto database_manager::load_readers(std::uint64_t const current_mutation_seq) -> reader_load_outcome
{
    position_postings_.reset();
    composite_tree_.reset();
    auto loaded = load_snapshot_readers(dir_, manifest_, current_mutation_seq);
    position_postings_ = std::move(loaded.postings);
    composite_tree_ = std::move(loaded.tree);
    return reader_load_outcome {.metadata_chain_invalid = loaded.metadata_chain_invalid, .baseline = std::move(loaded.baseline)};
}

// ── Composite snapshot chains ────────────────────────────────────────────────

namespace
{

auto family_prefix(derived_artifact_family const family) noexcept -> std::string_view
{
    switch (family) {
        case derived_artifact_family::postings_metadata:
            return postings_metadata_artifact_prefix;
        case derived_artifact_family::tree_checkpoint:
            return tree_artifact_prefix;
        case derived_artifact_family::tree_patch:
            return tree_patch_artifact_prefix;
    }
    return {};
}

auto family_format(derived_artifact_family const family) noexcept -> std::string_view
{
    switch (family) {
        case derived_artifact_family::postings_metadata:
            return postings_metadata_format_name;
        case derived_artifact_family::tree_checkpoint:
            return tree_format_name;
        case derived_artifact_family::tree_patch:
            return tree_patch_format_name;
    }
    return {};
}

auto family_version(derived_artifact_family const family) noexcept -> std::uint32_t
{
    switch (family) {
        case derived_artifact_family::postings_metadata:
            return postings_metadata_format_version;
        case derived_artifact_family::tree_checkpoint:
            return opening_tree_index_format_version;
        case derived_artifact_family::tree_patch:
            return tree_patch_format_version;
    }
    return 0;
}

// Runs its action unless released: removes a failed attempt's own files.
template<typename Action>
class scope_exit
{
  public:
    explicit scope_exit(Action action)
        : action_ {std::move(action)}
    {
    }

    ~scope_exit()
    {
        if (active_) {
            action_();
        }
    }

    void release() noexcept { active_ = false; }

    scope_exit(scope_exit const&) = delete;
    auto operator=(scope_exit const&) -> scope_exit& = delete;
    scope_exit(scope_exit&&) = delete;
    auto operator=(scope_exit&&) -> scope_exit& = delete;

  private:
    Action action_;
    bool active_ {true};
};

// What a failed publication step reports: an io_failure carrying `what`.
auto io_failure_because(char const* const what) -> tl::unexpected<error>
{
    return tl::unexpected {error {error_code::io_failure, what}};
}

// Removes a failed attempt's staged files; a renamed one also loses its final
// name.
void remove_staged_files(std::vector<staged_derived_artifact> const& staged, std::vector<bool> const& installed) noexcept
{
    std::error_code fs_err;
    for (std::size_t index = 0; index < staged.size(); ++index) {
        std::filesystem::remove(staged[index].staging_path, fs_err);
        if (installed[index]) {
            fs_err.clear();
            std::filesystem::remove(staged[index].final_path, fs_err);
        }
    }
}

// Every staged file must be referenced by the new payload but not by the
// installed one, and its bytes must be exactly what its descriptor promises.
auto verify_staged_artifacts(std::vector<staged_derived_artifact> const& staged, db_manifest const& next, snapshot_payload const& current)
    -> result<void>
{
    auto const referenced_names = referenced_artifact_filenames(next.payload);
    auto const already_referenced_names = referenced_artifact_filenames(current);
    for (auto const& file : staged) {
        if (std::ranges::find(referenced_names, file.filename) == referenced_names.end()
            || std::ranges::find(already_referenced_names, file.filename) != already_referenced_names.end())
        {
            return io_failure_because("a staged artifact is not newly referenced by the payload");
        }
    }

    auto const descriptors = referenced_artifacts(next.payload);
    for (auto const& file : staged) {
        auto const sums = file_checksum(file.staging_path);
        if (!sums) {
            return tl::unexpected {sums.error()};
        }
        auto const matched = std::ranges::any_of(descriptors,
                                                 [&](artifact_descriptor const& descriptor) -> bool
                                                 {
                                                     return descriptor.filename == file.filename && descriptor.size_bytes == sums->first
                                                         && descriptor.checksum == sums->second;
                                                 });
        if (!matched) {
            return io_failure_because("a staged artifact disagrees with its descriptor");
        }
    }
    return {};
}

// Renames every staged file to its final name without replacing anything, then
// fsyncs the directory. `installed` records each rename so a failure can undo it.
auto install_staged_artifacts(std::filesystem::path const& dir,
                              std::vector<staged_derived_artifact> const& staged,
                              std::vector<bool>& installed,
                              fault::artifact_kind const kind,
                              std::uint64_t const generation) -> result<void>
{
    if (injected(fault::point::before_artifact_rename, kind, generation)) {
        return io_failure_because("injected failure before artifact rename");
    }
    for (std::size_t index = 0; index < staged.size(); ++index) {
        if (!rename_no_replace(staged[index].staging_path, staged[index].final_path)) {
            return io_failure_because("artifact rename failed or its final name already exists");
        }
        installed[index] = true;
    }
    if (injected(fault::point::after_artifact_rename, kind, generation)) {
        return io_failure_because("injected failure after artifact rename");
    }
    if (injected(fault::point::before_artifact_directory_fsync, kind, generation)) {
        return io_failure_because("injected failure before directory fsync");
    }
    if (auto dir_sync = sync_directory(dir); !dir_sync) {
        return tl::unexpected {dir_sync.error()};
    }
    if (injected(fault::point::after_artifact_directory_fsync, kind, generation)) {
        return io_failure_because("injected failure after directory fsync");
    }
    return {};
}

// The shared tail of a rebuild's artifact: fsync the built staging file, take
// its size and checksum, rename it to its final name (never replacing) and fsync
// the directory. Returns {size, FNV-1a-64 checksum}.
auto install_built_artifact(std::filesystem::path const& dir,
                            staged_artifact& artifact,
                            fault::artifact_kind const kind,
                            std::uint64_t const generation) -> result<std::pair<std::uint64_t, std::uint64_t>>
{
    if (injected(fault::point::before_artifact_fsync, kind, generation)) {
        return io_failure_because("injected failure before artifact fsync");
    }
    if (auto sync_res = sync_file(artifact.staging_path); !sync_res) {
        return tl::unexpected {sync_res.error()};
    }
    auto checksum = file_checksum(artifact.staging_path);
    if (!checksum) {
        return tl::unexpected {checksum.error()};
    }
    if (injected(fault::point::after_artifact_fsync, kind, generation)) {
        return io_failure_because("injected failure after artifact fsync");
    }

    if (injected(fault::point::before_artifact_rename, kind, generation)) {
        return io_failure_because("injected failure before artifact rename");
    }
    if (!rename_no_replace(artifact.staging_path, artifact.final_path)) {
        return io_failure_because("artifact rename failed or its final name already exists");
    }
    artifact.installed = true;
    if (injected(fault::point::after_artifact_rename, kind, generation)) {
        return io_failure_because("injected failure after artifact rename");
    }
    // The synced inode is not a durable referenced artifact until its directory
    // entry is durable too, and the COMMIT that follows must not outrun that.
    if (injected(fault::point::before_artifact_directory_fsync, kind, generation)) {
        return io_failure_because("injected failure before directory fsync");
    }
    if (auto dir_sync = sync_directory(dir); !dir_sync) {
        return tl::unexpected {dir_sync.error()};
    }
    if (injected(fault::point::after_artifact_directory_fsync, kind, generation)) {
        return io_failure_because("injected failure after directory fsync");
    }
    return checksum;
}

// The staged postings file must open and index exactly the canonical games.
auto validate_postings_candidate(std::filesystem::path const& path, std::uint64_t const game_count) -> result<void>
{
    auto staged = position_postings {path};
    if (auto open_res = staged.open(); !open_res) {
        return tl::unexpected {open_res.error()};
    }
    if (staged.indexed_game_count() != game_count) {
        return io_failure_because("postings game count disagrees with canonical SQLite");
    }
    return {};
}

// The staged tree file must open and aggregate exactly the canonical games.
auto validate_tree_candidate(std::filesystem::path const& path, std::uint64_t const game_count) -> result<void>
{
    auto const staged = opening_tree_index::open(path);
    if (!staged) {
        return tl::unexpected {staged.error()};
    }
    if (staged->source_game_count() != game_count) {
        return io_failure_because("tree game count disagrees with canonical SQLite");
    }
    return {};
}

// Folds the live tree chain plus one more run into a single checkpoint at path.
auto write_folded_tree_checkpoint(composite_opening_tree const& live,
                                  std::vector<tree_patch_run_ptr> const& runs,
                                  std::filesystem::path const& path) -> result<void>
{
    auto folded = fold_tree_chain(live.checkpoint(), runs, live.child_frequency_mode(), path);
    if (!folded) {
        return tl::unexpected {folded.error()};
    }
    return {};
}

// What the post-rename validation of a freshly folded checkpoint needs.
struct checkpoint_loader_spec
{
    std::filesystem::path final_path;
    tree_child_frequency_mode mode {tree_child_frequency_mode::checkpoint};
    std::uint64_t decoded_byte_cap {};
    std::uint64_t source_games {};
    std::uint64_t generation {};
};

// A reader loader that opens and validates the folded checkpoint under its
// final name, so a checkpoint is never committed before it proved loadable.
auto make_checkpoint_loader(checkpoint_loader_spec spec) -> std::function<result<std::optional<composite_opening_tree>>()>
{
    return [spec = std::move(spec)]() -> result<std::optional<composite_opening_tree>>
    {
        if (injected(fault::point::before_artifact_validation, fault::artifact_kind::edit, spec.generation)) {
            return tl::unexpected {error {error_code::io_failure, "injected failure before the folded checkpoint is validated"}};
        }
        auto opened = composite_opening_tree::open(spec.final_path, {}, spec.mode, spec.decoded_byte_cap);
        if (!opened) {
            return tl::unexpected {opened.error()};
        }
        if (opened->source_game_count() != spec.source_games) {
            return tl::unexpected {error {error_code::io_failure, "the folded checkpoint disagrees with the snapshot's game count"}};
        }
        if (injected(fault::point::after_artifact_validation, fault::artifact_kind::edit, spec.generation)) {
            return tl::unexpected {error {error_code::io_failure, "injected failure after the folded checkpoint is validated"}};
        }
        return std::optional<composite_opening_tree> {std::move(*opened)};
    };
}

}  // namespace

auto database_manager::reserve_derived_artifact(derived_artifact_family const family) -> result<staged_derived_artifact>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (dir_.empty() || !store_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto reserved = reserve_staged_artifact(dir_, family_prefix(family));
    if (!reserved) {
        return tl::unexpected {reserved.error()};
    }
    note_created_path(reserved->staging_path);
    note_created_path(reserved->final_path);
    return staged_derived_artifact {.id = std::move(reserved->id),
                                    .filename = std::move(reserved->filename),
                                    .staging_path = std::move(reserved->staging_path),
                                    .final_path = std::move(reserved->final_path)};
}

auto database_manager::seal_derived_artifact(staged_derived_artifact const& staged, derived_artifact_family const family)
    -> result<artifact_descriptor>
{
    if (auto synced = sync_file(staged.staging_path); !synced) {
        return tl::unexpected {synced.error()};
    }
    auto const checksum = file_checksum(staged.staging_path);
    if (!checksum) {
        return tl::unexpected {checksum.error()};
    }
    return artifact_descriptor {.id = staged.id,
                                .filename = staged.filename,
                                .format = std::string {family_format(family)},
                                .format_version = family_version(family),
                                .size_bytes = checksum->first,
                                .checksum = checksum->second};
}

void database_manager::discard_derived_artifact(staged_derived_artifact const& staged) noexcept
{
    std::error_code fs_err;
    std::filesystem::remove(staged.staging_path, fs_err);
    fs_err.clear();
    std::filesystem::remove(staged.final_path, fs_err);
}

auto database_manager::publish_snapshot_chain(snapshot_chain_publication publication) -> result<void>
{
    return publish_prepared(std::move(publication.payload), std::move(publication.staged), prepared_readers {.load_from_disk = true});
}

void database_manager::set_chain_limits(patch_chain_limits limits) noexcept
{
    // open() rejects a chain longer than max_patch_chain_runs or holding more than
    // max_chain_resident_bytes of decoded runs. Admission above those bounds would
    // publish a snapshot the next open discards, so the bounds are the ceiling.
    limits.hard_runs = std::min(limits.hard_runs, max_patch_chain_runs);
    limits.hard_decoded_bytes = std::min(limits.hard_decoded_bytes, max_chain_resident_bytes);
    chain_limits_ = limits;
}

// What a publication reads before it writes anything.
struct database_manager::publication_gate
{
    game_store* store {nullptr};
    std::uint64_t mutation_seq {};
    std::uint64_t game_count {};
    std::uint64_t generation {};
};

auto database_manager::begin_publication(canonical_edit_steps const* const edit) -> result<publication_gate>
{
    if (dir_.empty() || !store_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (store_->has_pending_canonical_change() || sqlite3_get_autocommit(conn_) == 0) {
        return tl::unexpected {error {error_code::invalid_argument, "cannot publish while a canonical transaction is open"}};
    }
    auto const seq = store_->mutation_seq();
    if (!seq) {
        return tl::unexpected {seq.error()};
    }
    auto const game_count = store_->count_games();
    if (!game_count || *game_count < 0) {
        return tl::unexpected {error_code::io_failure};
    }
    constexpr auto int64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (manifest_.publication_generation >= int64_max) {
        return tl::unexpected {error {error_code::io_failure, "publication generation exhausted"}};
    }
    if (edit != nullptr && (*seq != edit->base_mutation_seq || *seq >= int64_max - 1U)) {
        return tl::unexpected {
            error {error_code::io_failure, "canonical state changed during the edit, or the mutation sequence is exhausted"}};
    }
    return publication_gate {.store = &*store_,
                             .mutation_seq = *seq,
                             .game_count = static_cast<std::uint64_t>(*game_count),
                             .generation = manifest_.publication_generation + 1U};
}

auto database_manager::materialize_readers(prepared_readers& readers, db_manifest const& next, std::uint64_t const mutation_seq) const
    -> result<void>
{
    if (readers.load_from_disk) {
        auto loaded = load_snapshot_readers(dir_, next, mutation_seq);
        if (!loaded.postings) {
            return io_failure_because("the snapshot's postings or metadata chain did not verify");
        }
        if (next.tree() != nullptr && !loaded.tree) {
            return io_failure_because("the snapshot's tree chain did not verify");
        }
        readers.postings = std::move(loaded.postings);
        readers.tree = std::move(loaded.tree);
    }
    if (readers.tree_loader) {
        auto loaded = readers.tree_loader();
        if (!loaded) {
            return tl::unexpected {loaded.error()};
        }
        readers.tree = std::move(*loaded);
    }
    return {};
}

// Same protocol and failure boundaries as rebuild_position_postings():
// rename each staged file without replacing -> fsync the directory -> load and
// verify the reader state the payload describes -> FULL COMMIT (the only
// publication point) -> install -> mirror -> collect garbage. A failure before
// COMMIT removes only this attempt's own files; an unknown COMMIT outcome keeps
// them.
auto database_manager::publish_prepared(snapshot_payload payload,
                                        std::vector<staged_derived_artifact> staged,
                                        prepared_readers readers,
                                        prepared_edit* const edit) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    auto installed = std::vector<bool>(staged.size(), false);
    auto cleanup = scope_exit {[&staged, &installed]() noexcept -> void { remove_staged_files(staged, installed); }};
    auto const gate = begin_publication(edit != nullptr ? &edit->steps : nullptr);
    if (!gate) {
        return tl::unexpected {gate.error()};
    }
    auto const kind = edit != nullptr ? fault::artifact_kind::edit : fault::artifact_kind::chain;

    // build_manifest validates the payload structure (chains reach the covered
    // sequence, intervals are contiguous, every artifact is referenced once).
    // An edit's snapshot covers the sequence its own transaction advances to.
    auto next = build_manifest(gate->generation, gate->mutation_seq + (edit != nullptr ? 1U : 0U), gate->game_count, std::move(payload));
    if (!next) {
        return tl::unexpected {next.error()};
    }
    if (auto verified = verify_staged_artifacts(staged, *next, manifest_.payload); !verified) {
        return verified;
    }
    if (auto renamed = install_staged_artifacts(dir_, staged, installed, kind, gate->generation); !renamed) {
        return renamed;
    }
    if (auto materialized = materialize_readers(readers, *next, gate->mutation_seq); !materialized) {
        return materialized;
    }

    auto outcome_unknown = false;
    auto const publication = edit != nullptr ? publication_kind::edit : publication_kind::chain;
    if (auto committed = commit_snapshot(*next, gate->mutation_seq, publication, outcome_unknown, edit != nullptr ? &edit->steps : nullptr);
        !committed)
    {
        if (edit != nullptr) {
            // The rolled-back (or unknown) transaction may have filled the id
            // caches with rows that do not exist; copies of canonical data a
            // reader holds may be stale if the outcome is unknown.
            gate->store->discard_id_caches();
            if (outcome_unknown) {
                gate->store->note_data_changed();
            }
        }
        if (outcome_unknown) {
            // Never delete a potentially committed artifact on an error return.
            cleanup.release();
        }
        return tl::unexpected {committed.error()};
    }
    cleanup.release();

    install_snapshot(std::move(*next), std::move(readers.postings), std::move(readers.tree), edit != nullptr ? &edit->slot : nullptr);
    if (edit != nullptr) {
        // Anything a cache copied out of the canonical store is stale now.
        gate->store->note_data_changed();
    }
    mirror_manifest();
    collect_garbage();
    return {};
}

// The narrow canonical repair for a corrupt metadata chain: with queries
// disabled and the lock held, scan canonical id/revision/result/ratings in
// lockstep with the verified baseline, require identical membership, write the
// differences as one sparse checkpoint at the current sequence and FULL-publish
// it. No chess move is replayed.
auto database_manager::repair_metadata_chain(position_postings baseline) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (dir_.empty() || !store_ || store_->has_pending_canonical_change() || sqlite3_get_autocommit(conn_) == 0) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto const* segment = manifest_.postings();
    auto const seq = store_->mutation_seq();
    if (!seq) {
        return tl::unexpected {seq.error()};
    }
    auto const bundle_id = identity_from_hex(manifest_.payload.bundle.id);
    if (segment == nullptr || !bundle_id || !manifest_.covered_mutation_seq || *manifest_.covered_mutation_seq != *seq
        || segment->source_mutation_seq >= *seq)
    {
        return tl::unexpected {error {error_code::invalid_argument, "metadata chain repair does not apply"}};
    }

    auto baseline_images = std::vector<position_metadata_after_image> {};
    baseline_images.reserve(static_cast<std::size_t>(segment->document_count));
    baseline.for_each_metadata([&baseline_images](position_metadata_after_image const& image) -> void
                               { baseline_images.push_back(image); });

    auto overrides = std::vector<position_metadata_after_image> {};
    auto cursor = std::size_t {0};
    auto const scanned = store_->for_each_metadata_row(
        [&](canonical_metadata_row const& row) -> result<void>
        {
            if (cursor >= baseline_images.size() || baseline_images[cursor].id != row.id || row.content_revision != 1U) {
                return tl::unexpected {error {error_code::io_failure, "canonical membership differs from the verified baseline"}};
            }
            auto const white = narrow_elo(row.white_elo);
            auto const black = narrow_elo(row.black_elo);
            if (!white || !black) {
                return tl::unexpected {error {error_code::io_failure, "rating outside the signed 16-bit domain"}};
            }
            auto const image = position_metadata_after_image {
                .id = row.id, .content_revision = 1, .result = encode_result_code(row.result), .white_elo = *white, .black_elo = *black};
            if (image != baseline_images[cursor]) {
                overrides.push_back(image);
            }
            ++cursor;
            return {};
        });
    if (!scanned) {
        return tl::unexpected {scanned.error()};
    }
    if (cursor != baseline_images.size()) {
        return tl::unexpected {error {error_code::io_failure, "canonical membership differs from the verified baseline"}};
    }

    auto artifact = metadata_artifact {.identity = {.bundle_id = *bundle_id, .index_semantics = index_semantics_version},
                                       .kind = metadata_artifact_kind::checkpoint,
                                       .from_exclusive_seq = segment->source_mutation_seq,
                                       .to_inclusive_seq = *seq,
                                       .records = overrides,
                                       .encoded_bytes = 0};
    auto staged = reserve_derived_artifact(derived_artifact_family::postings_metadata);
    if (!staged) {
        return tl::unexpected {staged.error()};
    }
    if (auto written = write_metadata_artifact(staged->staging_path, artifact); !written) {
        discard_derived_artifact(*staged);
        return tl::unexpected {written.error()};
    }
    auto descriptor = seal_derived_artifact(*staged, derived_artifact_family::postings_metadata);
    if (!descriptor) {
        discard_derived_artifact(*staged);
        return tl::unexpected {descriptor.error()};
    }
    if (auto applied = baseline.apply_metadata_overlay(overrides); !applied) {
        discard_derived_artifact(*staged);
        return tl::unexpected {applied.error()};
    }
    auto payload = manifest_.payload;
    payload.metadata = postings_metadata_chain {.checkpoint = patch_run_descriptor {.artifact = std::move(*descriptor),
                                                                                    .from_exclusive_seq = segment->source_mutation_seq,
                                                                                    .to_inclusive_seq = *seq},
                                                .runs = {}};
    auto staged_files = std::vector<staged_derived_artifact> {};
    staged_files.push_back(std::move(*staged));
    return publish_prepared(std::move(payload), std::move(staged_files), prepared_readers {.postings = std::move(baseline)});
}

auto database_manager::query_position_matches(zobrist_hash const hash, std::size_t const limit, std::size_t const offset) const
    -> result<std::vector<position_match>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    auto const postings = usable_postings();
    if (!postings) {
        return tl::unexpected {postings.error()};
    }
    auto const& reader = **postings;
    auto matches = reader.occurrences(hash, limit, offset);
    if (!matches) {
        return tl::unexpected {matches.error()};
    }
    return matches;
}

auto database_manager::query_position_first_matches(zobrist_hash const hash, std::span<game_id const> const game_ids) const
    -> result<std::vector<position_match>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    // Coverage is required even when game_ids is empty: an empty argument
    // must not become a way to read an unavailable index.
    auto const postings = usable_postings();
    if (!postings) {
        return tl::unexpected {postings.error()};
    }
    auto const& reader = **postings;
    auto matches = reader.first_occurrences(hash, game_ids);
    if (!matches) {
        return tl::unexpected {matches.error()};
    }
    return matches;
}

auto database_manager::position_summary(zobrist_hash const hash) const -> result<std::optional<position_postings_summary>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    // Stale or unbuilt is an error, not "hash absent": a successful nullopt
    // below means the hash is missing from a valid, current snapshot.
    auto const postings = usable_postings();
    if (!postings) {
        return tl::unexpected {postings.error()};
    }
    return (*postings)->summary(hash);
}

// Builds the replacement postings file under a unique, exclusively reserved
// name and publishes it. Order, with the crash behavior of each boundary:
//
//   build -> validate -> fsync file -> rename (no replace) -> fsync directory
//     failure/crash anywhere up to here: nothing references the candidate; the
//     previous snapshot stays authoritative; the orphan is deleted by the next
//     publication or open (never adopted).
//   -> SQLite COMMIT of the new published_snapshot row (FULL synchronous)
//     the only publication point; durable and final once it returns.
//   -> install readers -> manifest.json mirror -> garbage-collect superseded files
//     none of these can fail the publication.
//
// The sequence captured at entry is the coverage published: the generation
// lock is held throughout and a write transaction is refused, and the commit
// re-checks the sequence inside its transaction.
auto database_manager::rebuild_position_postings(position_postings_build_metrics* const metrics) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    auto const gate = begin_publication();
    if (!gate) {
        return tl::unexpected {gate.error()};
    }
    constexpr auto kind = fault::artifact_kind::postings;

    auto reserved = reserve_staged_artifact(dir_, postings_artifact_prefix);
    if (!reserved) {
        return tl::unexpected {reserved.error()};
    }
    auto artifact = std::move(*reserved);
    auto cleanup = attempt_cleanup {artifact};
    note_created_path(artifact.staging_path);
    note_created_path(artifact.final_path);

    if (injected(fault::point::before_build, kind, gate->generation)) {
        return io_failure_because("injected failure before build");
    }
    if (auto build_res = position_postings::build(*gate->store, artifact.staging_path, position_postings::default_spill_threshold, metrics);
        !build_res)
    {
        return tl::unexpected {build_res.error()};
    }
    if (injected(fault::point::after_build, kind, gate->generation)) {
        return io_failure_because("injected failure after build");
    }

    if (injected(fault::point::before_artifact_validation, kind, gate->generation)) {
        return io_failure_because("injected failure before artifact validation");
    }
    if (auto validated = validate_postings_candidate(artifact.staging_path, gate->game_count); !validated) {
        return validated;
    }
    if (injected(fault::point::after_artifact_validation, kind, gate->generation)) {
        return io_failure_because("injected failure after artifact validation");
    }

    auto const checksum = install_built_artifact(dir_, artifact, kind, gate->generation);
    if (!checksum) {
        return tl::unexpected {checksum.error()};
    }

    auto live_reader = position_postings {artifact.final_path};
    if (auto open_res = live_reader.open(); !open_res) {
        return tl::unexpected {open_res.error()};
    }

    // Prepare everything the commit and the install need before the commit: the
    // new payload (bundle identity preserved, one postings segment, the tree
    // only if it is already valid at this exact sequence) and its checksum.
    auto payload = manifest_.payload;
    payload.segments.clear();
    // The new segment embeds the current canonical metadata at this sequence, so
    // the old chain's after-images must not be reapplied to it.
    payload.metadata = {};
    payload.segments.push_back(postings_segment {.artifact = {.id = artifact.id,
                                                              .filename = artifact.filename,
                                                              .format = std::string {postings_format_name},
                                                              .format_version = position_postings_format_version,
                                                              .size_bytes = checksum->first,
                                                              .checksum = checksum->second},
                                                 .source_mutation_seq = gate->mutation_seq,
                                                 .document_count = gate->game_count,
                                                 .min_content_revision = 1,
                                                 .max_content_revision = 1});
    // An already-usable composite tree (checkpoint, runs and covered sequence) is
    // preserved untouched; anything else is cleared.
    auto const keep_tree = composite_tree_.has_value() && payload.tree && payload.tree->covered_mutation_seq == gate->mutation_seq;
    if (!keep_tree) {
        payload.tree.reset();
    }
    auto next = build_manifest(gate->generation, gate->mutation_seq, gate->game_count, std::move(payload));
    if (!next) {
        return tl::unexpected {next.error()};
    }

    auto outcome_unknown = false;
    if (auto committed = commit_snapshot(*next, gate->mutation_seq, publication_kind::postings, outcome_unknown); !committed) {
        if (outcome_unknown) {
            // Never delete a potentially committed artifact on an error return.
            cleanup.release();
        }
        return tl::unexpected {committed.error()};
    }
    cleanup.release();

    install_snapshot(std::move(*next), std::move(live_reader), std::nullopt);
    mirror_manifest();
    collect_garbage();
    return {};
}

// Same protocol as rebuild_position_postings() (see its comment). A tree
// rebuild requires valid postings at the current sequence and changes only the
// tree descriptor; the postings segment and coverage are carried over.
auto database_manager::rebuild_opening_tree_index(opening_tree_index_build_metrics* const metrics) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (dir_.empty() || !store_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto const coverage = coverage_locked();
    if (!coverage) {
        return tl::unexpected {coverage.error()};
    }
    if (!coverage->postings_usable || !position_postings_ || sqlite3_get_autocommit(conn_) == 0) {
        return tl::unexpected {error_code::invalid_argument};
    }
    auto const& store = *store_;
    auto const& postings = *position_postings_;
    auto const seq = coverage->mutation_seq;
    auto const game_count = store.count_games();
    if (!game_count || *game_count < 0 || static_cast<std::uint64_t>(*game_count) != postings.indexed_game_count()) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (manifest_.publication_generation >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return tl::unexpected {error {error_code::io_failure, "publication generation exhausted"}};
    }
    auto const generation = manifest_.publication_generation + 1;
    constexpr auto kind = fault::artifact_kind::tree;

    auto reserved = reserve_staged_artifact(dir_, tree_artifact_prefix);
    if (!reserved) {
        return tl::unexpected {reserved.error()};
    }
    auto artifact = std::move(*reserved);
    auto cleanup = attempt_cleanup {artifact};
    note_created_path(artifact.staging_path);
    note_created_path(artifact.final_path);

    if (injected(fault::point::before_build, kind, generation)) {
        return io_failure_because("injected failure before build");
    }
    auto child_counts = opening_tree_child_count_stream {
        [&postings](opening_tree_child_count_visitor const& visitor) -> result<void>
        {
            return postings.for_each_summary([&visitor](zobrist_hash const hash, position_postings_summary const& summary) -> result<void>
                                             { return visitor(hash, summary.distinct_game_count); });
        }};
    if (auto build_res = opening_tree_index::build(store, artifact.staging_path, {}, metrics, std::move(child_counts)); !build_res) {
        return tl::unexpected {build_res.error()};
    }
    if (injected(fault::point::after_build, kind, generation)) {
        return io_failure_because("injected failure after build");
    }

    if (injected(fault::point::before_artifact_validation, kind, generation)) {
        return io_failure_because("injected failure before artifact validation");
    }
    if (auto validated = validate_tree_candidate(artifact.staging_path, static_cast<std::uint64_t>(*game_count)); !validated) {
        return validated;
    }
    if (injected(fault::point::after_artifact_validation, kind, generation)) {
        return io_failure_because("injected failure after artifact validation");
    }

    auto const checksum = install_built_artifact(dir_, artifact, kind, generation);
    if (!checksum) {
        return tl::unexpected {checksum.error()};
    }

    auto live_index = composite_opening_tree::open(artifact.final_path, {}, tree_child_frequency_mode::checkpoint);
    if (!live_index) {
        return tl::unexpected {live_index.error()};
    }

    auto payload = manifest_.payload;
    payload.tree = tree_descriptor {.covered_mutation_seq = seq,
                                    .max_root_ply = live_index->max_root_ply(),
                                    .completeness_policy = std::string {tree_completeness_policy_name},
                                    .checkpoint = {.id = artifact.id,
                                                   .filename = artifact.filename,
                                                   .format = std::string {tree_format_name},
                                                   .format_version = opening_tree_index_format_version,
                                                   .size_bytes = checksum->first,
                                                   .checksum = checksum->second},
                                    .base_mutation_seq = seq};
    auto next = build_manifest(generation, manifest_.covered_mutation_seq, manifest_.canonical_game_count, std::move(payload));
    if (!next) {
        return tl::unexpected {next.error()};
    }

    auto outcome_unknown = false;
    if (auto committed = commit_snapshot(*next, seq, publication_kind::tree, outcome_unknown); !committed) {
        if (outcome_unknown) {
            cleanup.release();
        }
        return tl::unexpected {committed.error()};
    }
    cleanup.release();

    install_snapshot(std::move(*next), std::nullopt, std::move(*live_index));
    mirror_manifest();
    collect_garbage();
    return {};
}

auto database_manager::usable_postings() const -> result<position_postings const*>
{
    if (auto usable = require_postings_usable(); !usable) {
        return tl::unexpected {usable.error()};
    }
    // Usable coverage already requires an installed reader; the check keeps the
    // access explicit rather than assumed.
    if (!position_postings_) {
        return tl::unexpected {error_code::io_failure};
    }
    return &*position_postings_;
}

auto database_manager::position_game_ids(zobrist_hash const hash) const -> result<std::vector<game_id>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    auto const postings = usable_postings();
    if (!postings) {
        return tl::unexpected {postings.error()};
    }
    return (*postings)->distinct_game_ids(hash);
}

namespace
{

// A metadata run is not kept decoded; its resident cost once materialized is its
// record count times the decoded record size.
auto estimated_metadata_decoded_bytes(artifact_descriptor const& artifact) -> std::uint64_t
{
    return (artifact.size_bytes / metadata_record_bytes + 1U) * sizeof(position_metadata_after_image);
}

auto metadata_chain_usage(postings_metadata_chain const& chain) -> patch_chain_usage
{
    auto usage = patch_chain_usage {};
    usage.runs = chain.runs.size();
    for (auto const& run : chain.runs) {
        usage.encoded_bytes += run.artifact.size_bytes;
        usage.decoded_bytes += estimated_metadata_decoded_bytes(run.artifact);
    }
    return usage;
}

// Loads the metadata chain's artifacts in chronological order (checkpoint first),
// each verified against its descriptor, for a fold.
auto load_metadata_chain_artifacts(std::filesystem::path const& dir,
                                   postings_metadata_chain const& chain,
                                   metadata_artifact_identity const& identity) -> result<std::vector<metadata_artifact>>
{
    auto loaded = std::vector<metadata_artifact> {};
    loaded.reserve(chain.runs.size() + 1U);
    auto const load_one = [&](patch_run_descriptor const& member, metadata_artifact_kind const kind) -> result<void>
    {
        auto artifact = load_metadata_artifact(dir / member.artifact.filename,
                                               member.artifact,
                                               metadata_artifact_expectation {.identity = identity,
                                                                              .kind = kind,
                                                                              .from_exclusive_seq = member.from_exclusive_seq,
                                                                              .to_inclusive_seq = member.to_inclusive_seq});
        if (!artifact) {
            return tl::unexpected {artifact.error()};
        }
        loaded.push_back(std::move(*artifact));
        return {};
    };
    if (chain.checkpoint) {
        if (auto one = load_one(*chain.checkpoint, metadata_artifact_kind::checkpoint); !one) {
            return tl::unexpected {one.error()};
        }
    }
    for (auto const& run : chain.runs) {
        if (auto one = load_one(run, metadata_artifact_kind::run); !one) {
            return tl::unexpected {one.error()};
        }
    }
    return loaded;
}

// Disk admission: the candidate artifacts (plus a fixed margin for the commit's
// WAL growth and the manifest mirror) must fit before any file is written.
auto require_free_space(std::uint64_t const needed_bytes, std::filesystem::path const& dir, std::uint64_t const generation) -> result<void>
{
    constexpr auto margin_bytes = std::uint64_t {1} << 20U;
    if (injected(fault::point::before_disk_admission, fault::artifact_kind::edit, generation)) {
        return tl::unexpected {error {error_code::io_failure, "not enough free disk space for the edit's artifacts"}};
    }
    std::error_code fs_err;
    auto const info = std::filesystem::space(dir, fs_err);
    if (fs_err) {
        return tl::unexpected {error {error_code::io_failure, "could not determine the free disk space"}};
    }
    if (info.available < needed_bytes + margin_bytes) {
        return tl::unexpected {error {error_code::io_failure, "not enough free disk space for the edit's artifacts"}};
    }
    return {};
}

}  // namespace

// Working state of one indexed patch, threaded through its phases.
struct database_manager::indexed_patch_context
{
    std::uint64_t base_seq {};
    std::uint64_t to_seq {};
    std::uint64_t generation {};
    std::uint64_t segment_source_seq {};
    bool tree_usable {false};
    identity_bytes bundle_id {};
    position_metadata_after_image image {};
    position_metadata_slot slot {};
    patch_report report {};
    // The installed tree, set only when the edit contributes a tree run.
    composite_opening_tree const* live_tree {nullptr};
    std::optional<tree_patch_run> tree_run;
    tree_patch_run_ptr tree_run_ptr;
    snapshot_payload payload;
    metadata_artifact_identity metadata_identity {};
    metadata_artifact pending_metadata {};
    bool fold_metadata {false};
    bool fold_tree {false};
    prepared_readers readers;
    std::vector<staged_derived_artifact> staged_files;
};

// ── Gates: nothing has been written, nothing is staged ───────────────────────
auto database_manager::prepare_indexed_patch(prepared_metadata_patch const& plan, indexed_patch_context& ctx) const -> result<void>
{
    constexpr auto int64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    if (dir_.empty() || !store_) {
        return tl::unexpected {error_code::invalid_argument};
    }
    if (store_->has_pending_canonical_change() || sqlite3_get_autocommit(conn_) == 0) {
        return tl::unexpected {
            error {error_code::invalid_argument, "a result or rating patch cannot run inside an open canonical transaction"}};
    }
    // A stale baseline (an insert/remove/import not yet rebuilt) cannot certify
    // incremental coverage; reject before any UPDATE rather than carry the gap.
    auto const status = require_postings_usable();
    if (!status) {
        return tl::unexpected {status.error()};
    }
    auto const base_seq = status->mutation_seq;
    if (manifest_.publication_generation >= int64_max || base_seq >= int64_max - 1U) {
        return io_failure_because("publication generation or mutation sequence exhausted");
    }
    auto const* segment = manifest_.postings();
    auto const bundle_id = identity_from_hex(manifest_.payload.bundle.id);
    if (segment == nullptr || !bundle_id || !position_postings_) {
        return io_failure_because("the installed snapshot is not a complete postings snapshot");
    }
    if (plan.content_revision != 1U) {
        return io_failure_because("unsupported content revision");
    }

    // The metadata after-image and its resident slot (verifies the document and
    // its revision; assigns nothing yet).
    auto const white = narrow_elo(plan.after.white_elo);
    auto const black = narrow_elo(plan.after.black_elo);
    if (!white || !black) {
        return tl::unexpected {error {error_code::invalid_argument, "rating outside the signed 16-bit domain"}};
    }
    auto const image = position_metadata_after_image {
        .id = plan.id, .content_revision = 1, .result = encode_result_code(plan.after.result), .white_elo = *white, .black_elo = *black};
    auto const slot = position_postings_->prepare_metadata_update(image);
    if (!slot) {
        return tl::unexpected {slot.error()};
    }

    ctx.base_seq = base_seq;
    ctx.to_seq = base_seq + 1U;
    ctx.generation = manifest_.publication_generation + 1U;
    ctx.segment_source_seq = segment->source_mutation_seq;
    ctx.tree_usable = status->tree_usable;
    ctx.bundle_id = *bundle_id;
    ctx.image = image;
    ctx.slot = *slot;
    if (injected(fault::point::before_build, fault::artifact_kind::edit, ctx.generation)) {
        return io_failure_because("injected failure before the edit's artifacts are built");
    }
    return {};
}

// ── One-game contribution, only when a usable tree exists ────────────────────
auto database_manager::build_patch_tree_run(prepared_metadata_patch const& plan, indexed_patch_context& ctx) const -> result<void>
{
    auto const* tree_descriptor_now = manifest_.tree();
    if (!ctx.tree_usable || !composite_tree_ || tree_descriptor_now == nullptr || !store_) {
        return {};
    }
    ctx.live_tree = &*composite_tree_;
    auto const record = store_->get_replay_record(plan.id);
    if (!record) {
        return tl::unexpected {record.error()};
    }
    if (record->result != plan.before.result || record->white_elo != plan.before.white_elo || record->black_elo != plan.before.black_elo) {
        return io_failure_because("the game changed after the patch was prepared");
    }
    auto builder = tree_patch_builder {tree_patch_identity {.bundle_id = ctx.bundle_id,
                                                            .index_semantics = index_semantics_version,
                                                            .canonical_start_hash = opening_tree_start_hash(),
                                                            .max_root_ply = tree_descriptor_now->max_root_ply}};
    auto const state_of = [&](indexed_game_metadata const& values) -> tree_game_state
    {
        return tree_game_state {.id = plan.id,
                                .moves = record->moves,
                                .result = encode_result_code(values.result),
                                .white_elo = values.white_elo,
                                .black_elo = values.black_elo};
    };
    if (auto added = builder.add_metadata_edit(state_of(plan.before), state_of(plan.after)); !added) {
        return tl::unexpected {added.error()};
    }
    auto built = builder.build(ctx.base_seq, ctx.to_seq);
    if (!built) {
        return tl::unexpected {built.error()};
    }
    ctx.tree_run = std::move(*built);
    if (encode_result_code(plan.before.result) != encode_result_code(plan.after.result) || plan.before.white_elo != plan.after.white_elo
        || plan.before.black_elo != plan.after.black_elo)
    {
        ctx.report.replayed_games = 1;
        ctx.report.replayed_plies = record->moves.size();
    }
    return {};
}

// ── Admission: append each chain, or fold it when a hard cap would be crossed ─
auto database_manager::admit_patch_runs(indexed_patch_context& ctx) const -> result<void>
{
    ctx.payload = manifest_.payload;
    ctx.metadata_identity = metadata_artifact_identity {.bundle_id = ctx.bundle_id, .index_semantics = index_semantics_version};
    ctx.pending_metadata = metadata_artifact {.identity = ctx.metadata_identity,
                                              .kind = metadata_artifact_kind::run,
                                              .from_exclusive_seq = ctx.base_seq,
                                              .to_inclusive_seq = ctx.to_seq,
                                              .records = {ctx.image},
                                              .encoded_bytes = 0};
    auto const encoded_metadata = encode_metadata_artifact(ctx.pending_metadata);
    if (!encoded_metadata) {
        return tl::unexpected {encoded_metadata.error()};
    }
    ctx.pending_metadata.encoded_bytes = encoded_metadata->size();
    auto const metadata_admission = classify_chain_growth(
        chain_limits_,
        metadata_chain_usage(manifest_.payload.metadata),
        patch_chain_usage {
            .runs = 1, .encoded_bytes = ctx.pending_metadata.encoded_bytes, .decoded_bytes = ctx.pending_metadata.decoded_bytes()});

    auto tree_admission = patch_chain_admission::admit;
    if (ctx.tree_run) {
        auto const encoded_tree = encode_tree_patch_run(*ctx.tree_run);
        if (!encoded_tree) {
            return tl::unexpected {encoded_tree.error()};
        }
        ctx.tree_run->encoded_bytes = encoded_tree->size();
        tree_admission = classify_chain_growth(
            chain_limits_,
            ctx.live_tree->usage(),
            patch_chain_usage {.runs = 1, .encoded_bytes = ctx.tree_run->encoded_bytes, .decoded_bytes = ctx.tree_run->decoded_bytes()});
        ctx.tree_run_ptr = std::make_shared<tree_patch_run const>(std::move(*ctx.tree_run));
    }
    ctx.fold_metadata = metadata_admission == patch_chain_admission::fold_required;
    ctx.fold_tree = ctx.tree_run_ptr != nullptr && tree_admission == patch_chain_admission::fold_required;

    // Free space for every candidate (a fold rewrites its whole chain).
    auto needed_bytes = ctx.pending_metadata.encoded_bytes + (ctx.tree_run_ptr ? ctx.tree_run_ptr->encoded_bytes : 0U);
    if (ctx.fold_metadata) {
        for (auto const& member : manifest_.payload.metadata.runs) {
            needed_bytes += member.artifact.size_bytes;
        }
        needed_bytes += manifest_.payload.metadata.checkpoint ? manifest_.payload.metadata.checkpoint->artifact.size_bytes : 0U;
    }
    if (ctx.fold_tree) {
        needed_bytes += manifest_.tree()->checkpoint.size_bytes + ctx.live_tree->usage().encoded_bytes;
    }
    return require_free_space(needed_bytes, dir_, ctx.generation);
}

// Candidate tree reader for an append: shares the checkpoint and every run;
// validates only the touched nodes (and the start node). Nothing is written
// when it fails.
auto database_manager::extend_patch_tree_reader(indexed_patch_context& ctx) const -> result<void>
{
    if (ctx.tree_run_ptr == nullptr || ctx.fold_tree) {
        return {};
    }
    if (injected(fault::point::before_artifact_validation, fault::artifact_kind::edit, ctx.generation)) {
        return io_failure_because("injected failure before the tree run is validated");
    }
    auto extended = ctx.live_tree->with_appended_run(ctx.tree_run_ptr, chain_limits_.hard_decoded_bytes);
    if (!extended) {
        return tl::unexpected {extended.error()};
    }
    if (injected(fault::point::after_artifact_validation, fault::artifact_kind::edit, ctx.generation)) {
        return io_failure_because("injected failure after the tree run is validated");
    }
    ctx.readers.tree = std::move(*extended);
    return {};
}

// ── Stage the candidates: exclusive random names, fsync, descriptor ──────────
auto database_manager::stage_patch_artifact(indexed_patch_context& ctx,
                                            derived_artifact_family const family,
                                            std::function<result<void>(std::filesystem::path const&)> const& write)
    -> result<artifact_descriptor>
{
    constexpr auto kind = fault::artifact_kind::edit;
    auto reserved = reserve_derived_artifact(family);
    if (!reserved) {
        return tl::unexpected {reserved.error()};
    }
    ctx.staged_files.push_back(*reserved);
    if (auto written = write(reserved->staging_path); !written) {
        return tl::unexpected {written.error()};
    }
    if (injected(fault::point::before_artifact_fsync, kind, ctx.generation)) {
        return io_failure_because("injected failure before artifact fsync");
    }
    auto sealed = seal_derived_artifact(*reserved, family);
    if (!sealed) {
        return tl::unexpected {sealed.error()};
    }
    if (injected(fault::point::after_artifact_fsync, kind, ctx.generation)) {
        return io_failure_because("injected failure after artifact fsync");
    }
    return sealed;
}

auto database_manager::stage_patch_metadata(indexed_patch_context& ctx) -> result<void>
{
    if (!ctx.fold_metadata) {
        auto descriptor = stage_patch_artifact(ctx,
                                               derived_artifact_family::postings_metadata,
                                               [&ctx](std::filesystem::path const& path) -> result<void>
                                               { return write_metadata_artifact(path, ctx.pending_metadata); });
        if (!descriptor) {
            return tl::unexpected {descriptor.error()};
        }
        ctx.payload.metadata.runs.push_back(
            patch_run_descriptor {.artifact = std::move(*descriptor), .from_exclusive_seq = ctx.base_seq, .to_inclusive_seq = ctx.to_seq});
        return {};
    }

    auto chain = load_metadata_chain_artifacts(dir_, manifest_.payload.metadata, ctx.metadata_identity);
    if (!chain) {
        return tl::unexpected {chain.error()};
    }
    auto members = std::vector<metadata_artifact const*> {};
    for (auto const& member : *chain) {
        members.push_back(&member);
    }
    members.push_back(&ctx.pending_metadata);
    auto folded = fold_metadata_chain(ctx.metadata_identity, ctx.segment_source_seq, members);
    if (!folded) {
        return tl::unexpected {folded.error()};
    }
    auto descriptor = stage_patch_artifact(ctx,
                                           derived_artifact_family::postings_metadata,
                                           [&folded](std::filesystem::path const& path) -> result<void>
                                           { return write_metadata_artifact(path, *folded); });
    if (!descriptor) {
        return tl::unexpected {descriptor.error()};
    }
    ctx.payload.metadata = postings_metadata_chain {.checkpoint = patch_run_descriptor {.artifact = std::move(*descriptor),
                                                                                        .from_exclusive_seq = folded->from_exclusive_seq,
                                                                                        .to_inclusive_seq = folded->to_inclusive_seq},
                                                    .runs = {}};
    ctx.report.metadata_folded = true;
    return {};
}

auto database_manager::stage_patch_tree(indexed_patch_context& ctx) -> result<void>
{
    if (ctx.tree_run_ptr == nullptr || !ctx.payload.tree) {
        // A tree descriptor that is not usable cannot be extended: the snapshot
        // goes on without it and queries use exact postings (the existing
        // optional-tree repair, now folded into this publication).
        ctx.payload.tree.reset();
        return {};
    }
    auto& tree_payload = *ctx.payload.tree;
    if (ctx.fold_tree) {
        return stage_folded_tree(ctx, tree_payload);
    }
    auto descriptor = stage_patch_artifact(ctx,
                                           derived_artifact_family::tree_patch,
                                           [&ctx](std::filesystem::path const& path) -> result<void>
                                           { return write_tree_patch_run(path, *ctx.tree_run_ptr); });
    if (!descriptor) {
        return tl::unexpected {descriptor.error()};
    }
    tree_payload.patch_runs.push_back(
        patch_run_descriptor {.artifact = std::move(*descriptor), .from_exclusive_seq = ctx.base_seq, .to_inclusive_seq = ctx.to_seq});
    tree_payload.covered_mutation_seq = ctx.to_seq;
    return {};
}

auto database_manager::stage_folded_tree(indexed_patch_context& ctx, tree_descriptor& tree_payload) -> result<void>
{
    auto runs = std::vector<tree_patch_run_ptr> {ctx.live_tree->runs().begin(), ctx.live_tree->runs().end()};
    runs.push_back(ctx.tree_run_ptr);
    auto descriptor = stage_patch_artifact(ctx,
                                           derived_artifact_family::tree_checkpoint,
                                           [&ctx, &runs](std::filesystem::path const& path) -> result<void>
                                           { return write_folded_tree_checkpoint(*ctx.live_tree, runs, path); });
    if (!descriptor) {
        return tl::unexpected {descriptor.error()};
    }
    tree_payload.checkpoint = std::move(*descriptor);
    tree_payload.base_mutation_seq = ctx.to_seq;
    tree_payload.covered_mutation_seq = ctx.to_seq;
    tree_payload.patch_runs.clear();
    // Validate the final-named checkpoint before committing its snapshot.
    ctx.readers.tree_loader = make_checkpoint_loader(checkpoint_loader_spec {.final_path = ctx.staged_files.back().final_path,
                                                                             .mode = ctx.live_tree->child_frequency_mode(),
                                                                             .decoded_byte_cap = chain_limits_.hard_decoded_bytes,
                                                                             .source_games = manifest_.canonical_game_count,
                                                                             .generation = ctx.generation});
    ctx.report.tree_folded = true;
    return {};
}

// The index-relevant half of an edit. Everything below runs under the generation
// lock and writes nothing canonical until the one FULL transaction:
//   prepare   gates, metadata slot, one-game contribution (only with a tree),
//             admission (append, or fold when a hard cap would be crossed)
//   install   random exclusive staging, fsync, no-replace rename, directory fsync
//             (publish_prepared)
//   commit    canonical UPDATE(s) + single sequence advance + snapshot row, one
//             COMMIT; ambiguous COMMIT reconciled against the stored row
//   install   noexcept slot/tree/payload swap, data revision, manifest mirror, GC
auto database_manager::publish_indexed_patch(prepared_metadata_patch const& plan) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    auto ctx = indexed_patch_context {};

    if (auto prepared = prepare_indexed_patch(plan, ctx); !prepared) {
        return prepared;
    }
    if (auto built = build_patch_tree_run(plan, ctx); !built) {
        return built;
    }
    if (injected(fault::point::after_build, fault::artifact_kind::edit, ctx.generation)) {
        return io_failure_because("injected failure after the edit's artifacts are built");
    }
    if (auto admitted = admit_patch_runs(ctx); !admitted) {
        return admitted;
    }
    if (auto extended = extend_patch_tree_reader(ctx); !extended) {
        return extended;
    }

    auto cleanup_staged = scope_exit {[this, &ctx]() noexcept -> void
                                      {
                                          for (auto const& file : ctx.staged_files) {
                                              discard_derived_artifact(file);
                                          }
                                      }};
    if (auto staged = stage_patch_metadata(ctx); !staged) {
        return staged;
    }
    if (auto staged = stage_patch_tree(ctx); !staged) {
        return staged;
    }

    // ── Publish: durable artifacts, ONE FULL transaction, install ────────────
    auto edit = prepared_edit {
        .steps = canonical_edit_steps {.base_mutation_seq = ctx.base_seq,
                                       .apply = [this, &plan]() -> result<void> { return store_->apply_metadata_patch(plan); },
                                       .advance = [this]() -> result<void> { return store_->advance_mutation_seq(); }},
        .slot = ctx.slot};
    // From here publish_prepared owns the staged files (it removes them on a
    // failure before COMMIT and keeps them when the outcome is unknown).
    cleanup_staged.release();
    auto published = publish_prepared(std::move(ctx.payload), std::move(ctx.staged_files), std::move(ctx.readers), &edit);
    if (!published) {
        return tl::unexpected {published.error()};
    }
    ctx.report.published = true;
    ctx.report.tree_runs = composite_tree_ ? composite_tree_->runs().size() : 0U;
    ctx.report.metadata_runs = manifest_.payload.metadata.runs.size();
    last_patch_report_ = ctx.report;
    return {};
}

auto database_manager::patch_game_metadata(game_id const game_key, game_patch const& patch) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    last_patch_report_ = {};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }

    // Validation, the editability policy and the duplicate check live in the
    // store's prepare step, shared with the standalone store patch. Nothing is
    // written by it.
    auto plan = store_->prepare_metadata_patch(game_key, patch);
    if (!plan) {
        return tl::unexpected {plan.error()};
    }
    if (plan->is_noop()) {
        last_patch_report_.canonical_noop = true;
        return {};
    }
    if (!plan->indexed_change) {
        // Fields the indexes do not embed: today's NORMAL canonical-only
        // transaction, no artifacts, no FULL commit, no sequence advance.
        if (auto committed = store_->commit_metadata_patch(*plan); !committed) {
            return committed;
        }
        last_patch_report_.canonical_only = true;
        return {};
    }
    return publish_indexed_patch(*plan);
}

auto database_manager::remove_game(game_id const game_key) -> result<void>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
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

    auto const postings = usable_postings();
    if (!postings) {
        return tl::unexpected {postings.error()};
    }
    auto const& reader = **postings;

    if (!has_metadata) {
        return postings_elo_distribution(*store_, reader, hash, {}, bucket_width);
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

    return postings_elo_distribution(*store_, reader, hash, std::move(*filtered_ids_res), bucket_width);
}

auto database_manager::query_unfiltered_opening_stats(zobrist_hash const hash) const -> result<std::vector<opening_stat_agg_row>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const coverage = require_postings_usable();
    if (!coverage) {
        return tl::unexpected {coverage.error()};
    }
    if (!position_postings_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const& postings = *position_postings_;
    // The tree is used only when its own covered sequence is current too;
    // otherwise exact postings answer (slower, never stale).
    auto const exact_postings = [&]() -> result<std::vector<opening_stat_agg_row>>
    {
        auto game_ids = postings.distinct_game_ids(hash);
        if (!game_ids) {
            return tl::unexpected {game_ids.error()};
        }
        return postings_filtered_opening_stats(*store_, postings, hash, std::move(*game_ids), /*global_child_frequency=*/true);
    };
    if (!coverage->tree_usable || !composite_tree_) {
        return exact_postings();
    }
    auto const& tree = *composite_tree_;
    // The merged tree answers; should it be unable to (an edge whose extrema a
    // retraction made inexact needs a live witness, or the postings-mode child
    // count cannot be resolved), exact postings still can -- never a wrong or
    // missing answer.
    auto const from_tree = [&]() -> result<std::vector<opening_stat_agg_row>>
    {
        auto resolver = tree_child_frequency_resolver {};
        if (tree.child_frequency_mode() == tree_child_frequency_mode::postings) {
            resolver = [&postings](zobrist_hash const child) -> result<std::uint32_t>
            {
                auto const summary = postings.summary(child);
                if (!summary || !summary->has_value()) {
                    return tl::unexpected {error {error_code::io_failure, "child position missing from postings"}};
                }
                return summary->value().distinct_game_count;
            };
        }
        auto rows = tree.query_opening_stats(hash, resolver);
        if (rows) {
            return rows;
        }
        return exact_postings();
    };
    // A complete node (currently only the canonical starting position, see
    // opening_tree_index.cpp's format_version v5 comment) was aggregated from
    // every occurrence of hash in every game, not only occurrences within
    // max_root_ply, so it needs no summary max_ply cross-check.
    if (tree.is_complete(hash)) {
        return from_tree();
    }
    auto const summary = postings.summary(hash);
    if (!summary || !summary->has_value()) {
        return std::vector<opening_stat_agg_row> {};
    }
    if (summary->value().max_ply > tree.max_root_ply()) {
        return exact_postings();
    }
    return from_tree();
}

auto database_manager::query_filtered_opening_stats(zobrist_hash const hash, std::vector<game_id> const& game_ids) const
    -> result<std::vector<opening_stat_agg_row>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const postings = usable_postings();
    if (!postings) {
        return tl::unexpected {postings.error()};
    }
    return postings_filtered_opening_stats(*store_, **postings, hash, game_ids);
}

auto database_manager::query_tree_slice(zobrist_hash const root_hash,
                                        std::uint16_t const max_depth,
                                        std::vector<game_id> const& game_ids) const -> result<std::vector<tree_position_row>>
{
    auto const lock = std::scoped_lock {generation_mutex_};
    if (!store_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const postings = usable_postings();
    if (!postings) {
        return tl::unexpected {postings.error()};
    }
    auto const& reader = **postings;

    auto allowed_game_ids = game_ids;
    std::ranges::sort(allowed_game_ids);
    auto occurrences = reader.occurrences(root_hash);
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

    auto const postings = usable_postings();
    if (!postings) {
        return tl::unexpected {postings.error()};
    }
    auto const& reader = **postings;

    auto const has_metadata_filter = filter.player_name || filter.result || filter.eco_prefix || filter.min_elo || filter.max_elo;
    auto const fast_path_eligible = !has_metadata_filter && filter.sort_column == game_sort_column::id && filter.sort_ascending;
    if (fast_path_eligible) {
        auto const summary = reader.summary(*filter.position);
        if (!summary) {
            return tl::unexpected {summary.error()};
        }
        if (!*summary) {
            return game_list_result {};
        }
        auto page_game_ids = reader.distinct_game_ids(*filter.position, filter.limit, filter.offset);
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
