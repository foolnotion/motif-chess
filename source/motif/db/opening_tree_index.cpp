#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "motif/db/opening_tree_index.hpp"

#include <gtl/phmap.hpp>
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

namespace
{

constexpr auto magic = std::array<char, 8> {'M', 'O', 'T', 'I', 'F', 'O', 'T', '1'};
// v1 -> v2: continuation-record counter/sum fields switched from fixed-width
// integers to LEB128 varints (see write_varint/read_varint) -- most edges
// have small counts, so this is a meaningful size win on a real corpus, not
// just a format bump for its own sake.
constexpr auto format_version = std::uint32_t {2};
constexpr auto byte_bits = std::size_t {8};
constexpr auto byte_mask = std::uint64_t {0xff};

template<typename Integer>
auto write_little_endian(std::ofstream& output, Integer value) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    auto raw_value = std::uint64_t {value};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.put(static_cast<char>(raw_value & byte_mask));
        if constexpr (sizeof(Integer) > 1U) {
            raw_value >>= byte_bits;
        }
    }
    return static_cast<bool>(output);
}

template<typename Integer>
auto read_little_endian(std::ifstream& input, Integer& value) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    auto raw_value = std::uint64_t {0};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        auto const byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
            return false;
        }
        raw_value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte)) << (index * byte_bits);
    }
    value = static_cast<Integer>(raw_value);
    return true;
}

auto write_f64(std::ofstream& output, double value) -> bool
{
    return write_little_endian(output, std::bit_cast<std::uint64_t>(value));
}

auto read_f64(std::ifstream& input, double& value) -> bool
{
    std::uint64_t raw {};
    if (!read_little_endian(input, raw)) {
        return false;
    }
    value = std::bit_cast<double>(raw);
    return true;
}

// LEB128-style unsigned varint: 7 payload bits/byte, high bit = "more
// bytes follow". Most continuation-record fields (frequency, win/draw/loss
// counts, elo sums) follow a power-law distribution across a real corpus --
// a handful of popular opening moves have huge counts, the overwhelming
// majority of edges have small ones -- so this costs 1 byte for the common
// case and only grows for the rare high-frequency edges, instead of paying
// a fixed 4 or 8 bytes for every edge regardless of its actual value.
auto write_varint(std::ofstream& output, std::uint64_t value) -> bool
{
    constexpr auto continuation_bit = std::uint8_t {0x80};
    constexpr auto payload_mask = std::uint8_t {0x7F};
    constexpr auto payload_bits = 7U;

    auto more = true;
    while (more) {
        auto byte = static_cast<std::uint8_t>(value & payload_mask);
        value >>= payload_bits;
        more = value != 0U;
        if (more) {
            byte |= continuation_bit;
        }
        output.put(static_cast<char>(byte));
    }
    return static_cast<bool>(output);
}

auto read_varint(std::ifstream& input, std::uint64_t& value) -> bool
{
    constexpr auto continuation_bit = std::uint8_t {0x80};
    constexpr auto payload_mask = std::uint8_t {0x7F};
    constexpr auto payload_bits = 7U;
    constexpr auto max_shift = 63U;
    // At shift 63, only bit 0 of a 7-bit payload fits inside a 64-bit
    // value; the rest would silently shift out. Reject a 10th byte that
    // sets any of those bits instead of dropping them -- otherwise a
    // corrupted/overflowing varint decodes to a wrapped value instead of
    // failing, and the caller has no way to tell the two apart.
    constexpr auto max_shift_payload_limit = std::uint8_t {0x01};

    value = 0;
    auto shift = 0U;
    while (true) {
        auto const byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
            return false;
        }
        if (shift > max_shift) {
            return false;
        }
        auto const payload = static_cast<std::uint8_t>(byte);
        auto const payload_bits_value = static_cast<std::uint8_t>(payload & payload_mask);
        if (shift == max_shift && payload_bits_value > max_shift_payload_limit) {
            return false;
        }
        value |= static_cast<std::uint64_t>(payload_bits_value) << shift;
        if ((payload & continuation_bit) == 0U) {
            return true;
        }
        shift += payload_bits;
    }
}

// (root_hash, encoded_move, child_hash) -- one entry per distinct edge.
struct edge_key
{
    std::uint64_t root_hash {};
    std::uint16_t encoded_move {};
    std::uint64_t child_hash {};

