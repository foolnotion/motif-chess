// PR 6a: the manager opens, installs, serves and validates composite snapshots
// (checkpoint + sorted tree runs, postings metadata chain), publishes them in the
// same FULL-commit protocol, and falls back or repairs exactly when a chain is
// corrupt. There is no production writer of runs in 6a, so the tests build runs
// through the new codecs and publish them through publish_snapshot_chain(),
// emulating an edit by changing canonical SQLite through a second connection.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include "composite_test_support.hpp"
#include "equivalence_support.hpp"
#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/fault_injection.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_composite.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/opening_tree_patch.hpp"
#include "motif/db/postings_metadata_patch.hpp"

namespace motif::db
{

// The friend of database_manager that reaches its private snapshot publication
// protocol: these tests hand-build run chains after changing canonical SQLite
// through a second connection, which no production API allows.
struct snapshot_publication_test_access
{
    static auto reserve(database_manager& manager, derived_artifact_family const family) -> result<staged_derived_artifact>
    {
        return manager.reserve_derived_artifact(family);
    }

    static auto seal(database_manager const& /*manager*/, staged_derived_artifact const& staged, derived_artifact_family const family)
        -> result<artifact_descriptor>
    {
        return database_manager::seal_derived_artifact(staged, family);
    }

    static void discard(database_manager const& /*manager*/, staged_derived_artifact const& staged) noexcept
    {
        database_manager::discard_derived_artifact(staged);
    }

    static auto publish(database_manager& manager, snapshot_chain_publication publication) -> result<void>
    {
        return manager.publish_snapshot_chain(std::move(publication));
    }
};

}  // namespace motif::db

namespace
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the file exercises most of motif::db; qualifying every name buries
                            // the assertions.
using motif_test::temp_dir;

using motif_test::distribution_equal;
using motif_test::oracle_distribution;
using motif_test::probe_hashes;
using motif_test::rows_equal;

// The v5 checkpoint writer stored half of every doubled weighted sum.
constexpr auto weight_scale = 2.0;
constexpr auto v5_format_version = std::uint32_t {5};
// A small rating for the sample metadata record, and the Elo bucket width the distribution queries use.
constexpr auto sample_rating = std::int16_t {5};
constexpr auto bucket_width = 50;

// ── The edit harness ─────────────────────────────────────────────────────────

struct sql_connection
{
    sqlite3* handle {};

    explicit sql_connection(std::filesystem::path const& db_path)
    {
        REQUIRE(sqlite3_open_v2(db_path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
        constexpr auto busy_timeout_ms = 5'000;
        sqlite3_busy_timeout(handle, busy_timeout_ms);
    }

    ~sql_connection() { sqlite3_close(handle); }

    sql_connection(sql_connection const&) = delete;
    auto operator=(sql_connection const&) -> sql_connection& = delete;
    sql_connection(sql_connection&&) = delete;
    auto operator=(sql_connection&&) -> sql_connection& = delete;

    void exec(std::string const& sql) const
    {
        char* message = nullptr;
        auto const status = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &message);
        INFO((message != nullptr ? std::string {message} : std::string {}));
        REQUIRE(status == SQLITE_OK);
        sqlite3_free(message);
    }
};

auto sql_elo(std::optional<std::int32_t> const& elo) -> std::string
{
    return elo ? std::to_string(*elo) : std::string {"NULL"};
}

struct harness
{
    temp_dir dir {"harness"};
    std::filesystem::path bundle;
    std::vector<motif_test::model_game> corpus;
    std::unique_ptr<database_manager> manager;
    bool with_tree;

    harness(std::uint64_t const seed, std::size_t const random_games, bool const tree)
        : bundle {dir.path / "db"}
        , corpus {motif_test::make_corpus(seed, random_games)}
        , manager {std::make_unique<database_manager>(motif_test::build_bundle(bundle, corpus, tree))}
        , with_tree {tree}
    {
    }

    [[nodiscard]] auto identity() const -> identity_bytes
    {
        return motif_test::require_value(identity_from_hex(manager->manifest().payload.bundle.id));
    }

    [[nodiscard]] auto sequence() const -> std::uint64_t { return manager->derived_index_status()->mutation_seq; }

    [[nodiscard]] auto covered_seq() const -> std::uint64_t { return motif_test::require_value(manager->manifest().covered_mutation_seq); }

    void reopen()
    {
        manager.reset();
        auto reopened = database_manager::open(bundle);
        REQUIRE(reopened.has_value());
        manager = std::make_unique<database_manager>(std::move(*reopened));
    }

    auto publish(snapshot_chain_publication publication) const -> result<void>
    {
        return snapshot_publication_test_access::publish(*manager, std::move(publication));
    }

    void discard(staged_derived_artifact const& staged) const { snapshot_publication_test_access::discard(*manager, staged); }

    // Writes `write`'s artifact into a fresh staging file and seals it.
    template<typename Writer>
    auto stage(derived_artifact_family const family, Writer const& write) -> std::pair<staged_derived_artifact, artifact_descriptor>
    {
        auto staged = snapshot_publication_test_access::reserve(*manager, family);
        REQUIRE(staged.has_value());
        REQUIRE(write(staged->staging_path).has_value());
        auto descriptor = snapshot_publication_test_access::seal(*manager, *staged, family);
        REQUIRE(descriptor.has_value());
        return {std::move(*staged), std::move(*descriptor)};
    }

    // Emulates a 6b edit: change canonical SQLite (result and both ratings) and
    // advance the mutation sequence once, then publish the metadata after-image run
    // and the signed tree run for that epoch in one FULL-commit snapshot.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this one linear edit sequence.
    void edit(std::size_t const index, motif_test::model_game const& after)
    {
        auto const before = corpus[index];
        auto const from = sequence();
        {
            auto const connection = sql_connection {bundle / "games.db"};
            connection.exec("UPDATE game SET result = '" + after.result + "', white_elo = " + sql_elo(after.white_elo)
                            + ", black_elo = " + sql_elo(after.black_elo) + " WHERE id = " + std::to_string(index + 1) + ";");
            connection.exec("UPDATE mutation_state SET mutation_seq = mutation_seq + 1 WHERE id = 1;");
        }
        auto const to_seq = from + 1;
        REQUIRE(sequence() == to_seq);
        CHECK_FALSE(manager->derived_index_status()->postings_usable);  // stale until the snapshot publishes

        auto payload = manager->manifest().payload;
        auto staged_files = std::vector<staged_derived_artifact> {};

        auto const metadata_record = position_metadata_after_image {
            .id = game_id {static_cast<std::uint32_t>(index + 1)},
            .content_revision = 1,
            .result = encode_result_code(after.result),
            .white_elo = after.white_elo ? std::optional<std::int16_t> {static_cast<std::int16_t>(*after.white_elo)} : std::nullopt,
            .black_elo = after.black_elo ? std::optional<std::int16_t> {static_cast<std::int16_t>(*after.black_elo)} : std::nullopt};
        auto metadata = metadata_artifact {.identity = {.bundle_id = identity(), .index_semantics = index_semantics_version},
                                           .kind = metadata_artifact_kind::run,
                                           .from_exclusive_seq = from,
                                           .to_inclusive_seq = to_seq,
                                           .records = {metadata_record},
                                           .encoded_bytes = 0};
        auto [metadata_staged, metadata_descriptor] =
            stage(derived_artifact_family::postings_metadata,
                  [&](std::filesystem::path const& path) -> result<void> { return write_metadata_artifact(path, metadata); });
        staged_files.push_back(metadata_staged);
        payload.metadata.runs.push_back(
            patch_run_descriptor {.artifact = metadata_descriptor, .from_exclusive_seq = from, .to_inclusive_seq = to_seq});

        if (with_tree) {
            auto builder = tree_patch_builder {tree_identity()};
            REQUIRE(builder
                        .add_metadata_edit(motif_test::tree_state_of(before, static_cast<std::uint32_t>(index + 1)),
                                           motif_test::tree_state_of(after, static_cast<std::uint32_t>(index + 1)))
                        .has_value());
            auto run = builder.build(from, to_seq);
            REQUIRE(run.has_value());
            auto [tree_staged, tree_descriptor] =
                stage(derived_artifact_family::tree_patch,
                      [&](std::filesystem::path const& path) -> result<void> { return write_tree_patch_run(path, *run); });
            staged_files.push_back(tree_staged);
            auto& tree_payload = motif_test::require_value(payload.tree);
            tree_payload.patch_runs.push_back(
                patch_run_descriptor {.artifact = tree_descriptor, .from_exclusive_seq = from, .to_inclusive_seq = to_seq});
            tree_payload.covered_mutation_seq = to_seq;
        }
        auto const published = publish(snapshot_chain_publication {.payload = std::move(payload), .staged = std::move(staged_files)});
        INFO((published.has_value() ? std::string {} : published.error().message));
        REQUIRE(published.has_value());
        corpus[index] = after;
        auto const status = manager->derived_index_status();
        REQUIRE(status.has_value());
        CHECK(status->postings_usable);
        CHECK(status->tree_usable == with_tree);
    }

