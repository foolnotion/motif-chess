#include <optional>

#include "motif/engine/engine_manager.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/engine/error.hpp"

TEST_CASE("engine_manager: start_analysis reports engine not configured in phase 2 stub", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const result = manager.start_analysis(motif::engine::analysis_params {
        .fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        .engine = "",
        .multipv = 1,
        .depth = 20,
        .movetime_ms = std::nullopt,
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::engine_not_configured);
}

TEST_CASE("engine_manager: stop_analysis reports analysis not found in phase 2 stub", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const result = manager.stop_analysis("unknown-analysis");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::analysis_not_found);
}

TEST_CASE("engine_manager: subscribe reports analysis not found in phase 2 stub", "[motif-engine]")
{
    motif::engine::engine_manager manager;

    auto const result = manager.subscribe(
        "unknown-analysis",
        [](motif::engine::info_event const&) -> void {},
        [](motif::engine::complete_event const&) -> void {},
        [](motif::engine::error_event const&) -> void {});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == motif::engine::error_code::analysis_not_found);
}