    auto operator==(edge_key const&) const -> bool = default;
};

struct edge_key_hash
{
    auto operator()(edge_key const& key) const noexcept -> std::size_t
    {
        constexpr std::size_t phi = 0x9e3779b97f4a7c15ULL;
        constexpr std::size_t lshift = 12U;
        constexpr std::size_t rshift = 4U;
        auto seed = std::hash<std::uint64_t> {}(key.root_hash);
        seed ^= std::hash<std::uint64_t> {}(key.child_hash) + phi + (seed << lshift) + (seed >> rshift);
        seed ^= static_cast<std::size_t>(key.encoded_move) + phi + (seed << lshift) + (seed >> rshift);
        return seed;
    }
};

// In-memory accumulator for one edge, before the child's global
// transposition_frequency is known (attached in a second pass once every
// game has been replayed).
struct edge_agg
{
    std::uint16_t root_ply {std::numeric_limits<std::uint16_t>::max()};
    std::uint32_t frequency {};
    std::uint32_t white_wins {};
    std::uint32_t draws {};
    std::uint32_t black_wins {};
    std::int64_t white_elo_sum {};
    std::uint32_t white_elo_count {};
    std::int64_t black_elo_sum {};
    std::uint32_t black_elo_count {};
    double weighted_contrib_sum {0.0};
    double elo_weight_sum {0.0};
    std::uint32_t eco_sample_min {std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t eco_sample_max {0};
};

// Mirrors database_manager.cpp's local map_result: draws and unrecognized
// result strings both map to 0. Duplicated rather than shared because that
// function is anonymous-namespace, file-local to database_manager.cpp.
auto map_result(std::string const& pgn_result) -> std::int8_t
{
    if (pgn_result == "1-0") {
        return 1;
    }
    if (pgn_result == "0-1") {
        return -1;
    }
    return 0;
}

// Mirrors note_result/note_elo in source/motif/search/opening_tree.cpp,
// duplicated for the same reason (file-local there).
void note_result(edge_agg& agg, std::int8_t const result)
{
    if (result > 0) {
        ++agg.white_wins;
        return;
    }
    if (result < 0) {
        ++agg.black_wins;
        return;
    }
    ++agg.draws;
}

void note_elo(edge_agg& agg,
              std::int8_t const result,
              std::optional<std::int32_t> const& white_elo,
              std::optional<std::int32_t> const& black_elo)
{
    if (white_elo.has_value()) {
        agg.white_elo_sum += *white_elo;
        ++agg.white_elo_count;
    }
    if (black_elo.has_value()) {
        agg.black_elo_sum += *black_elo;
        ++agg.black_elo_count;
    }
    if (white_elo.has_value() && black_elo.has_value()) {
        auto const avg_elo = (static_cast<double>(*white_elo) + static_cast<double>(*black_elo)) / 2.0;
        agg.weighted_contrib_sum += static_cast<double>(result) * avg_elo;
        agg.elo_weight_sum += avg_elo;
    }
}

auto average_elo(std::int64_t const sum, std::uint32_t const count) -> std::optional<double>
{
    if (count == 0U) {
        return std::nullopt;
    }
    return static_cast<double>(sum) / static_cast<double>(count);
}

// Bounded-memory external-merge counter: counts how many distinct times each
// hash is `visit()`-ed (callers are responsible for calling it at most once
// per game per hash, so the count is "distinct games that visited this
// hash"). This is the piece that must not scale with corpus size in memory:
// transposition_frequency's scope is every ply of every game, unlike the
// ply-capped edge map below, so a flat in-memory map here would grow with
// total corpus positions rather than with the (much smaller) shallow index
// being built. Generalizes the spill/k-way-merge shape of
// position_prefix_postings.cpp's finalize_external_merge to a counting
// merge instead of a dedup merge.
class hash_visit_counter
{
  public:
    hash_visit_counter(std::filesystem::path scratch_prefix, std::size_t const spill_threshold)
        : scratch_prefix_ {std::move(scratch_prefix)}
        , spill_threshold_ {spill_threshold}
    {
    }