    [[nodiscard]] auto tree_identity() const -> tree_patch_identity
    {
        return tree_patch_identity {.bundle_id = identity(),
                                    .index_semantics = index_semantics_version,
                                    .canonical_start_hash = motif_test::start_hash(),
                                    .max_root_ply = opening_tree_index_default_max_root_ply};
    }

    // Folds the tree checkpoint + every run into a new v6 checkpoint and publishes it
    // (same sequence, new generation).
    void fold_tree()
    {
        REQUIRE(with_tree);
        auto const& manifest = manager->manifest();
        auto const tree_descriptor_copy = *manifest.tree();
        auto composite = motif_test::load_chain_tree(bundle, manifest);
        REQUIRE(composite.has_value());
        auto payload = manifest.payload;
        auto [staged, descriptor] =
            stage(derived_artifact_family::tree_checkpoint,
                  [&](std::filesystem::path const& path) -> result<void>
                  {
                      auto folded = fold_tree_chain(composite->checkpoint(), composite->runs(), composite->child_frequency_mode(), path);
                      if (!folded) {
                          return tl::unexpected {folded.error()};
                      }
                      return {};
                  });
        auto& tree_payload = motif_test::require_value(payload.tree);
        tree_payload.checkpoint = descriptor;
        tree_payload.base_mutation_seq = tree_descriptor_copy.covered_mutation_seq;
        tree_payload.patch_runs.clear();
        auto const published = publish(snapshot_chain_publication {.payload = std::move(payload), .staged = {staged}});
        INFO((published.has_value() ? std::string {} : published.error().message));
        REQUIRE(published.has_value());
        CHECK(manager->derived_index_status()->tree_usable);
    }

    // Folds the metadata chain into one sparse checkpoint and publishes it.
    void fold_metadata()
    {
        auto const& manifest = manager->manifest();
        auto const* segment = manifest.postings();
        REQUIRE(segment != nullptr);
        auto const metadata_identity = metadata_artifact_identity {.bundle_id = identity(), .index_semantics = index_semantics_version};
        auto loaded = std::vector<metadata_artifact> {};
        auto const load_member = [&](patch_run_descriptor const& member, metadata_artifact_kind const kind) -> void
        {
            auto artifact = load_metadata_artifact(bundle / member.artifact.filename,
                                                   member.artifact,
                                                   metadata_artifact_expectation {.identity = metadata_identity,
                                                                                  .kind = kind,
                                                                                  .from_exclusive_seq = member.from_exclusive_seq,
                                                                                  .to_inclusive_seq = member.to_inclusive_seq});
            REQUIRE(artifact.has_value());
            loaded.push_back(std::move(*artifact));
        };
        if (manifest.payload.metadata.checkpoint) {
            load_member(*manifest.payload.metadata.checkpoint, metadata_artifact_kind::checkpoint);
        }
        for (auto const& run : manifest.payload.metadata.runs) {
            load_member(run, metadata_artifact_kind::run);
        }
        auto pointers = std::vector<metadata_artifact const*> {};
        for (auto const& artifact : loaded) {
            pointers.push_back(&artifact);
        }
        auto folded = fold_metadata_chain(metadata_identity, segment->source_mutation_seq, pointers);
        REQUIRE(folded.has_value());
        auto payload = manifest.payload;
        auto [staged, descriptor] =
            stage(derived_artifact_family::postings_metadata,
                  [&](std::filesystem::path const& path) -> result<void> { return write_metadata_artifact(path, *folded); });
        payload.metadata = postings_metadata_chain {.checkpoint = patch_run_descriptor {.artifact = descriptor,
                                                                                        .from_exclusive_seq = folded->from_exclusive_seq,
                                                                                        .to_inclusive_seq = folded->to_inclusive_seq},
                                                    .runs = {}};
        auto const published = publish(snapshot_chain_publication {.payload = std::move(payload), .staged = {staged}});
        INFO((published.has_value() ? std::string {} : published.error().message));
        REQUIRE(published.has_value());
    }
};

auto random_edit(std::mt19937_64& rng, motif_test::model_game const& base) -> motif_test::model_game
{
    auto after = base;
    after.result = motif_test::pick_result(rng);
    if (rng() % 2U == 0U) {
        after.white_elo = motif_test::pick_elo(rng);
    }
    if (rng() % 2U == 0U) {
        after.black_elo = motif_test::pick_elo(rng);
    }
    return after;
}

// The shared checker: every query family on the patched bundle equals a fresh
// rebuild of its canonical data, and the unfiltered/Elo answers equal an
// independent oracle.
void require_equivalent_to_fresh(harness& patched, std::uint64_t const seed)
{
    motif_test::require_equivalent_to_fresh(*patched.manager, patched.bundle, patched.corpus, patched.with_tree, seed);
}

}  // namespace

// ── Equivalence through the manager ──────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("a composite snapshot (checkpoint + signed runs + metadata chain) answers exactly like a fresh rebuild",
          "[motif-db][pr6a][composite][equivalence]")
{
    constexpr auto random_games = std::size_t {14};
    constexpr auto edit_count = 14;
    constexpr auto reopen_seed_offset = 1000U;
    for (auto const seed : {401ULL, 402ULL}) {
        harness env {seed, random_games, /*tree=*/true};
        auto const initial_generation = env.manager->derived_index_status()->publication_generation;
        auto rng = std::mt19937_64 {seed};
        for (auto step = 0; step < edit_count; ++step) {
            auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
            env.edit(index, random_edit(rng, env.corpus[index]));
            require_equivalent_to_fresh(env, seed + static_cast<std::uint64_t>(step));
        }
        // Every edit was a publication, never a rebuild: one generation per edit.
        CHECK(env.manager->derived_index_status()->publication_generation == initial_generation + 14U);
        CHECK(env.manager->manifest().payload_version == snapshot_payload_version_2);
        CHECK(env.manager->manifest().payload.metadata.runs.size() == 14U);
        CHECK(env.manager->manifest().tree()->patch_runs.size() == 14U);
        CHECK(env.manager->manifest().tree()->base_mutation_seq + 14U == env.manager->manifest().tree()->covered_mutation_seq);
        CHECK(env.manager->manifest().postings()->source_mutation_seq + 14U == env.covered_seq());

        // Reopening serves the same state from the chain with no rebuild: the
        // published generation and payload are unchanged.
        auto const payload_before = env.manager->manifest().payload_json;
        auto const generation_before = env.manager->manifest().publication_generation;
        env.reopen();
        CHECK(env.manager->manifest().publication_generation == generation_before);
        CHECK(env.manager->manifest().payload_json == payload_before);
        require_equivalent_to_fresh(env, seed + reopen_seed_offset);
    }
}

