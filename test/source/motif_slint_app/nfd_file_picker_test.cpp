#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include "motif/slint_app/nfd_file_picker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "motif/slint_app/file_picker.hpp"

// The real adapter without a display or a portal. A dialog needs a desktop
// session, so the positive path is the manual check in the story's evidence;
// what runs here is the failure path: no session bus, so NFD cannot connect and
// reports it. libdbus does not autolaunch a bus for an address that names a
// socket that does not exist.
//
// This is its own executable: it changes the process environment, and it links
// NFD, which the service test binary never does.
namespace
{

using motif::slint_app::make_default_file_picker;
using motif::slint_app::nfd_file_picker;
using motif::slint_app::picker_kind;
using motif::slint_app::picker_outcome;
using motif::slint_app::picker_outcome_for_nfd_error;
using motif::slint_app::picker_request;
using motif::slint_app::picker_result;
using motif::slint_app::picker_thread;

constexpr auto bound = std::chrono::seconds {10};

// Points the session bus at a socket that does not exist, and restores it.
class dead_session_bus
{
  public:
    dead_session_bus()
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): the test binary is single-threaded here.
        ::setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/motif-test-bus", 1);
    }

    ~dead_session_bus()
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): the test binary is single-threaded here.
        ::unsetenv("DBUS_SESSION_BUS_ADDRESS");
    }

    dead_session_bus(dead_session_bus const&) = delete;
    auto operator=(dead_session_bus const&) -> dead_session_bus& = delete;
    dead_session_bus(dead_session_bus&&) = delete;
    auto operator=(dead_session_bus&&) -> dead_session_bus& = delete;
};

}  // namespace

TEST_CASE("nfd adapter: with no session bus a dialog fails with NFD's reason", "[motif-slint-app][picker][nfd]")
{
    auto const bus = dead_session_bus {};
    auto picker = nfd_file_picker {};

    auto const started = std::chrono::steady_clock::now();
    auto const folder = picker.pick(picker_request {.kind = picker_kind::folder, .start_path = "/tmp"});
    CHECK(std::chrono::steady_clock::now() - started < bound);

    CHECK(folder.result == picker_result::failed);
    CHECK_FALSE(folder.message.empty());
    CHECK(folder.path.empty());
}

TEST_CASE("nfd adapter: a failed dialog leaves nothing behind, so the next one fails the same way", "[motif-slint-app][picker][nfd]")
{
    auto const bus = dead_session_bus {};
    auto picker = nfd_file_picker {};

    auto const first = picker.pick(picker_request {.kind = picker_kind::file, .filters = {{.name = "PGN files", .spec = "pgn"}}});
    auto const second = picker.pick(picker_request {.kind = picker_kind::file, .filters = {{.name = "PGN files", .spec = "pgn"}}});

    CHECK(first.result == picker_result::failed);
    CHECK(second == first);
}

TEST_CASE("nfd adapter: a portal dialog closed with Escape or the window button is a cancel, not a failure",
          "[motif-slint-app][picker][nfd]")
{
    // The text NFD 1.3.0 sets for portal response code 2, seen live from the GTK backend on Escape.
    auto const closed = std::string {"D-Bus file dialog interaction was ended abruptly with response code 2."};
    CHECK(picker_outcome_for_nfd_error(closed) == picker_outcome::cancelled());
    CHECK(picker_outcome_for_nfd_error("D-Bus file dialog interaction was ended abruptly with response code 2")
          == picker_outcome::cancelled());

    // Other codes and every other error stay failures with their text.
    auto const other = std::string {"D-Bus file dialog interaction was ended abruptly with response code 3."};
    CHECK(picker_outcome_for_nfd_error(other) == picker_outcome::failed(other));
    auto const code_twelve = std::string {"D-Bus file dialog interaction was ended abruptly with response code 12."};
    CHECK(picker_outcome_for_nfd_error(code_twelve) == picker_outcome::failed(code_twelve));
    CHECK(picker_outcome_for_nfd_error("The name org.freedesktop.portal.Desktop was not provided")
          == picker_outcome::failed("The name org.freedesktop.portal.Desktop was not provided"));
    CHECK(picker_outcome_for_nfd_error("") == picker_outcome::failed(""));
}

TEST_CASE("nfd adapter: it cannot be aborted and runs on a worker thread off macOS", "[motif-slint-app][picker][nfd]")
{
    auto const picker = nfd_file_picker {};
    CHECK_FALSE(picker.abortable());
    CHECK(picker.thread() == picker_thread::worker);
}

TEST_CASE("MOTIF_FILE_PICKER selects the scripted picker, anything unrecognised keeps NFD", "[motif-slint-app][picker][nfd]")
{
    // NOLINTBEGIN(concurrency-mt-unsafe): the test binary is single-threaded here.
    ::setenv("MOTIF_FILE_PICKER", "choose:/scripted/path", 1);
    auto const scripted = make_default_file_picker();
    REQUIRE(scripted != nullptr);
    CHECK(dynamic_cast<nfd_file_picker*>(scripted.get()) == nullptr);
    CHECK(scripted->pick(picker_request {}) == picker_outcome::chosen("/scripted/path"));

    ::setenv("MOTIF_FILE_PICKER", "zenity", 1);
    auto const unknown = make_default_file_picker();
    REQUIRE(unknown != nullptr);
    CHECK(dynamic_cast<nfd_file_picker*>(unknown.get()) != nullptr);

    ::unsetenv("MOTIF_FILE_PICKER");
    auto const real = make_default_file_picker();
    REQUIRE(real != nullptr);
    CHECK(dynamic_cast<nfd_file_picker*>(real.get()) != nullptr);
    // NOLINTEND(concurrency-mt-unsafe)
}