    // Best-effort cleanup of any spill runs left behind by an error return
    // from spill_current_buffer()/finalize() -- the success path in
    // finalize() already removes and clears run_paths_, so this is a no-op
    // there; it only matters on the error paths, which otherwise leaked
    // these files indefinitely.
    ~hash_visit_counter()
    {
        for (auto const& run_path : run_paths_) {
            std::error_code remove_err;
            std::filesystem::remove(run_path, remove_err);
        }
    }

    hash_visit_counter(hash_visit_counter const&) = delete;
    auto operator=(hash_visit_counter const&) -> hash_visit_counter& = delete;
    hash_visit_counter(hash_visit_counter&&) = delete;
    auto operator=(hash_visit_counter&&) -> hash_visit_counter& = delete;

    auto visit(std::uint64_t const hash) -> result<void>
    {
        buffer_.push_back(hash);
        if (buffer_.size() < spill_threshold_) {
            return {};
        }
        return spill_current_buffer();
    }

    // Sorted ascending by hash. Omits hashes with a total count below
    // keep_min_count -- callers must treat "not present" as count 1 (the
    // implicit case for a hash visited by exactly one game, which is the
    // overwhelming majority of positions past shallow depth). Also omits
    // any hash not in `needed_hashes`, even if it clears keep_min_count --
    // this is what actually bounds peak memory by the shallow index being
    // built rather than by every repeated position anywhere in the corpus
    // (a hot ply-60+ endgame reached by many games would otherwise sit in
    // the output despite never being looked up, since write_node() only
    // ever queries child hashes of a ply-capped edge).
    [[nodiscard]] auto finalize(std::uint32_t keep_min_count, gtl::flat_hash_set<std::uint64_t> const& needed_hashes)
        -> result<std::vector<std::pair<std::uint64_t, std::uint32_t>>>;

  private:
    [[nodiscard]] auto spill_current_buffer() -> result<void>;

    std::filesystem::path scratch_prefix_;
    std::size_t spill_threshold_;
    std::vector<std::uint64_t> buffer_;
    std::vector<std::filesystem::path> run_paths_;
};

// Sorts and run-length-encodes the buffer into (hash, count) pairs before
// writing -- a hot position (e.g. the start position) visited by millions of
// games would otherwise spill millions of duplicate raw hash values instead
// of one pair.
auto hash_visit_counter::spill_current_buffer() -> result<void>
{
    if (buffer_.empty()) {
        return {};
    }

    std::ranges::sort(buffer_);
    std::vector<std::pair<std::uint64_t, std::uint32_t>> encoded;
    encoded.reserve(buffer_.size());
    for (std::size_t index = 0; index < buffer_.size();) {
        auto const hash = buffer_[index];
        std::uint32_t count = 0;
        while (index < buffer_.size() && buffer_[index] == hash) {
            ++count;
            ++index;
        }
        encoded.emplace_back(hash, count);
    }
    buffer_.clear();

    auto const run_path = std::filesystem::path {scratch_prefix_.string() + ".spill" + std::to_string(run_paths_.size())};
    std::ofstream output {run_path, std::ios::binary | std::ios::trunc};
    if (!output || !write_little_endian(output, static_cast<std::uint64_t>(encoded.size()))) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }
    for (auto const& [hash, count] : encoded) {
        if (!write_little_endian(output, hash) || !write_little_endian(output, count)) {
            return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
        }
    }
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "spill write failed"}};
    }

    run_paths_.push_back(run_path);
    return {};
}

struct hash_count_run_cursor
{
    std::ifstream input;
    std::uint64_t remaining {};
    std::uint64_t current_hash {};
    std::uint32_t current_count {};
};