TEST_CASE("a metadata-only chain on a bundle without a tree serves exact postings with the patched metadata", "[motif-db][pr6a][composite]")
{
    constexpr auto seed = std::uint64_t {410};
    constexpr auto random_games = std::size_t {10};
    constexpr auto edit_count = 8;
    harness env {seed, random_games, /*tree=*/false};
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    for (auto step = 0; step < edit_count; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
        env.edit(index, random_edit(rng, env.corpus[index]));
    }
    require_equivalent_to_fresh(env, seed);
    CHECK(env.manager->manifest().tree() == nullptr);
    env.reopen();
    require_equivalent_to_fresh(env, seed + 1U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("folding the tree and metadata chains keeps every answer and later edits continue from the fold",
          "[motif-db][pr6a][composite][fold]")
{
    constexpr auto seed = std::uint64_t {420};
    constexpr auto random_games = std::size_t {14};
    constexpr auto edits_before_fold = 5;
    constexpr auto second_fold_check = seed + 5U;
    harness env {seed, random_games, /*tree=*/true};
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    auto const edit_some = [&](int const count) -> void
    {
        for (auto step = 0; step < count; ++step) {
            auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
            env.edit(index, random_edit(rng, env.corpus[index]));
        }
    };
    edit_some(edits_before_fold);
    auto const referenced_before = referenced_artifact_filenames(env.manager->manifest().payload);
    env.fold_tree();
    {
        auto const* tree = env.manager->manifest().tree();
        REQUIRE(tree != nullptr);
        CHECK(tree->patch_runs.empty());
        CHECK(tree->base_mutation_seq == tree->covered_mutation_seq);
        CHECK(tree->checkpoint.format_version == opening_tree_index_format_version);
    }
    require_equivalent_to_fresh(env, seed + 1U);
    env.fold_metadata();
    {
        auto const& chain = env.manager->manifest().payload.metadata;
        auto const& checkpoint = motif_test::require_value(chain.checkpoint);
        CHECK(chain.runs.empty());
        CHECK(checkpoint.from_exclusive_seq == env.manager->manifest().postings()->source_mutation_seq);
        CHECK(checkpoint.to_inclusive_seq == env.covered_seq());
    }
    require_equivalent_to_fresh(env, seed + 2U);

    // Superseded chain members were collected exactly once; the live set remains.
    auto const live_now = referenced_artifact_filenames(env.manager->manifest().payload);
    for (auto const& name : referenced_before) {
        auto const still_live = std::ranges::find(live_now, name) != live_now.end();
        CHECK(std::filesystem::exists(env.bundle / name) == still_live);
    }
    for (auto const& name : live_now) {
        CHECK(std::filesystem::exists(env.bundle / name));
    }

    edit_some(4);  // new runs chain from the folded checkpoints
    CHECK(env.manager->manifest().tree()->patch_runs.size() == 4U);
    CHECK(env.manager->manifest().payload.metadata.runs.size() == 4U);
    require_equivalent_to_fresh(env, seed + 3U);
    env.reopen();
    require_equivalent_to_fresh(env, seed + 4U);
    // A second fold of fold-plus-runs equals the uncompacted chain, which equals a rebuild.
    env.fold_tree();
    env.fold_metadata();
    require_equivalent_to_fresh(env, second_fold_check);
}

// ── Rebuilds around a patched snapshot (integration addendum) ────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("a tree rebuild after patches clears only the tree runs; a postings rebuild clears only the metadata chain",
          "[motif-db][pr6a][composite]")
{
    constexpr auto seed = std::uint64_t {430};
    constexpr auto random_games = std::size_t {12};
    harness env {seed, random_games, /*tree=*/true};
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    for (auto step = 0; step < 4; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
        env.edit(index, random_edit(rng, env.corpus[index]));
    }
    auto const chain_before = env.manager->manifest().payload.metadata;
    REQUIRE(chain_before.runs.size() == 4U);

    REQUIRE(env.manager->rebuild_opening_tree_index().has_value());
    {
        auto const* tree = env.manager->manifest().tree();
        REQUIRE(tree != nullptr);
        CHECK(tree->patch_runs.empty());
        CHECK(tree->base_mutation_seq == env.covered_seq());
        CHECK(tree->covered_mutation_seq == env.covered_seq());
        CHECK(env.manager->manifest().payload.metadata == chain_before);  // a tree rebuild is not a metadata rebuild
    }
    require_equivalent_to_fresh(env, seed + 1U);

    // Edit again so both chains are non-empty, then rebuild postings.
    auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
    env.edit(index, random_edit(rng, env.corpus[index]));
    auto const tree_before = *env.manager->manifest().tree();
    REQUIRE(tree_before.patch_runs.size() == 1U);
    REQUIRE(env.manager->rebuild_position_postings().has_value());
    CHECK(env.manager->manifest().payload.metadata.empty());
    CHECK(env.manager->manifest().postings()->source_mutation_seq == env.covered_seq());
    REQUIRE(env.manager->manifest().tree() != nullptr);
    CHECK(*env.manager->manifest().tree() == tree_before);  // usable composite tree preserved, runs included
    require_equivalent_to_fresh(env, seed + 2U);
    env.reopen();
    require_equivalent_to_fresh(env, seed + 3U);
}

// ── Corruption: the whole composite tree is disabled; exact postings answer ──

namespace
{

void flip_byte(std::filesystem::path const& path, std::size_t const offset)
{
    std::fstream file {path, std::ios::in | std::ios::out | std::ios::binary};
    REQUIRE(file.is_open());
    file.seekg(static_cast<std::streamoff>(offset));
    char byte {};
    file.read(&byte, 1);
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&byte, 1);
}

auto patched_bundle_with_runs(std::uint64_t const seed) -> std::unique_ptr<harness>
{
    constexpr auto random_games = std::size_t {12};
    auto env = std::make_unique<harness>(seed, random_games, /*tree=*/true);
    auto rng = std::mt19937_64 {seed};
    for (auto step = 0; step < 4; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, env->corpus.size() - 1}(rng);
        env->edit(index, random_edit(rng, env->corpus[index]));
    }
    return env;
}

