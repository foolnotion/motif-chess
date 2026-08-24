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
// v1 -> v2: continuation-record fields switched from fixed-width to
// LEB128 varints (write_varint/read_varint) -- most edges have small counts.
constexpr auto format_version = std::uint32_t {3};
constexpr auto byte_bits = std::size_t {8};
constexpr auto byte_mask = std::uint64_t {0xff};
constexpr auto default_spill_threshold = std::size_t {1} << 20U;

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
// bytes follow". Cheap for the common case since opening frequencies are
// power-law distributed (a few huge counts, most edges small).
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
    // At shift 63 only bit 0 of a payload fits in a u64; reject a 10th byte
    // that sets any other bit instead of silently shifting it out.
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

// Bounded-memory external-merge counter: counts distinct-game visits per
// hash (callers call visit() at most once per game per hash). Unlike the
// ply-capped edge map below, transposition_frequency spans every ply of
// every game, so this can't be a flat in-memory map. Generalizes
// position_prefix_postings.cpp's spill/k-way-merge to counting, not dedup.
class hash_visit_counter
{
  public:
    hash_visit_counter(std::filesystem::path scratch_prefix, std::size_t const spill_threshold)
        : scratch_prefix_ {std::move(scratch_prefix)}
        , spill_threshold_ {spill_threshold}
    {
    }

    // Best-effort cleanup of spill runs left behind by an error return from
    // spill_current_buffer()/finalize() -- a no-op on the success path,
    // which already removes and clears run_paths_.
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

    // Sorted ascending by hash. Omits hashes below keep_min_count (callers
    // treat "not present" as count 1) and any hash not in `needed_hashes` --
    // the latter is what actually bounds peak memory by the index being
    // built rather than by every repeated position in the corpus.
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

// Everything build() accumulates across the whole corpus. The uncapped
// transposition counter lives outside this struct, in a hash_visit_counter,
// because it must stay bounded-memory; edges/edges_by_root are bounded by
// the ply cap and are the actual output being built, so keeping them in
// memory is unavoidable regardless of build strategy.
struct build_state
{
    gtl::flat_hash_map<edge_key, edge_agg, edge_key_hash> edges;
    // root_hash -> the edges discovered under it, built up as edges are
    // found so the final grouping pass doesn't need to re-derive it.
    gtl::flat_hash_map<std::uint64_t, std::vector<edge_key>> edges_by_root;
    // Distinct games that reach every indexed root, including terminal roots
    // with no continuation record.
    gtl::flat_hash_map<std::uint64_t, std::uint32_t> root_game_counts;
};

// Replays one game into `state` and `visits`: `visits`
// (transposition_frequency) is uncapped, `state.edges` is capped at
// max_root_ply -- see position_store.cpp's create_opening_stats_rollups_template
// for why that asymmetry exists in the DuckDB rollup this mirrors.
auto accumulate_game(replay_game_record const& current_game,
                     std::uint16_t const max_root_ply,
                     hash_visit_counter& visits,
                     build_state& state) -> result<void>
{
    auto const result_code = map_result(current_game.result);
    auto const& white_elo = current_game.white_elo;
    auto const& black_elo = current_game.black_elo;

    auto board = motif::chess::board {};
    auto prev_hash = board.hash();

    gtl::flat_hash_set<std::uint64_t> visited_this_game;
    // Per-game dedup + min-root-ply-within-this-game tracking, matching
    // edge_deduped's "GROUP BY p_root.game_id, ..." + MIN(p_root.ply).
    gtl::flat_hash_map<edge_key, std::uint16_t, edge_key_hash> edges_this_game;
    gtl::flat_hash_set<std::uint64_t> roots_this_game;

    if (visited_this_game.insert(prev_hash).second) {
        if (auto visit_res = visits.visit(prev_hash); !visit_res) {
            return visit_res;
        }
    }
    roots_this_game.insert(prev_hash);

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

        if (root_ply + 1U <= max_root_ply) {
            roots_this_game.insert(child_hash);
        }

        prev_hash = child_hash;
    }