auto advance_run_cursor(hash_count_run_cursor& cursor) -> result<bool>
{
    if (cursor.remaining == 0U) {
        return false;
    }
    if (!read_little_endian(cursor.input, cursor.current_hash) || !read_little_endian(cursor.input, cursor.current_count)) {
        return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
    }
    --cursor.remaining;
    return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- k-way merge w/ per-step checks, mirrors position_prefix_postings.cpp
auto hash_visit_counter::finalize(std::uint32_t const keep_min_count, gtl::flat_hash_set<std::uint64_t> const& needed_hashes)
    -> result<std::vector<std::pair<std::uint64_t, std::uint32_t>>>
{
    if (auto spill_res = spill_current_buffer(); !spill_res) {
        return tl::unexpected {spill_res.error()};
    }

    std::vector<hash_count_run_cursor> cursors;
    cursors.reserve(run_paths_.size());
    for (auto const& run_path : run_paths_) {
        hash_count_run_cursor cursor {.input = std::ifstream {run_path, std::ios::binary}};
        if (!cursor.input || !read_little_endian(cursor.input, cursor.remaining)) {
            return tl::unexpected {error {error_code::io_failure, "spill read failed"}};
        }
        cursors.push_back(std::move(cursor));
    }

    auto compare = [&cursors](std::size_t const lhs, std::size_t const rhs) -> bool
    { return cursors[rhs].current_hash < cursors[lhs].current_hash; };
    std::vector<std::size_t> heap;
    heap.reserve(cursors.size());
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        auto const has_record = advance_run_cursor(cursors[index]);
        if (!has_record) {
            return tl::unexpected {has_record.error()};
        }
        if (*has_record) {
            heap.push_back(index);
        }
    }
    std::ranges::make_heap(heap, compare);

    std::vector<std::pair<std::uint64_t, std::uint32_t>> merged;
    auto current_hash = std::uint64_t {0};
    auto current_sum = std::uint32_t {0};
    auto has_current = false;

    auto flush_current = [&]() -> void
    {
        if (has_current && current_sum >= keep_min_count && needed_hashes.contains(current_hash)) {
            merged.emplace_back(current_hash, current_sum);
        }
    };

    while (!heap.empty()) {
        std::ranges::pop_heap(heap, compare);
        auto const index = heap.back();
        heap.pop_back();

        auto const hash = cursors[index].current_hash;
        if (!has_current || hash != current_hash) {
            flush_current();
            current_hash = hash;
            current_sum = 0;
            has_current = true;
        }
        current_sum += cursors[index].current_count;

        auto const has_next = advance_run_cursor(cursors[index]);
        if (!has_next) {
            return tl::unexpected {has_next.error()};
        }
        if (*has_next) {
            heap.push_back(index);
            std::ranges::push_heap(heap, compare);
        }
    }
    flush_current();

    for (auto const& run_path : run_paths_) {
        std::error_code remove_err;
        std::filesystem::remove(run_path, remove_err);
    }
    run_paths_.clear();

    return merged;
}

auto transposition_frequency_for(std::uint64_t const child_hash, std::vector<std::pair<std::uint64_t, std::uint32_t>> const& counts)
    -> std::uint32_t
{
    auto const lower = std::ranges::lower_bound(counts, child_hash, {}, &std::pair<std::uint64_t, std::uint32_t>::first);
    if (lower != counts.end() && lower->first == child_hash) {
        return lower->second;
    }
    // Not present means no game other than the one edge visited it, i.e. a
    // single distinct visiting game -- see hash_visit_counter::finalize's
    // keep_min_count contract.
    return 1;
}

// Everything build() accumulates across the whole corpus, threaded through
// the per-game and per-write helper functions below. child_freq lives
// outside this struct (in a hash_visit_counter) because it must stay
// bounded-memory; edges/edges_by_root are bounded by the ply cap instead and
// are the actual output being built, so keeping them in memory is
// unavoidable regardless of build strategy.
struct build_state
{
    gtl::flat_hash_map<edge_key, edge_agg, edge_key_hash> edges;
    // root_hash -> the edges discovered under it, built up as edges are
    // found so the final grouping pass doesn't need to re-derive it.
    gtl::flat_hash_map<std::uint64_t, std::vector<edge_key>> edges_by_root;
};