// Catch2 macros inflate one linear check sequence; the generation and sequence are distinct values every caller names.
// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters)
void require_tree_disabled_and_postings_exact(harness& env, std::uint64_t const generation_before, std::uint64_t const sequence_before)
{
    auto const status = env.manager->derived_index_status();
    REQUIRE(status.has_value());
    CHECK(status->postings_usable);
    CHECK_FALSE(status->tree_usable);
    CHECK(env.manager->manifest().tree() == nullptr);  // descriptor cleared by the optional-tree repair
    CHECK(status->mutation_seq == sequence_before);  // without advancing the mutation sequence
    CHECK(status->publication_generation == generation_before + 1U);  // one repair publication, no rebuild
    // The postings metadata chain survived, and the answers are correct.
    CHECK(env.manager->manifest().payload.metadata.runs.size() == 4U);
    temp_dir const fresh_dir {"fresh-after-corruption"};
    auto fresh = motif_test::build_bundle(fresh_dir.path / "db", env.corpus, /*with_tree=*/true);
    constexpr auto probe_seed = 99U;
    auto rng = std::mt19937_64 {probe_seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    for (auto const hash : probe_hashes(env.corpus, rng)) {
        auto const got = env.manager->query_unfiltered_opening_stats(zobrist_hash {hash});
        auto const want = fresh.query_unfiltered_opening_stats(zobrist_hash {hash});
        REQUIRE(got.has_value());
        REQUIRE(want.has_value());
        REQUIRE(rows_equal(*got, *want));
        auto const dist = env.manager->query_elo_distribution(zobrist_hash {hash}, search_filter {}, bucket_width);
        REQUIRE(dist.has_value());
        REQUIRE(distribution_equal(*dist, oracle_distribution(env.corpus, hash, bucket_width)));
    }
    // The dropped tree's files are no longer referenced and are collected.
    for (auto const& entry : std::filesystem::directory_iterator {env.bundle}) {
        auto const name = entry.path().filename().string();
        CHECK(name.rfind("opening_tree.patch.", 0) != 0U);
        CHECK(name.rfind("opening_tree.idx.", 0) != 0U);
    }
}

}  // namespace

TEST_CASE("a flipped byte in a tree run (checksum mismatch) disables the whole composite tree", "[motif-db][pr6a][composite][corruption]")
{
    constexpr auto seed = 501U;
    auto env = patched_bundle_with_runs(seed);
    auto const generation = env->manager->manifest().publication_generation;
    auto const sequence = env->sequence();
    auto const victim = env->bundle / env->manager->manifest().tree()->patch_runs[1].artifact.filename;
    env->manager.reset();
    flip_byte(victim, std::filesystem::file_size(victim) / 2);
    env->reopen();
    require_tree_disabled_and_postings_exact(*env, generation, sequence);
}

TEST_CASE("a truncated, missing, or swapped tree run (gap/reorder) disables the whole composite tree",
          "[motif-db][pr6a][composite][corruption]")
{
    SECTION("missing run")
    {
        constexpr auto seed = 502U;
        auto env = patched_bundle_with_runs(seed);
        auto const generation = env->manager->manifest().publication_generation;
        auto const sequence = env->sequence();
        auto const victim = env->bundle / env->manager->manifest().tree()->patch_runs[0].artifact.filename;
        env->manager.reset();
        std::filesystem::remove(victim);
        env->reopen();
        require_tree_disabled_and_postings_exact(*env, generation, sequence);
    }
    SECTION("truncated run")
    {
        constexpr auto seed = 503U;
        auto env = patched_bundle_with_runs(seed);
        auto const generation = env->manager->manifest().publication_generation;
        auto const sequence = env->sequence();
        auto const victim = env->bundle / env->manager->manifest().tree()->patch_runs[2].artifact.filename;
        env->manager.reset();
        std::filesystem::resize_file(victim, std::filesystem::file_size(victim) - 1);
        env->reopen();
        require_tree_disabled_and_postings_exact(*env, generation, sequence);
    }
    SECTION("two runs swapped on disk: their headers no longer match their descriptors' intervals")
    {
        constexpr auto seed = 504U;
        auto env = patched_bundle_with_runs(seed);
        auto const generation = env->manager->manifest().publication_generation;
        auto const sequence = env->sequence();
        auto const& runs = env->manager->manifest().tree()->patch_runs;
        auto const first = env->bundle / runs[0].artifact.filename;
        auto const second = env->bundle / runs[1].artifact.filename;
        env->manager.reset();
        auto const scratch = env->bundle / "swap.tmp";
        std::filesystem::rename(first, scratch);
        std::filesystem::rename(second, first);
        std::filesystem::rename(scratch, second);
        env->reopen();
        require_tree_disabled_and_postings_exact(*env, generation, sequence);
    }
    SECTION("the checkpoint itself is damaged")
    {
        constexpr auto seed = 505U;
        auto env = patched_bundle_with_runs(seed);
        auto const generation = env->manager->manifest().publication_generation;
        auto const sequence = env->sequence();
        auto const victim = env->bundle / env->manager->manifest().tree()->checkpoint.filename;
        env->manager.reset();
        constexpr auto checkpoint_offset = std::size_t {40};
        flip_byte(victim, checkpoint_offset);
        env->reopen();
        require_tree_disabled_and_postings_exact(*env, generation, sequence);
    }
}

