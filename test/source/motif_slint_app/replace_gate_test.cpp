#include <string>
#include <vector>

#include "motif/slint_app/replace_gate.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/slint_app/board_presenter.hpp"

namespace
{

using motif::slint_app::replace_gate;

}  // namespace

TEST_CASE("replace_gate: an action that discards nothing runs at once", "[motif-slint-app]")
{
    auto gate = replace_gate {};
    auto log = std::vector<std::string> {};

    auto const outcome = gate.request(/*would_discard=*/false, [&log]() -> void { log.emplace_back("ran"); });

    CHECK(outcome == replace_gate::outcome::ran);
    CHECK(log == std::vector<std::string> {"ran"});
    CHECK_FALSE(gate.pending());
}

TEST_CASE("replace_gate: a discarding action waits for confirmation and runs once on accept", "[motif-slint-app]")
{
    auto gate = replace_gate {};
    auto runs = 0;

    auto const outcome = gate.request(/*would_discard=*/true, [&runs]() -> void { ++runs; });
    CHECK(outcome == replace_gate::outcome::deferred);
    CHECK(runs == 0);
    CHECK(gate.pending());

    gate.accept();
    CHECK(runs == 1);
    CHECK_FALSE(gate.pending());

    gate.accept();
    CHECK(runs == 1);
}

TEST_CASE("replace_gate: cancel keeps everything and a newer request replaces a waiting one", "[motif-slint-app]")
{
    auto gate = replace_gate {};
    auto log = std::vector<std::string> {};

    gate.request(/*would_discard=*/true, [&log]() -> void { log.emplace_back("first"); });
    gate.cancel();
    gate.accept();
    CHECK(log.empty());

    gate.request(/*would_discard=*/true, [&log]() -> void { log.emplace_back("first"); });
    gate.request(/*would_discard=*/true, [&log]() -> void { log.emplace_back("second"); });
    gate.accept();
    CHECK(log == std::vector<std::string> {"second"});
}

// Catch2 assertion macros inflate this test's measured cognitive complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("replace_gate: replacing an unsaved or edited game asks first, replacing an untouched one does not", "[motif-slint-app]")
{
    auto presenter = motif::slint_app::board_presenter {};
    auto gate = replace_gate {};
    auto replaced = 0;
    auto const replace = [&]() -> replace_gate::outcome
    { return gate.request(presenter.state().edited, [&replaced]() -> void { ++replaced; }); };

    // Nothing on the board, then a fresh New Game: nothing to lose.
    CHECK(replace() == replace_gate::outcome::ran);
    presenter.start_new_game();
    CHECK(replace() == replace_gate::outcome::ran);
    CHECK(replaced == 2);

    // One move makes the unsaved game worth asking about.
    REQUIRE(presenter.play_san("e4").has_value());
    CHECK(replace() == replace_gate::outcome::deferred);
    CHECK(replaced == 2);
    gate.cancel();
    CHECK(presenter.state().total_plies == 1);

    CHECK(replace() == replace_gate::outcome::deferred);
    gate.accept();
    CHECK(replaced == 3);
}