// Replays one game, updating `state` and `visits` in place. Two
// aggregations happen over the same replay at two different depth scopes:
// `visits` (transposition_frequency) is uncapped, `state.edges` is capped at
// max_root_ply. See position_store.cpp's create_opening_stats_rollups_template
// comment for why that asymmetry exists in the DuckDB rollup this mirrors.
auto accumulate_game(motif::db::game const& current_game,
                     game_id const game_key,
                     std::uint16_t const max_root_ply,
                     hash_visit_counter& visits,
                     build_state& state) -> result<void>
{
    auto const result_code = map_result(current_game.result);
    auto const white_elo = current_game.white.elo;
    auto const black_elo = current_game.black.elo;

    auto board = motif::chess::board {};
    auto prev_hash = board.hash();

    gtl::flat_hash_set<std::uint64_t> visited_this_game;
    // Per-game dedup + min-root-ply-within-this-game tracking, matching
    // edge_deduped's "GROUP BY p_root.game_id, ..." + MIN(p_root.ply).
    gtl::flat_hash_map<edge_key, std::uint16_t, edge_key_hash> edges_this_game;

    if (visited_this_game.insert(prev_hash).second) {
        if (auto visit_res = visits.visit(prev_hash); !visit_res) {
            return visit_res;
        }
    }

    for (std::size_t root_ply = 0; root_ply < current_game.moves.size(); ++root_ply) {
        if (root_ply > std::numeric_limits<std::uint16_t>::max()) {
            return tl::unexpected {error {error_code::io_failure, "game exceeds max representable ply"}};
        }

        auto const encoded_move = current_game.moves[root_ply];
        motif::chess::apply_encoded_move(board, encoded_move);
        auto const child_hash = board.hash();

        if (visited_this_game.insert(child_hash).second) {
            if (auto visit_res = visits.visit(child_hash); !visit_res) {
                return visit_res;
            }
        }

        if (root_ply <= max_root_ply) {
            auto const key = edge_key {.root_hash = prev_hash, .encoded_move = encoded_move, .child_hash = child_hash};
            auto const ply16 = static_cast<std::uint16_t>(root_ply);
            auto [entry, inserted] = edges_this_game.try_emplace(key, ply16);
            if (!inserted && ply16 < entry->second) {
                entry->second = ply16;
            }
        }

        prev_hash = child_hash;
    }

    for (auto const& [key, min_ply_this_game] : edges_this_game) {
        auto [entry, inserted] = state.edges.try_emplace(key);
        if (inserted) {
            state.edges_by_root[key.root_hash].push_back(key);
        }
        auto& agg = entry->second;
        ++agg.frequency;
        note_result(agg, result_code);
        note_elo(agg, result_code, white_elo, black_elo);
        agg.root_ply = std::min(agg.root_ply, min_ply_this_game);
        agg.eco_sample_min = std::min(agg.eco_sample_min, game_key.value);
        agg.eco_sample_max = std::max(agg.eco_sample_max, game_key.value);
    }

    return {};
}

auto write_continuation(std::ofstream& output, edge_key const& key, edge_agg const& agg, std::uint32_t const transposition_frequency)
    -> bool
{
    // white_elo_sum/black_elo_sum only ever accumulate non-negative int32
    // elo values (see note_elo), so they fit an unsigned varint without a
    // zigzag scheme. weighted_contrib_sum/elo_weight_sum are genuinely
    // fractional/signed doubles with no natural varint mapping -- left fixed.
    return write_little_endian(output, key.encoded_move) && write_little_endian(output, key.child_hash)
        && write_varint(output, agg.root_ply) && write_varint(output, agg.frequency) && write_varint(output, agg.white_wins)
        && write_varint(output, agg.draws) && write_varint(output, agg.black_wins)
        && write_varint(output, static_cast<std::uint64_t>(agg.white_elo_sum)) && write_varint(output, agg.white_elo_count)
        && write_varint(output, static_cast<std::uint64_t>(agg.black_elo_sum)) && write_varint(output, agg.black_elo_count)
        && write_f64(output, agg.weighted_contrib_sum) && write_f64(output, agg.elo_weight_sum) && write_varint(output, agg.eco_sample_min)
        && write_varint(output, agg.eco_sample_max) && write_varint(output, transposition_frequency);
}

auto write_node(std::ofstream& output,
                std::uint64_t const root_hash,
                build_state const& state,
                std::vector<std::pair<std::uint64_t, std::uint32_t>> const& child_counts) -> bool
{
    auto keys = state.edges_by_root.at(root_hash);
    std::ranges::sort(keys, {}, &edge_key::encoded_move);

    if (!write_little_endian(output, root_hash) || !write_varint(output, keys.size())) {
        return false;
    }

    for (auto const& key : keys) {
        auto const transposition_frequency = transposition_frequency_for(key.child_hash, child_counts);
        if (!write_continuation(output, key, state.edges.at(key), transposition_frequency)) {
            return false;
        }
    }
    return true;
}

