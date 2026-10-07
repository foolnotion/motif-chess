#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "motif/slint_app/database_host.hpp"

#include <slint.h>

#include "motif/slint_app/activity_gate.hpp"
#include "motif/slint_app/replace_controller.hpp"
#include "workspace.h"

namespace motif::slint_app
{

namespace
{

auto shared_string(std::string_view value) -> slint::SharedString
{
    return slint::SharedString {value};
}

auto text_of(slint::SharedString const& value) -> std::string_view
{
    return trim_whitespace(std::string_view {value.data(), value.size()});
}

constexpr auto busy_notice = "Nothing was changed: the workspace became busy while the confirmation was open. Try again when it is idle.";

// The gate is decided again when the user accepts, because an import, load or
// search can start while the dialog is open. A request that lost the race is
// dropped and the error bar says so; the dialog is already closed by then.
auto when_still_idle(WorkspaceWindow& window, activity_probe activity, std::function<void()> action) -> std::function<void()>
{
    return [&window, activity = std::move(activity), action = std::move(action)]() -> void
    {
        if (!run_when_idle(activity(), action)) {
            window.set_error_text(busy_notice);
        }
    };
}

// The gate as decided when the user asks. This is the one request-time
// rejection path of the five database callbacks: a busy workspace runs nothing,
// keeps nothing queued, and the error bar says why.
template<typename Request>
void request_when_idle(WorkspaceWindow& window, activity_probe const& activity, Request&& request)
{
    if (!run_when_idle(activity(), std::forward<Request>(request))) {
        window.set_error_text(shared_string(request_busy_notice));
    }
}

}  // namespace

auto trim_whitespace(std::string_view const value) -> std::string_view
{
    constexpr auto whitespace = " \t\n\r\f\v";
    auto const begin = value.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    return value.substr(begin, value.find_last_not_of(whitespace) - begin + 1);
}

void register_database_callbacks(WorkspaceWindow& window,
                                 activity_probe const& activity,
                                 replace_controller& confirmation,
                                 database_actions actions)
{
    auto const shared = std::make_shared<database_actions>(std::move(actions));
    window.on_create_requested(
        [&window, &confirmation, activity, shared](slint::SharedString const& dir_path, slint::SharedString const& name) -> void
        {
            request_when_idle(window,
                              activity,
                              [&]() -> void
                              {
                                  confirmation.request("Create database",
                                                       when_still_idle(window,
                                                                       activity,
                                                                       [&window, shared, dir_path, name]() -> void
                                                                       {
                                                                           window.set_error_text("");
                                                                           auto const trimmed_path = std::string {text_of(dir_path)};
                                                                           auto const trimmed_name = std::string {text_of(name)};
                                                                           window.set_create_path_text(shared_string(trimmed_path));
                                                                           window.set_create_name_text(shared_string(trimmed_name));
                                                                           if (shared->create(trimmed_path, trimmed_name)) {
                                                                               window.set_busy(true);
                                                                           }
                                                                       }));
                              });
        });
    window.on_open_requested(
        [&window, &confirmation, activity, shared](slint::SharedString const& dir_path) -> void
        {
            request_when_idle(window,
                              activity,
                              [&]() -> void
                              {
                                  confirmation.request("Open database",
                                                       when_still_idle(window,
                                                                       activity,
                                                                       [&window, shared, dir_path]() -> void
                                                                       {
                                                                           window.set_error_text("");
                                                                           auto const trimmed_path = std::string {text_of(dir_path)};
                                                                           window.set_open_path_text(shared_string(trimmed_path));
                                                                           if (shared->open(trimmed_path)) {
                                                                               window.set_busy(true);
                                                                           }
                                                                       }));
                              });
        });
    window.on_recent_activated(
        [&window, &confirmation, activity, shared](slint::SharedString const& path) -> void
        {
            request_when_idle(window,
                              activity,
                              [&]() -> void
                              {
                                  confirmation.request("Open database",
                                                       when_still_idle(window,
                                                                       activity,
                                                                       [&window, shared, path]() -> void
                                                                       {
                                                                           window.set_error_text("");
                                                                           if (shared->open(std::string {text_of(path)})) {
                                                                               window.set_busy(true);
                                                                           }
                                                                       }));
                              });
        });
    window.on_scratch_requested(
        [&window, &confirmation, activity, shared]() -> void
        {
            request_when_idle(window,
                              activity,
                              [&]() -> void
                              {
                                  confirmation.request("Open scratch database",
                                                       when_still_idle(window,
                                                                       activity,
                                                                       [&window, shared]() -> void
                                                                       {
                                                                           window.set_error_text("");
                                                                           if (shared->scratch()) {
                                                                               window.set_busy(true);
                                                                           }
                                                                       }));
                              });
        });
    window.on_close_requested(
        [&window, &confirmation, activity, shared]() -> void
        {
            request_when_idle(
                window,
                activity,
                [&]() -> void
                { confirmation.request("Close database", when_still_idle(window, activity, [shared]() -> void { shared->close(); })); });
        });
}

}  // namespace motif::slint_app
