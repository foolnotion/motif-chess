#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "motif/slint_app/file_picker.hpp"

#include <catch2/catch_test_macros.hpp>

#include "fake_uci_engine.hpp"

// Test values are plain numbers and short literals.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
namespace
{

using motif::slint_app::file_picker;
using motif::slint_app::make_scripted_picker;
using motif::slint_app::picker_kind;
using motif::slint_app::picker_outcome;
using motif::slint_app::picker_request;
using motif::slint_app::picker_result;
using motif::slint_app::picker_start_path;
using motif::slint_app::picker_thread;
using motif::slint_app::scripted_file_picker;

constexpr auto patience = std::chrono::seconds {10};
constexpr auto brief = std::chrono::milliseconds {50};

struct temp_tree
{
    std::filesystem::path root {std::filesystem::temp_directory_path() / ("motif_file_picker_" + fake_uci::unique_stamp())};

    temp_tree()
    {
        std::filesystem::create_directories(root / "games" / "bundle");
        std::ofstream {root / "games" / "master.pgn"} << "1. e4 *\n";
    }

    ~temp_tree()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    temp_tree(temp_tree const&) = delete;
    auto operator=(temp_tree const&) -> temp_tree& = delete;
    temp_tree(temp_tree&&) = delete;
    auto operator=(temp_tree&&) -> temp_tree& = delete;
};

auto scripted(scripted_file_picker::mode const behaviour, std::string text = {}, bool const abortable = true)
    -> std::shared_ptr<scripted_file_picker>
{
    return std::make_shared<scripted_file_picker>(
        scripted_file_picker::script {.behaviour = behaviour, .text = std::move(text), .abortable = abortable});
}

}  // namespace

TEST_CASE("picker start path: an existing directory opens itself, with or without a trailing slash", "[motif-slint-app][picker]")
{
    auto const tree = temp_tree {};
    auto const bundle = (tree.root / "games" / "bundle").string();
    CHECK(picker_start_path(bundle) == bundle);
    CHECK(picker_start_path(bundle + "/") == bundle);
    CHECK(picker_start_path("  " + bundle + "  ") == bundle);
}

TEST_CASE("picker start path: a file opens its folder", "[motif-slint-app][picker]")
{
    auto const tree = temp_tree {};
    CHECK(picker_start_path((tree.root / "games" / "master.pgn").string()) == (tree.root / "games").string());
}

TEST_CASE("picker start path: a path that does not exist opens its nearest existing ancestor", "[motif-slint-app][picker]")
{
    auto const tree = temp_tree {};
    CHECK(picker_start_path((tree.root / "games" / "bundle" / "new" / "deeper").string()) == (tree.root / "games" / "bundle").string());
    CHECK(picker_start_path((tree.root / "games" / "missing.pgn").string()) == (tree.root / "games").string());
    CHECK(picker_start_path((tree.root / "games" / "missing.pgn").string() + "/") == (tree.root / "games").string());
}

TEST_CASE("picker start path: nothing to seed from gives the system's own default", "[motif-slint-app][picker]")
{
    CHECK(picker_start_path("").empty());
    CHECK(picker_start_path("   ").empty());
    CHECK(picker_start_path("no-such-relative-path-" + fake_uci::unique_stamp()).empty());
}

TEST_CASE("outcome constructors set the result and the matching text", "[motif-slint-app][picker]")
{
    auto const chosen = picker_outcome::chosen("/a/b");
    CHECK(chosen.result == picker_result::chosen);
    CHECK(chosen.path == "/a/b");
    CHECK(chosen.message.empty());

    auto const cancelled = picker_outcome::cancelled();
    CHECK(cancelled.result == picker_result::cancelled);
    CHECK(cancelled.path.empty());

    auto const failed = picker_outcome::failed("no portal");
    CHECK(failed.result == picker_result::failed);
    CHECK(failed.message == "no portal");
    CHECK(failed.path.empty());
}