    for (auto const root_hash : roots_this_game) {
        ++state.root_game_counts[root_hash];
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
        agg.eco_sample_min = std::min(agg.eco_sample_min, current_game.id.value);
        agg.eco_sample_max = std::max(agg.eco_sample_max, current_game.id.value);
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
    auto keys = std::vector<edge_key> {};
    if (auto const edge_iter = state.edges_by_root.find(root_hash); edge_iter != state.edges_by_root.end()) {
        keys = edge_iter->second;
    }
    std::ranges::sort(keys, {}, &edge_key::encoded_move);

    if (!write_little_endian(output, root_hash) || !write_varint(output, state.root_game_counts.at(root_hash))
        || !write_varint(output, keys.size()))
    {
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
    roots.reserve(state.root_game_counts.size());
    for (auto const& [root_hash, game_count] : state.root_game_counts) {
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

struct opening_tree_index_builder::state
{
    std::filesystem::path path;
    opening_tree_index_build_options options;
    hash_visit_counter visits;
    build_state aggregate;
    bool finalized {false};

    state(std::filesystem::path output_path, opening_tree_index_build_options build_options)
        : path {std::move(output_path)}
        , options {build_options}
        , visits {std::filesystem::path {path.string() + ".childfreq"}, default_spill_threshold}
    {
    }
};

opening_tree_index_builder::opening_tree_index_builder(std::filesystem::path path, opening_tree_index_build_options options)
    : state_ {std::make_unique<state>(std::move(path), options)}
{
}

opening_tree_index_builder::~opening_tree_index_builder() = default;
opening_tree_index_builder::opening_tree_index_builder(opening_tree_index_builder&&) noexcept = default;
auto opening_tree_index_builder::operator=(opening_tree_index_builder&&) noexcept -> opening_tree_index_builder& = default;

auto opening_tree_index_builder::accumulate(replay_game_record const& game) -> result<void>
{
    if (state_->finalized) {
        return tl::unexpected {error_code::invalid_argument};
    }
    return accumulate_game(game, state_->options.max_root_ply, state_->visits, state_->aggregate);
}

auto opening_tree_index_builder::finalize() -> result<void>
{
    if (state_->finalized) {
        return tl::unexpected {error_code::invalid_argument};
    }
    state_->finalized = true;

    gtl::flat_hash_set<std::uint64_t> needed_child_hashes;
    needed_child_hashes.reserve(state_->aggregate.edges.size());
    for (auto const& entry : state_->aggregate.edges) {
        needed_child_hashes.insert(entry.first.child_hash);
    }
    auto child_counts = state_->visits.finalize(std::uint32_t {2}, needed_child_hashes);
    if (!child_counts) {
        return tl::unexpected {child_counts.error()};
    }
    return write_index_file(state_->path, state_->options, state_->aggregate, *child_counts);
}

auto opening_tree_index::build(game_store& store, std::filesystem::path const& path, build_options const& opts) -> result<void>
{
    auto builder = opening_tree_index_builder {path, opts};
    auto const replay_res =
        store.for_each_replay_game([&builder](replay_game_record const& game) -> result<void> { return builder.accumulate(game); });
    if (!replay_res) {
        return tl::unexpected {replay_res.error()};
    }
    return builder.finalize();
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
    // No reserve(): node_count is untrusted, straight from the file header --
    // reserving against it could throw before the truncation check below runs.
    index.node_offsets_.push_back(0);

    for (std::uint64_t node_index = 0; node_index < node_count; ++node_index) {
        std::uint64_t node_hash {};
        std::uint64_t game_count_raw {};
        std::uint64_t continuation_count {};
        if (!read_little_endian(input, node_hash) || !read_varint(input, game_count_raw) || !read_varint(input, continuation_count)) {
            return tl::unexpected {error {error_code::io_failure, "truncated node header"}};
        }
        index.node_hashes_.push_back(node_hash);
        index.node_game_counts_.push_back(static_cast<std::uint32_t>(game_count_raw));

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

auto opening_tree_index::game_count(zobrist_hash const hash) const -> result<std::uint32_t>
{
    auto const lower = std::ranges::lower_bound(node_hashes_, hash.value);
    auto const node_index = static_cast<std::size_t>(lower - node_hashes_.begin());
    if (node_index >= node_hashes_.size() || node_hashes_[node_index] != hash.value) {
        return std::uint32_t {0};
    }
    return node_game_counts_[node_index];
}

}  // namespace motif::db