auto write_index_file(std::filesystem::path const& path,
                      opening_tree_index_build_options const& opts,
                      build_state const& state,
                      std::vector<std::pair<std::uint64_t, std::uint32_t>> const& child_counts) -> result<void>
{
    std::vector<std::uint64_t> roots;
    roots.reserve(state.edges_by_root.size());
    for (auto const& [root_hash, keys] : state.edges_by_root) {
        roots.push_back(root_hash);
    }
    std::ranges::sort(roots);

    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "cannot open output file"}};
    }

    output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!write_little_endian(output, format_version) || !write_little_endian(output, opts.max_root_ply)
        || !write_little_endian(output, static_cast<std::uint64_t>(roots.size())))
    {
        return tl::unexpected {error {error_code::io_failure, "write failed"}};
    }

    for (auto const root_hash : roots) {
        if (!write_node(output, root_hash, state, child_counts)) {
            return tl::unexpected {error {error_code::io_failure, "write failed"}};
        }
    }

    if (!output) {
        return tl::unexpected {error {error_code::io_failure, "write failed"}};
    }
    return {};
}

}  // namespace

auto opening_tree_index::build(game_store& store, std::filesystem::path const& path, build_options const& opts) -> result<void>
{
    auto ids_res = store.all_game_ids();
    if (!ids_res) {
        return tl::unexpected {ids_res.error()};
    }

    constexpr auto default_spill_threshold = std::size_t {1} << 20U;
    // Only hashes visited by 2+ distinct games need an explicit entry; a
    // single-visit hash's transposition_frequency is implicitly 1 (see
    // transposition_frequency_for). This is the actual memory-bounding
    // lever: deep positions are overwhelmingly singletons.
    constexpr auto keep_min_count = std::uint32_t {2};

    hash_visit_counter visits {std::filesystem::path {path.string() + ".childfreq"}, default_spill_threshold};
    build_state state;
    for (auto const game_key : *ids_res) {
        auto game_res = store.get(game_key);
        if (!game_res) {
            return tl::unexpected {game_res.error()};
        }
        if (auto accumulate_res = accumulate_game(*game_res, game_key, opts.max_root_ply, visits, state); !accumulate_res) {
            return accumulate_res;
        }
    }

    gtl::flat_hash_set<std::uint64_t> needed_child_hashes;
    needed_child_hashes.reserve(state.edges.size());
    for (auto const& entry : state.edges) {
        needed_child_hashes.insert(entry.first.child_hash);
    }

    auto child_counts = visits.finalize(keep_min_count, needed_child_hashes);
    if (!child_counts) {
        return tl::unexpected {child_counts.error()};
    }

    return write_index_file(path, opts, state, *child_counts);
}