TEST_CASE("scripted picker: choose, cancel and fail answer at once and record the request", "[motif-slint-app][picker]")
{
    auto const request =
        picker_request {.kind = picker_kind::file, .start_path = "/data", .filters = {{.name = "PGN files", .spec = "pgn"}}};

    auto const choose = scripted(scripted_file_picker::mode::choose, "/data/a.pgn");
    CHECK(choose->pick(request) == picker_outcome::chosen("/data/a.pgn"));

    auto const cancel = scripted(scripted_file_picker::mode::cancel);
    CHECK(cancel->pick(request) == picker_outcome::cancelled());

    auto const fail = scripted(scripted_file_picker::mode::fail, "no portal");
    CHECK(fail->pick(request) == picker_outcome::failed("no portal"));

    CHECK(choose->requests() == std::vector<picker_request> {request});
    CHECK(choose->last_thread() == std::this_thread::get_id());
    CHECK(choose->wait_for_calls(1, brief));
    CHECK_FALSE(choose->wait_for_calls(2, brief));
}

TEST_CASE("scripted picker: an abortable hang ends on abort with a cancel", "[motif-slint-app][picker]")
{
    auto const picker = scripted(scripted_file_picker::mode::hang);
    REQUIRE(picker->abortable());

    auto answer = picker_outcome::failed("not set");
    auto worker = std::thread {[&]() -> void { answer = picker->pick(picker_request {}); }};
    REQUIRE(picker->wait_for_calls(1, patience));
    CHECK_FALSE(picker->wait_for_return(brief));

    auto const worker_id = worker.get_id();
    picker->abort();
    worker.join();
    CHECK(answer == picker_outcome::cancelled());
    CHECK(picker->wait_for_return(patience));
    CHECK(picker->last_thread() == worker_id);
}

TEST_CASE("scripted picker: an unabortable hang ignores abort and ends on finish", "[motif-slint-app][picker]")
{
    auto const picker = scripted(scripted_file_picker::mode::hang, {}, /*abortable=*/false);
    REQUIRE_FALSE(picker->abortable());

    auto answer = picker_outcome::failed("not set");
    auto worker = std::thread {[&]() -> void { answer = picker->pick(picker_request {}); }};
    REQUIRE(picker->wait_for_calls(1, patience));

    picker->abort();
    CHECK_FALSE(picker->wait_for_return(brief));

    picker->finish(picker_outcome::chosen("/late"));
    worker.join();
    CHECK(answer == picker_outcome::chosen("/late"));
}

TEST_CASE("scripted picker: the thread policy is the script's", "[motif-slint-app][picker]")
{
    CHECK(scripted_file_picker {{}}.thread() == picker_thread::worker);
    CHECK(scripted_file_picker {{.thread = picker_thread::event_loop}}.thread() == picker_thread::event_loop);
}

TEST_CASE("MOTIF_FILE_PICKER values name the scripted pickers", "[motif-slint-app][picker]")
{
    auto const request = picker_request {};

    auto const choose = make_scripted_picker("choose:/home/me/my games/a.pgn");
    REQUIRE(choose != nullptr);
    CHECK(choose->pick(request) == picker_outcome::chosen("/home/me/my games/a.pgn"));

    auto const colon = make_scripted_picker("choose:C:/games");
    REQUIRE(colon != nullptr);
    CHECK(colon->pick(request) == picker_outcome::chosen("C:/games"));

    auto const cancel = make_scripted_picker("cancel");
    REQUIRE(cancel != nullptr);
    CHECK(cancel->pick(request) == picker_outcome::cancelled());

    auto const fail = make_scripted_picker("fail:The name org.freedesktop.portal.Desktop was not provided");
    REQUIRE(fail != nullptr);
    CHECK(fail->pick(request) == picker_outcome::failed("The name org.freedesktop.portal.Desktop was not provided"));

    auto const hang = make_scripted_picker("hang");
    REQUIRE(hang != nullptr);
    CHECK_FALSE(hang->abortable());
    CHECK(hang->thread() == picker_thread::worker);
}

TEST_CASE("MOTIF_FILE_PICKER: anything else is not a scripted picker", "[motif-slint-app][picker]")
{
    CHECK(make_scripted_picker("") == nullptr);
    CHECK(make_scripted_picker("choose") == nullptr);
    CHECK(make_scripted_picker("choose:") == nullptr);
    CHECK(make_scripted_picker("fail") == nullptr);
    CHECK(make_scripted_picker("fail:") == nullptr);
    CHECK(make_scripted_picker("cancel:now") == nullptr);
    CHECK(make_scripted_picker("hang:1") == nullptr);
    CHECK(make_scripted_picker("zenity") == nullptr);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