TEST_CASE("a corrupt metadata run is repaired from canonical SQLite and never served as the stale baseline",
          "[motif-db][pr6a][composite][corruption]")
{
    constexpr auto seed = std::uint64_t {511};
    auto env = patched_bundle_with_runs(seed);
    auto const generation = env->manager->manifest().publication_generation;
    auto const sequence = env->sequence();
    auto const tree_before = *env->manager->manifest().tree();
    auto const victim = env->bundle / env->manager->manifest().payload.metadata.runs[2].artifact.filename;
    env->manager.reset();
    constexpr auto bytes_from_end = 5U;
    flip_byte(victim, std::filesystem::file_size(victim) - bytes_from_end);
    env->reopen();

    auto const status = env->manager->derived_index_status();
    REQUIRE(status.has_value());
    CHECK(status->postings_usable);
    CHECK(status->tree_usable);  // a bad metadata chain must not take a healthy tree with it
    CHECK(status->mutation_seq == sequence);
    CHECK(status->publication_generation == generation + 1U);  // one repair publication
    auto const& chain = env->manager->manifest().payload.metadata;
    auto const& checkpoint = motif_test::require_value(chain.checkpoint);  // replaced by one sparse checkpoint at the current sequence
    CHECK(chain.runs.empty());
    CHECK(checkpoint.from_exclusive_seq == env->manager->manifest().postings()->source_mutation_seq);
    CHECK(checkpoint.to_inclusive_seq == sequence);
    CHECK(*env->manager->manifest().tree() == tree_before);  // tree chain untouched
    require_equivalent_to_fresh(*env, seed + 1U);  // canonical values, not the baseline's

    // The repaired state is durable: reopening changes nothing and rebuilds nothing.
    auto const payload_json = env->manager->manifest().payload_json;
    env->reopen();
    CHECK(env->manager->manifest().payload_json == payload_json);
    CHECK(env->manager->manifest().publication_generation == generation + 1U);
    require_equivalent_to_fresh(*env, seed + 2U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("a metadata chain that cannot be repaired cheaply falls back to a postings rebuild that keeps a healthy tree",
          "[motif-db][pr6a][composite][corruption]")
{
    constexpr auto seed = 521U;
    auto env = patched_bundle_with_runs(seed);
    auto const generation = env->manager->manifest().publication_generation;
    auto const sequence = env->sequence();
    auto const tree_before = *env->manager->manifest().tree();
    auto const victim = env->bundle / env->manager->manifest().payload.metadata.runs[0].artifact.filename;
    env->manager.reset();
    constexpr auto bytes_from_end = 5U;
    flip_byte(victim, std::filesystem::file_size(victim) - bytes_from_end);
    {
        // The membership precondition of the cheap repair fails (a content revision
        // the baseline cannot account for).
        auto const connection = sql_connection {env->bundle / "games.db"};
        connection.exec("UPDATE game SET content_revision = 2 WHERE id = 1;");
    }
    env->reopen();

    auto const status = env->manager->derived_index_status();
    REQUIRE(status.has_value());
    CHECK(status->postings_usable);
    CHECK(status->mutation_seq == sequence);  // a rebuild does not advance the sequence
    CHECK(status->publication_generation == generation + 1U);
    CHECK(env->manager->manifest().payload.metadata.empty());  // the new segment embeds current metadata
    CHECK(env->manager->manifest().postings()->source_mutation_seq == sequence);
    REQUIRE(env->manager->manifest().tree() != nullptr);
    CHECK(*env->manager->manifest().tree() == tree_before);  // composite tree preserved with its runs
    CHECK(status->tree_usable);
    constexpr auto probe_seed = 5U;
    auto rng = std::mt19937_64 {probe_seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    for (auto const hash : probe_hashes(env->corpus, rng)) {
        auto const dist = env->manager->query_elo_distribution(zobrist_hash {hash}, search_filter {}, bucket_width);
        REQUIRE(dist.has_value());
        REQUIRE(distribution_equal(*dist, oracle_distribution(env->corpus, hash, bucket_width)));
    }
}

// ── Authoritative payload, v1 compatibility, GC ──────────────────────────────

namespace
{

auto rewrite_payload(std::filesystem::path const& bundle, std::string const& payload_json, std::uint32_t const payload_version) -> void
{
    auto const connection = sql_connection {bundle / "games.db"};
    sqlite3_stmt* raw = nullptr;
    REQUIRE(sqlite3_prepare_v2(connection.handle,
                               "UPDATE published_snapshot SET payload_json = ?1, payload_checksum = ?2, payload_version = ?3 WHERE id = 1",
                               -1,
                               &raw,
                               nullptr)
            == SQLITE_OK);
    auto const checksum = checksum_to_hex(fnv1a64(payload_json));
    sqlite3_bind_text(raw, 1, payload_json.c_str(), static_cast<int>(payload_json.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 2, checksum.c_str(), static_cast<int>(checksum.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(raw, 3, static_cast<int>(payload_version));
    REQUIRE(sqlite3_step(raw) == SQLITE_DONE);
    sqlite3_finalize(raw);
}

// The derived-artifact files in the bundle (SQLite sidecars and the manifest cache
// come and go with open/close and are not what these tests are about).
auto list_bundle(std::filesystem::path const& bundle) -> std::set<std::string>
{
    auto names = std::set<std::string> {};
    for (auto const& entry : std::filesystem::directory_iterator {bundle}) {
        auto const name = entry.path().filename().string();
        if (name.starts_with("positions.") || name.starts_with("opening_tree.")) {
            names.insert(name);
        }
    }
    return names;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("a malformed authoritative chain payload fails the open closed and preserves every file",
          "[motif-db][pr6a][composite][corruption]")
{
    constexpr auto seed = 531U;
    auto env = patched_bundle_with_runs(seed);
    auto const json = env->manager->manifest().payload_json;
    auto const before = list_bundle(env->bundle);
    env->manager.reset();

    auto const attempt = [&](std::string const& forged_json, std::uint32_t const version) -> error_code
    {
        rewrite_payload(env->bundle, forged_json, version);
        auto opened = database_manager::open(env->bundle);
        REQUIRE_FALSE(opened.has_value());
        return opened.error().code;
    };
    // A gap in the tree chain (valid JSON, honest checksum): the payload is not a trustworthy inventory.
    // (the manager is closed: the forgeries below are derived from the saved text)
    auto gap_json = json;
    auto const key_pos = gap_json.find("\"from_exclusive_seq\":");
    REQUIRE(key_pos != std::string::npos);
    auto const value_at = gap_json.find(':', key_pos) + 2;
    gap_json[value_at] = gap_json[value_at] == '9' ? '8' : static_cast<char>(gap_json[value_at] + 1);
    CHECK(attempt(gap_json, 2) == error_code::io_failure);
    // An unknown descriptor field.
    CHECK(attempt(std::string {"{\"unknown\":1,"} + json.substr(1), 2) == error_code::io_failure);
    // A future payload version is a distinct error and still preserves everything.
    CHECK(attempt(json, 3) == error_code::schema_mismatch);
    // A v2 payload mislabelled as v1.
    CHECK(attempt(json, 1) == error_code::io_failure);
    CHECK(list_bundle(env->bundle) == before);

    // Restoring the real payload makes the same bundle open again, unchanged.
    rewrite_payload(env->bundle, json, 2);
    auto restored = database_manager::open(env->bundle);
    REQUIRE(restored.has_value());
    CHECK(restored->derived_index_status()->tree_usable);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("bundles published before chains exist open unchanged: payload v1 with a v5 checkpoint", "[motif-db][pr6a][composite][v5]")
{
    SECTION("postings only")
    {
        temp_dir const dir {"v1-postings"};
        constexpr auto seed = 541U;
        constexpr auto random_games = std::size_t {8};
        auto manager = motif_test::build_bundle(dir.path / "db", motif_test::make_corpus(seed, random_games), /*with_tree=*/false);
        auto const json = manager.manifest().payload_json;
        auto const generation = manager.manifest().publication_generation;
        CHECK(manager.manifest().payload_version == snapshot_payload_version_1);
        auto reopened_path = dir.path / "db";
        manager.close();
        auto reopened = database_manager::open(reopened_path);
        REQUIRE(reopened.has_value());
        CHECK(reopened->manifest().payload_version == snapshot_payload_version_1);
        CHECK(reopened->manifest().payload_json == json);
        CHECK(reopened->manifest().publication_generation == generation);
    }
    SECTION("v5 tree checkpoint under a version 1 payload")
    {
        temp_dir const dir {"v1-v5-tree"};
        constexpr auto seed = 542U;
        constexpr auto random_games = std::size_t {10};
        constexpr auto check_seed = 5421U;
        auto corpus = motif_test::make_corpus(seed, random_games);
        auto manager = motif_test::build_bundle(dir.path / "db", corpus, /*with_tree=*/true);
        auto const bundle = dir.path / "db";
        auto const* tree = manager.manifest().tree();
        REQUIRE(tree != nullptr);
        auto const v6_path = bundle / tree->checkpoint.filename;
        auto v6_tree = opening_tree_index::open(v6_path);
        REQUIRE(v6_tree.has_value());
        auto const nodes = motif_test::collect_nodes(*v6_tree);
        auto const expected_rows = manager.query_unfiltered_opening_stats(zobrist_hash {motif_test::start_hash()});
        REQUIRE(expected_rows.has_value());
        auto payload = manager.manifest().payload;
        manager.close();

        // Convert the checkpoint to v5 exactly as PR 5 wrote it (negative sums as u64).
        auto v5_bytes = std::string {"MOTIFOT1"};
        append_fixed_le(v5_bytes, v5_format_version);
        append_fixed_le(v5_bytes, v6_tree->max_root_ply());
        append_fixed_le(v5_bytes, v6_tree->source_game_count());
        append_fixed_le(v5_bytes, static_cast<std::uint64_t>(nodes.size()));
        for (auto const& node : nodes) {
            append_fixed_le(v5_bytes, node.root_hash);
            append_uleb128(v5_bytes, node.game_count);
            append_uleb128(v5_bytes, node.edges.size());
            append_uleb128(v5_bytes, node.complete ? 1U : 0U);
            for (auto const& edge : node.edges) {
                append_fixed_le(v5_bytes, edge.key.encoded_move);
                append_fixed_le(v5_bytes, edge.key.child_hash);
                for (auto const value : {static_cast<std::uint64_t>(edge.min_root_ply),
                                         static_cast<std::uint64_t>(edge.stats.frequency),
                                         static_cast<std::uint64_t>(edge.stats.white_wins),
                                         static_cast<std::uint64_t>(edge.stats.draws),
                                         static_cast<std::uint64_t>(edge.stats.black_wins),
                                         static_cast<std::uint64_t>(edge.stats.white_sum),
                                         static_cast<std::uint64_t>(edge.stats.white_count),
                                         static_cast<std::uint64_t>(edge.stats.black_sum),
                                         static_cast<std::uint64_t>(edge.stats.black_count)})
                {
                    append_uleb128(v5_bytes, value);
                }
                append_fixed_le(v5_bytes, std::bit_cast<std::uint64_t>(static_cast<double>(edge.stats.numerator2) / weight_scale));
                append_fixed_le(v5_bytes, std::bit_cast<std::uint64_t>(static_cast<double>(edge.stats.weight2) / weight_scale));
                append_uleb128(v5_bytes, edge.sample_min);
                append_uleb128(v5_bytes, edge.sample_max);
                append_uleb128(v5_bytes, edge.child_frequency.value_or(0U));
            }
        }
        auto const v5_name = std::string {tree_artifact_prefix} + generate_random_id();
        {
            std::ofstream out {bundle / v5_name, std::ios::binary};
            out.write(v5_bytes.data(), static_cast<std::streamsize>(v5_bytes.size()));
        }
        auto& tree_payload = motif_test::require_value(payload.tree);
        tree_payload.checkpoint = artifact_descriptor {.id = v5_name.substr(tree_artifact_prefix.size()),
                                                       .filename = v5_name,
                                                       .format = std::string {tree_format_name},
                                                       .format_version = opening_tree_index_legacy_format_version,
                                                       .size_bytes = v5_bytes.size(),
                                                       .checksum = fnv1a64(v5_bytes)};
        constexpr auto snapshot_generation = 7U;
        auto const rebuilt = build_manifest(snapshot_generation, tree_payload.covered_mutation_seq, corpus.size(), payload);
        REQUIRE(rebuilt.has_value());
        CHECK(rebuilt->payload_version == snapshot_payload_version_1);  // PR 5's shape, byte for byte
        // Swap the snapshot row to the v1 payload, keeping its generation.
        rewrite_payload(bundle, rebuilt->payload_json, snapshot_payload_version_1);
        {
            auto const connection = sql_connection {bundle / "games.db"};
            connection.exec("UPDATE published_snapshot SET publication_generation = " + std::to_string(snapshot_generation)
                            + " WHERE id = 1;");
        }

        auto reopened = database_manager::open(bundle);
        REQUIRE(reopened.has_value());
        CHECK(reopened->manifest().payload_version == snapshot_payload_version_1);
        CHECK(reopened->manifest().publication_generation == 7U);  // no republication, no rewrite
        CHECK(reopened->derived_index_status()->tree_usable);
        CHECK(std::filesystem::exists(bundle / v5_name));
        auto const rows = reopened->query_unfiltered_opening_stats(zobrist_hash {motif_test::start_hash()});
        REQUIRE(rows.has_value());
        CHECK(rows_equal(*rows, *expected_rows));

        // The first PR 6b edit on this PR 5-shaped bundle does not rewrite the v5 checkpoint: it
        // appends a signed run to it (the payload becomes version 2 because it now has a chain).
        auto const v5_digest = fnv1a64(v5_bytes);
        auto edit = game_patch {};
        edit.result = corpus[3].result == "1-0" ? "0-1" : "1-0";
        constexpr auto corrected_rating = 1234;
        edit.white_elo = rating_patch::set(corrected_rating);
        REQUIRE(reopened->patch_game_metadata(game_id {4U}, edit).has_value());
        corpus[3].result = *edit.result;
        corpus[3].white_elo = corrected_rating;
        CHECK(reopened->manifest().tree()->checkpoint.filename == v5_name);
        CHECK(reopened->manifest().tree()->checkpoint.format_version == opening_tree_index_legacy_format_version);
        CHECK(reopened->manifest().tree()->patch_runs.size() == 1U);
        CHECK(reopened->manifest().payload_version == snapshot_payload_version_2);
        {
            auto input = std::ifstream {bundle / v5_name, std::ios::binary};
            auto const bytes = std::string {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
            CHECK(fnv1a64(bytes) == v5_digest);
        }
        motif_test::require_equivalent_to_fresh(*reopened, bundle, corpus, /*with_tree=*/true, check_seed);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("garbage collection keeps every referenced chain file and never touches look-alikes", "[motif-db][pr6a][composite][gc]")
{
    constexpr auto seed = std::uint64_t {551};
    auto env = patched_bundle_with_runs(seed);
    auto const& manifest = env->manager->manifest();
    auto const live = referenced_artifact_filenames(manifest.payload);
    REQUIRE(live.size() >= 1U + 4U + 1U + 4U);
    env->manager.reset();

    auto const write = [&](std::string const& name, std::string const& content = "x") -> void
    {
        std::ofstream out {env->bundle / name, std::ios::binary};
        out << content;
    };
    auto const orphan_hex = generate_random_id();
    auto const orphans = std::vector<std::string> {std::string {"opening_tree.patch."} + orphan_hex,
                                                   std::string {"positions.metadata."} + orphan_hex,
                                                   std::string {"positions.metadata."} + orphan_hex + ".building",
                                                   std::string {"opening_tree.patch."} + orphan_hex + ".tmp",
                                                   std::string {"opening_tree.idx."} + orphan_hex};
    auto const look_alikes = std::vector<std::string> {"opening_tree.patch.keepme",
                                                       "positions.metadata.zzzz",
                                                       std::string {"positions.metadata."} + orphan_hex + ".keep",
                                                       std::string {"opening_tree.patch."} + orphan_hex + ".txt",
                                                       "opening_tree.patches.0123",
                                                       "positions.metadata",
                                                       "notes.txt",
                                                       "positions.duckdb"};
    for (auto const& name : orphans) {
        write(name);
    }
    for (auto const& name : look_alikes) {
        write(name);
    }
    // A symlink with an owned-looking name is never followed or removed.
    auto const target = env->dir.path / "outside.bin";
    {
        std::ofstream out {target, std::ios::binary};
        out << "outside";
    }
    auto const link = std::string {"positions.metadata."} + generate_random_id();
    std::error_code link_error;
    std::filesystem::create_symlink(target, env->bundle / link, link_error);
    REQUIRE_FALSE(link_error);

    env->reopen();  // open() ends with a garbage collection
    for (auto const& name : live) {
        CHECK(std::filesystem::exists(env->bundle / name));
    }
    for (auto const& name : orphans) {
        CHECK_FALSE(std::filesystem::exists(env->bundle / name));
    }
    for (auto const& name : look_alikes) {
        CHECK(std::filesystem::exists(env->bundle / name));
    }
    CHECK(std::filesystem::is_symlink(env->bundle / link));
    CHECK(std::filesystem::exists(target));
    require_equivalent_to_fresh(*env, seed + 1U);
}

// ── Publication protocol: validation and failure boundaries ──────────────────

namespace
{

auto stage_one_run(harness& env, std::uint64_t const from, std::uint64_t const to_seq, derived_artifact_family const family)
    -> std::pair<staged_derived_artifact, artifact_descriptor>
{
    if (family == derived_artifact_family::postings_metadata) {
        auto artifact = metadata_artifact {
            .identity = {.bundle_id = env.identity(), .index_semantics = index_semantics_version},
            .kind = metadata_artifact_kind::run,
            .from_exclusive_seq = from,
            .to_inclusive_seq = to_seq,
            .records = {position_metadata_after_image {
                .id = game_id {1}, .content_revision = 1, .result = 1, .white_elo = sample_rating, .black_elo = std::nullopt}},
            .encoded_bytes = 0};
        return env.stage(family,
                         [&](std::filesystem::path const& path) -> result<void> { return write_metadata_artifact(path, artifact); });
    }
    auto run = tree_patch_run {.identity = env.tree_identity(),
                               .operations = tree_patch_operation_metadata,
                               .from_exclusive_seq = from,
                               .to_inclusive_seq = to_seq,
                               .nodes = {},
                               .edges = {}};
    return env.stage(family, [&](std::filesystem::path const& path) -> result<void> { return write_tree_patch_run(path, run); });
}

// The canonical half of an edit: change game 2's result and advance the sequence once.
auto advance_canonical(harness& env) -> std::uint64_t
{
    auto const from = env.sequence();
    auto const connection = sql_connection {env.bundle / "games.db"};
    connection.exec("UPDATE game SET result = '1/2-1/2' WHERE id = 2;");
    connection.exec("UPDATE mutation_state SET mutation_seq = mutation_seq + 1 WHERE id = 1;");
    return from;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("publish_snapshot_chain rejects bad payloads before touching anything and cleans up its own files",
          "[motif-db][pr6a][composite][publication]")
{
    constexpr auto seed = 561U;
    constexpr auto random_games = std::size_t {8};
    harness env {seed, random_games, /*tree=*/true};
    auto const json_before = env.manager->manifest().payload_json;
    auto const generation_before = env.manager->manifest().publication_generation;
    auto const files_before = list_bundle(env.bundle);
    auto const unchanged = [&]() -> void
    {
        CHECK(env.manager->manifest().payload_json == json_before);
        CHECK(env.manager->manifest().publication_generation == generation_before);
        CHECK(list_bundle(env.bundle) == files_before);  // the attempt's staged and renamed files are gone
    };

    // Nothing advanced the sequence, so a chain member beyond it cannot be reached by
    // the snapshot's coverage.
    {
        auto const sequence = env.sequence();
        auto payload = env.manager->manifest().payload;
        auto [staged, descriptor] = stage_one_run(env, sequence, sequence + 1, derived_artifact_family::postings_metadata);
        payload.metadata.runs.push_back(
            patch_run_descriptor {.artifact = descriptor, .from_exclusive_seq = sequence, .to_inclusive_seq = sequence + 1});
        CHECK_FALSE(env.publish({.payload = payload, .staged = {staged}}).has_value());
        unchanged();
    }
    // A staged artifact the payload does not reference.
    {
        auto [staged, descriptor] = stage_one_run(env, 0, 1, derived_artifact_family::tree_patch);
        (void)descriptor;
        CHECK_FALSE(env.publish({.payload = env.manager->manifest().payload, .staged = {staged}}).has_value());
        unchanged();
    }
    // From here the canonical sequence has advanced, so a well-formed payload exists.
    auto const from = advance_canonical(env);
    auto const well_formed = [&]() -> std::pair<snapshot_chain_publication, patch_run_descriptor>
    {
        auto payload = env.manager->manifest().payload;
        auto [staged, descriptor] = stage_one_run(env, from, from + 1, derived_artifact_family::postings_metadata);
        auto const member = patch_run_descriptor {.artifact = descriptor, .from_exclusive_seq = from, .to_inclusive_seq = from + 1};
        payload.metadata.runs.push_back(member);
        auto [tree_staged, tree_descriptor] = stage_one_run(env, from, from + 1, derived_artifact_family::tree_patch);
        auto& tree_payload = motif_test::require_value(payload.tree);
        tree_payload.patch_runs.push_back(
            patch_run_descriptor {.artifact = tree_descriptor, .from_exclusive_seq = from, .to_inclusive_seq = from + 1});
        tree_payload.covered_mutation_seq = from + 1;
        return {snapshot_chain_publication {.payload = std::move(payload), .staged = {staged, tree_staged}}, member};
    };
    // Bytes that disagree with their descriptor.
    {
        auto [publication, member] = well_formed();
        (void)member;
        publication.payload.metadata.runs.back().artifact.checksum ^= 1U;
        auto const published = env.publish(std::move(publication));
        CHECK_FALSE(published.has_value());
        unchanged();
    }
    // Overlapping and duplicated chain members, and a member under a foreign prefix.
    {
        auto [publication, member] = well_formed();
        publication.payload.metadata.runs.push_back(member);  // the same interval and artifact twice
        auto const published = env.publish(std::move(publication));
        CHECK_FALSE(published.has_value());
        unchanged();
    }
    {
        auto [publication, member] = well_formed();
        (void)member;
        auto& tree_payload = motif_test::require_value(publication.payload.tree);
        tree_payload.patch_runs.back().from_exclusive_seq = from > 0 ? from - 1 : from + 1;  // overlaps the base
        auto const published = env.publish(std::move(publication));
        CHECK_FALSE(published.has_value());
        unchanged();
    }
    // A staged file that does not exist any more (discarded) cannot be published.
    {
        auto [publication, member] = well_formed();
        (void)member;
        env.discard(publication.staged[0]);
        auto const published = env.publish(std::move(publication));
        CHECK_FALSE(published.has_value());
        unchanged();
    }
    // The snapshot stayed the old, now stale, one; a valid publication then succeeds.
    CHECK_FALSE(env.manager->derived_index_status()->postings_usable);
    auto [publication, member] = well_formed();
    (void)member;
    auto const published = env.publish(std::move(publication));
    REQUIRE(published.has_value());
    CHECK(env.manager->derived_index_status()->postings_usable);
    CHECK(env.manager->derived_index_status()->publication_generation == generation_before + 1U);
}

namespace
{

// The fault hook is a plain function pointer, so it reads process-global state.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline auto g_fault_point = motif::db::fault::point::before_build;
inline auto g_fault_hits = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) -- see g_fault_point.

auto fail_at_selected_point(motif::db::fault::point const where,
                            motif::db::fault::artifact_kind const kind,
                            std::uint64_t /*generation*/) noexcept -> motif::db::fault::fault_action
{
    if (where == g_fault_point && kind == motif::db::fault::artifact_kind::chain) {
        ++g_fault_hits;
        return motif::db::fault::fault_action::fail_io;
    }
    return motif::db::fault::fault_action::proceed;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("a failure at any boundary before COMMIT leaves the old snapshot serving and removes the attempt's files",
          "[motif-db][pr6a][composite][publication]")
{
    for (auto const point : {motif::db::fault::point::before_artifact_rename,
                             motif::db::fault::point::after_artifact_rename,
                             motif::db::fault::point::before_artifact_directory_fsync,
                             motif::db::fault::point::after_artifact_directory_fsync,
                             motif::db::fault::point::before_snapshot_begin,
                             motif::db::fault::point::after_snapshot_row_write,
                             motif::db::fault::point::before_sqlite_commit})
    {
        constexpr auto seed = 571U;
        constexpr auto random_games = std::size_t {8};
        harness env {seed, random_games, /*tree=*/true};
        auto const files_before = list_bundle(env.bundle);
        auto const json_before = env.manager->manifest().payload_json;
        auto const generation_before = env.manager->manifest().publication_generation;
        auto const before_corpus = env.corpus;

        // Emulate the canonical half of an edit, then publish with a failure injected.
        auto after = env.corpus[1];
        after.result = after.result == "1-0" ? "0-1" : "1-0";
        auto const from = env.sequence();
        {
            auto const connection = sql_connection {env.bundle / "games.db"};
            connection.exec("UPDATE game SET result = '" + after.result + "' WHERE id = 2;");
            connection.exec("UPDATE mutation_state SET mutation_seq = mutation_seq + 1 WHERE id = 1;");
        }
        auto payload = env.manager->manifest().payload;
        auto [metadata_staged, metadata_descriptor] = stage_one_run(env, from, from + 1, derived_artifact_family::postings_metadata);
        auto [tree_staged, tree_descriptor] = stage_one_run(env, from, from + 1, derived_artifact_family::tree_patch);
        payload.metadata.runs.push_back(
            patch_run_descriptor {.artifact = metadata_descriptor, .from_exclusive_seq = from, .to_inclusive_seq = from + 1});
        auto& tree_payload = motif_test::require_value(payload.tree);
        tree_payload.patch_runs.push_back(
            patch_run_descriptor {.artifact = tree_descriptor, .from_exclusive_seq = from, .to_inclusive_seq = from + 1});
        tree_payload.covered_mutation_seq = from + 1;

        g_fault_point = point;
        g_fault_hits = 0;
        {
            motif::db::fault::scoped_hook const hook {&fail_at_selected_point};
            auto const published = env.publish({.payload = payload, .staged = {metadata_staged, tree_staged}});
            CHECK_FALSE(published.has_value());
        }
        CHECK(g_fault_hits == 1);
        // The old snapshot row, in memory and in SQLite, is intact...
        CHECK(env.manager->manifest().payload_json == json_before);
        CHECK(env.manager->manifest().publication_generation == generation_before);
        // ...and this attempt's staged and renamed files are gone (the canonical
        // edit stays: the snapshot is simply stale, exactly as after any PR 5 edit).
        CHECK(list_bundle(env.bundle) == files_before);
        auto const status = env.manager->derived_index_status();
        REQUIRE(status.has_value());
        CHECK_FALSE(status->postings_usable);
        // A reopen finds the old snapshot stale and rebuilds from canonical SQLite.
        env.reopen();
        CHECK(env.manager->derived_index_status()->postings_usable);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 assertion macros inflate this linear test.
TEST_CASE("an ambiguous COMMIT reconciles to the true outcome for a chain publication", "[motif-db][pr6a][composite][publication]")
{
    struct outcome_case
    {
        motif::db::fault::fault_action action;
        bool published;
    };

    static auto selected = motif::db::fault::fault_action::commit_error_after_commit;
    auto const hook = +[](motif::db::fault::point const where,
                          motif::db::fault::artifact_kind const kind,
                          std::uint64_t /*generation*/) noexcept -> motif::db::fault::fault_action
    {
        return where == motif::db::fault::point::before_sqlite_commit && kind == motif::db::fault::artifact_kind::chain
            ? selected
            : motif::db::fault::fault_action::proceed;
    };
    for (auto const [action, expect_published] : {std::pair {motif::db::fault::fault_action::commit_error_after_commit, true},
                                                  std::pair {motif::db::fault::fault_action::commit_error_after_rollback, false}})
    {
        constexpr auto seed = 581U;
        constexpr auto random_games = std::size_t {8};
        harness env {seed, random_games, /*tree=*/false};
        auto const from = env.sequence();
        {
            auto const connection = sql_connection {env.bundle / "games.db"};
            connection.exec("UPDATE game SET result = '0-1' WHERE id = 1;");
            connection.exec("UPDATE mutation_state SET mutation_seq = mutation_seq + 1 WHERE id = 1;");
        }
        auto payload = env.manager->manifest().payload;
        auto [staged, descriptor] = stage_one_run(env, from, from + 1, derived_artifact_family::postings_metadata);
        payload.metadata.runs.push_back(
            patch_run_descriptor {.artifact = descriptor, .from_exclusive_seq = from, .to_inclusive_seq = from + 1});
        selected = action;
        motif::db::fault::scoped_hook const scoped {hook};
        auto const published = env.publish({.payload = payload, .staged = {staged}});
        CHECK(published.has_value() == expect_published);
        if (expect_published) {
            CHECK(env.manager->derived_index_status()->postings_usable);
            CHECK(std::filesystem::exists(env.bundle / descriptor.filename));
        } else {
            CHECK_FALSE(env.manager->derived_index_status()->postings_usable);
            CHECK_FALSE(std::filesystem::exists(env.bundle / descriptor.filename));  // known-old: the attempt's file is removed
        }
    }
}

TEST_CASE("the postings child-frequency mode resolves every child count from the postings", "[motif-db][pr6a][composite]")
{
    constexpr auto seed = std::uint64_t {591};
    constexpr auto random_games = std::size_t {12};
    harness env {seed, random_games, /*tree=*/true};
    auto rng = std::mt19937_64 {seed};  // NOLINT(cert-msc51-cpp,cert-msc32-c) -- a fixed seed keeps the fixtures reproducible.
    for (auto step = 0; step < 3; ++step) {
        auto const index = std::uniform_int_distribution<std::size_t> {0, env.corpus.size() - 1}(rng);
        env.edit(index, random_edit(rng, env.corpus[index]));
    }
    // Switch the descriptor to postings mode (valid for any chain; required once a
    // chain carries membership deltas). The cached checkpoint counts stay in the file
    // but are no longer trusted, so every answer must come through the resolver.
    auto payload = env.manager->manifest().payload;
    motif_test::require_value(payload.tree).child_frequency_mode = tree_child_frequency_mode::postings;
    REQUIRE(env.publish({.payload = payload, .staged = {}}).has_value());
    CHECK(env.manager->manifest().tree()->child_frequency_mode == tree_child_frequency_mode::postings);
    require_equivalent_to_fresh(env, seed + 1U);
    env.reopen();
    CHECK(env.manager->manifest().tree()->child_frequency_mode == tree_child_frequency_mode::postings);
    require_equivalent_to_fresh(env, seed + 2U);
}

TEST_CASE("rolling back a created bundle removes the chain artifacts it reserved and nothing else", "[motif-db][pr6a][composite][gc]")
{
    constexpr auto seed = 601U;
    auto env = patched_bundle_with_runs(seed);
    auto const keep = env->bundle / "positions.metadata.keepme";
    {
        std::ofstream out {keep};
        out << "mine";
    }
    auto const live = referenced_artifact_filenames(env->manager->manifest().payload);
    REQUIRE_FALSE(live.empty());
    env->manager->discard_created_bundle();
    for (auto const& name : live) {
        CHECK_FALSE(std::filesystem::exists(env->bundle / name));
    }
    CHECK_FALSE(std::filesystem::exists(env->bundle / "games.db"));
    CHECK(std::filesystem::exists(keep));  // a look-alike the manager never created survives
}