auto opening_tree_index::open(std::filesystem::path const& path) -> result<opening_tree_index>
{
    std::ifstream input {path, std::ios::binary};
    if (!input) {
        return tl::unexpected {error {error_code::io_failure, "cannot open input file"}};
    }

    std::array<char, magic.size()> read_magic {};
    input.read(read_magic.data(), static_cast<std::streamsize>(read_magic.size()));
    if (!input || read_magic != magic) {
        return tl::unexpected {error {error_code::io_failure, "bad magic"}};
    }

    std::uint32_t version {};
    std::uint16_t max_root_ply {};
    std::uint64_t node_count {};
    if (!read_little_endian(input, version) || version != format_version || !read_little_endian(input, max_root_ply)
        || !read_little_endian(input, node_count))
    {
        return tl::unexpected {error {error_code::io_failure, "bad header"}};
    }

    opening_tree_index index;
    // Deliberately no reserve() here: node_count is an untrusted 64-bit
    // value read straight from the file header. Reserving against it would
    // let a corrupt/truncated file throw std::length_error/bad_alloc out of
    // this tl::expected-returning API before the truncation check in the
    // loop below ever runs. Growing via push_back costs some reallocation
    // on a genuinely large, valid file; that's the right trade against an
    // exception escaping the error contract every other check in this
    // function honors.
    index.node_offsets_.push_back(0);

    for (std::uint64_t node_index = 0; node_index < node_count; ++node_index) {
        std::uint64_t node_hash {};
        std::uint64_t continuation_count {};
        if (!read_little_endian(input, node_hash) || !read_varint(input, continuation_count)) {
            return tl::unexpected {error {error_code::io_failure, "truncated node header"}};
        }
        index.node_hashes_.push_back(node_hash);

        for (std::uint64_t cont_index = 0; cont_index < continuation_count; ++cont_index) {
            std::uint16_t encoded_move {};
            std::uint64_t child_hash {};
            double weighted_contrib_sum {};
            double elo_weight_sum {};
            std::uint64_t root_ply_raw {};
            std::uint64_t frequency_raw {};
            std::uint64_t white_wins_raw {};
            std::uint64_t draws_raw {};
            std::uint64_t black_wins_raw {};
            std::uint64_t white_elo_sum_raw {};
            std::uint64_t white_elo_count_raw {};
            std::uint64_t black_elo_sum_raw {};
            std::uint64_t black_elo_count_raw {};
            std::uint64_t eco_sample_min_raw {};
            std::uint64_t eco_sample_max_raw {};
            std::uint64_t transposition_frequency_raw {};

            bool const read_ok = read_little_endian(input, encoded_move) && read_little_endian(input, child_hash)
                && read_varint(input, root_ply_raw) && read_varint(input, frequency_raw) && read_varint(input, white_wins_raw)
                && read_varint(input, draws_raw) && read_varint(input, black_wins_raw) && read_varint(input, white_elo_sum_raw)
                && read_varint(input, white_elo_count_raw) && read_varint(input, black_elo_sum_raw)
                && read_varint(input, black_elo_count_raw) && read_f64(input, weighted_contrib_sum) && read_f64(input, elo_weight_sum)
                && read_varint(input, eco_sample_min_raw) && read_varint(input, eco_sample_max_raw)
                && read_varint(input, transposition_frequency_raw);
            if (!read_ok) {
                return tl::unexpected {error {error_code::io_failure, "truncated continuation record"}};
            }

            auto const root_ply = static_cast<std::uint16_t>(root_ply_raw);
            auto const frequency = static_cast<std::uint32_t>(frequency_raw);
            auto const white_wins = static_cast<std::uint32_t>(white_wins_raw);
            auto const draws = static_cast<std::uint32_t>(draws_raw);
            auto const black_wins = static_cast<std::uint32_t>(black_wins_raw);
            auto const white_elo_sum = static_cast<std::int64_t>(white_elo_sum_raw);
            auto const white_elo_count = static_cast<std::uint32_t>(white_elo_count_raw);
            auto const black_elo_sum = static_cast<std::int64_t>(black_elo_sum_raw);
            auto const black_elo_count = static_cast<std::uint32_t>(black_elo_count_raw);
            auto const eco_sample_min = static_cast<std::uint32_t>(eco_sample_min_raw);
            auto const eco_sample_max = static_cast<std::uint32_t>(eco_sample_max_raw);
            auto const transposition_frequency = static_cast<std::uint32_t>(transposition_frequency_raw);

            index.continuations_.push_back(opening_stat_agg_row {
                .cont_encoded_move = encoded_move,
                .cont_hash = zobrist_hash {child_hash},
                .root_ply = root_ply,
                .frequency = frequency,
                .transposition_frequency = transposition_frequency,
                .white_wins = white_wins,
                .draws = draws,
                .black_wins = black_wins,
                .avg_white_elo = average_elo(white_elo_sum, white_elo_count),
                .avg_black_elo = average_elo(black_elo_sum, black_elo_count),
                .eco_sample_min = game_id {eco_sample_min},
                .eco_sample_max = game_id {eco_sample_max},
                .elo_weighted_score = elo_weight_sum > 0.0 ? std::optional<double> {weighted_contrib_sum / elo_weight_sum} : std::nullopt,
            });
        }

        index.node_offsets_.push_back(static_cast<std::uint32_t>(index.continuations_.size()));
    }

    return index;
}

auto opening_tree_index::query_opening_stats(zobrist_hash const hash) const -> result<std::vector<opening_stat_agg_row>>
{
    auto const lower = std::ranges::lower_bound(node_hashes_, hash.value);
    auto const node_index = static_cast<std::size_t>(lower - node_hashes_.begin());
    if (node_index >= node_hashes_.size() || node_hashes_[node_index] != hash.value) {
        return std::vector<opening_stat_agg_row> {};
    }

    auto const begin = continuations_.begin() + static_cast<std::ptrdiff_t>(node_offsets_[node_index]);
    auto const end = continuations_.begin() + static_cast<std::ptrdiff_t>(node_offsets_[node_index + 1]);
    return std::vector<opening_stat_agg_row> {begin, end};
}

}  // namespace motif::db
