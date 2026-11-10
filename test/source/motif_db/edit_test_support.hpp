#pragma once

// Fixtures shared by the PR 6b edit tests: a second SQLite connection on the
// bundle, artifact-directory probes, and an edit bundle whose canonical model is
// kept in step with every production patch so it can be compared with a fresh
// rebuild of the edited data.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include "composite_test_support.hpp"
#include "equivalence_support.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/types.hpp"

namespace motif_test
{

using namespace motif::db;  // NOLINT(google-build-using-namespace) -- the helpers touch most of motif::db; qualifying every name buries the
                            // checks.
namespace fs = std::filesystem;

// ── Small SQLite probe on the bundle's database (a second connection) ────────

struct probe_connection
{
    sqlite3* handle {};

    explicit probe_connection(fs::path const& bundle)
    {
        REQUIRE(sqlite3_open_v2((bundle / "games.db").c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
        constexpr auto busy_timeout_ms = 5'000;
        sqlite3_busy_timeout(handle, busy_timeout_ms);
    }

    ~probe_connection() { sqlite3_close(handle); }

    probe_connection(probe_connection const&) = delete;
    auto operator=(probe_connection const&) -> probe_connection& = delete;
    probe_connection(probe_connection&&) = delete;
    auto operator=(probe_connection&&) -> probe_connection& = delete;

    void exec(std::string const& sql) const
    {
        char* message = nullptr;
        auto const status = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &message);
        INFO((message != nullptr ? std::string {message} : std::string {}));
        REQUIRE(status == SQLITE_OK);
        sqlite3_free(message);
    }

    [[nodiscard]] auto scalar(std::string const& sql) const -> std::int64_t
    {
        sqlite3_stmt* raw = nullptr;
        REQUIRE(sqlite3_prepare_v2(handle, sql.c_str(), -1, &raw, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(raw) == SQLITE_ROW);
        auto const value = sqlite3_column_int64(raw, 0);
        sqlite3_finalize(raw);
        return value;
    }
};

// ── Bundle files ─────────────────────────────────────────────────────────────

inline auto is_owned_name(std::string const& name) -> bool
{
    return name.starts_with(postings_artifact_prefix) || name.starts_with(tree_artifact_prefix)
        || name.starts_with(postings_metadata_artifact_prefix) || name.starts_with(tree_patch_artifact_prefix);
}

// Every derived-artifact file in the bundle (published, staged or leftover).
inline auto artifact_names(fs::path const& dir) -> std::set<std::string>
{
    auto names = std::set<std::string> {};
    for (auto const& entry : fs::directory_iterator {dir}) {
        auto const name = entry.path().filename().string();
        if (is_owned_name(name)) {
            names.insert(name);
        }
    }
    return names;
}

inline auto referenced_names(db_manifest const& manifest) -> std::set<std::string>
{
    auto const names = referenced_artifact_filenames(manifest.payload);
    return {names.begin(), names.end()};
}

// The bundle identity a manifest records, decoded for the artifact loaders' expectations.
inline auto bundle_identity_of(db_manifest const& manifest) -> identity_bytes
{
    auto const identity = identity_from_hex(manifest.payload.bundle.id);
    REQUIRE(identity.has_value());
    return identity.value_or(identity_bytes {});
}

inline auto file_digest(fs::path const& path) -> std::uint64_t
{
    auto input = std::ifstream {path, std::ios::binary};
    auto const bytes = std::string {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
    return fnv1a64(bytes);
}

// ── The edit fixture ─────────────────────────────────────────────────────────

inline auto elo_edit(std::optional<std::int32_t> const& before, std::optional<std::int32_t> const& after) -> rating_patch
{
    if (before == after) {
        return rating_patch {};
    }
    return after ? rating_patch::set(*after) : rating_patch::clear();
}

inline auto patch_for(model_game const& before, model_game const& after) -> game_patch
{
    auto patch = game_patch {};
    if (before.result != after.result) {
        patch.result = after.result;
    }
    patch.white_elo = elo_edit(before.white_elo, after.white_elo);
    patch.black_elo = elo_edit(before.black_elo, after.black_elo);
    return patch;
}

inline auto indexed_values_differ(model_game const& before, model_game const& after) -> bool
{
    return before.result != after.result || before.white_elo != after.white_elo || before.black_elo != after.black_elo;
}

struct edit_bundle
{
    temp_dir dir {"edit"};
    fs::path bundle;
    std::vector<model_game> corpus;
    // Null only between a deliberate reset() (a child process owns the bundle, or a reopen follows) and the next reopen().
    std::unique_ptr<database_manager> manager;
    bool with_tree;

    // Games 1, 1 + manual_stride, ... are flipped to manual; stride 0 leaves every
    // game an imported one.
    edit_bundle(std::uint64_t const seed, std::size_t const random_games, bool const tree, std::size_t const manual_stride = 3)
        : bundle {dir.path / "db"}
        , corpus {make_corpus(seed, random_games)}
        , with_tree {tree}
    {
        manager = std::make_unique<database_manager>(build_bundle(bundle, corpus, tree));
        if (manual_stride != 0) {
            for (std::size_t index = 0; index < corpus.size(); index += manual_stride) {
                REQUIRE(manager->set_manual_game_provenance(id_of(index), std::nullopt, "new").has_value());
            }
        }
    }

    [[nodiscard]] static auto id_of(std::size_t const index) -> game_id { return game_id {static_cast<std::uint32_t>(index + 1U)}; }

    [[nodiscard]] auto status() const -> derived_index_report
    {
        auto report = manager->derived_index_status();
        REQUIRE(report.has_value());
        return *report;
    }

    void reopen()
    {
        manager.reset();
        auto reopened = database_manager::open(bundle);
        REQUIRE(reopened.has_value());
        manager = std::make_unique<database_manager>(std::move(*reopened));
    }

    // One production edit; the model follows on success. Checks the sequence
    // accounting: exactly one advance per index-relevant change, none otherwise.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Catch2 macros inflate this linear edit-and-verify sequence.
    void edit(std::size_t const index, model_game const& after)
    {
        auto const before = corpus[index];
        auto const seq_before = status().mutation_seq;
        auto const generation_before = status().publication_generation;
        auto const changed = indexed_values_differ(before, after);
        auto const patched = manager->patch_game_metadata(id_of(index), patch_for(before, after));
        INFO((patched.has_value() ? std::string {} : patched.error().message));
        REQUIRE(patched.has_value());
        corpus[index] = after;
        auto const now = status();
        CHECK(now.mutation_seq == seq_before + (changed ? 1U : 0U));
        CHECK(now.publication_generation == generation_before + (changed ? 1U : 0U));
        CHECK(now.postings_usable);
        CHECK(now.tree_usable == with_tree);
        CHECK(manager->last_patch_report().published == changed);
        CHECK(manager->last_patch_report().canonical_noop == !changed);
        // The canonical row holds the edited values.
        auto const stored = manager->store().get(id_of(index));
        REQUIRE(stored.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        CHECK(stored->result == after.result);
        CHECK(stored->white.elo == after.white_elo);
        CHECK(stored->black.elo == after.black_elo);
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    // GC invariant: the directory holds exactly the files the installed snapshot
    // references.
    void require_clean_directory() const { REQUIRE(artifact_names(bundle) == referenced_names(manager->manifest())); }

    void require_equivalent(std::uint64_t const seed) const { require_equivalent_to_fresh(*manager, bundle, corpus, with_tree, seed); }
};

inline auto random_edit_of(std::mt19937_64& rng, model_game const& base) -> model_game
{
    auto after = base;
    if (rng() % 3U != 0U) {
        after.result = pick_result(rng);
    }
    if (rng() % 2U == 0U) {
        after.white_elo = pick_elo(rng);
    }
    if (rng() % 2U == 0U) {
        after.black_elo = pick_elo(rng);
    }
    return after;
}

}  // namespace motif_test
