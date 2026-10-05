#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "motif/slint_app/file_picker.hpp"

namespace motif::slint_app
{

namespace
{

constexpr auto is_space(char const character) -> bool
{
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

auto trimmed(std::string_view value) -> std::string_view
{
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

auto names_directory(std::filesystem::path const& path) -> bool
{
    auto error = std::error_code {};
    return std::filesystem::is_directory(path, error);
}

}  // namespace

auto picker_outcome::chosen(std::string path) -> picker_outcome
{
    return {.result = picker_result::chosen, .path = std::move(path), .message = {}};
}

auto picker_outcome::cancelled() -> picker_outcome
{
    return {.result = picker_result::cancelled, .path = {}, .message = {}};
}

auto picker_outcome::failed(std::string message) -> picker_outcome
{
    return {.result = picker_result::failed, .path = {}, .message = std::move(message)};
}

auto picker_start_path(std::string_view const field_text) -> std::string
{
    auto const text = trimmed(field_text);
    if (text.empty()) {
        return {};
    }
    auto path = std::filesystem::path {text};
    // "/a/b/" has an empty file name: it names the directory "/a/b".
    if (!path.has_filename()) {
        path = path.parent_path();
    }
    // A directory seeds itself; a file or a path that does not exist seeds its
    // nearest ancestor that is one.
    while (!path.empty() && !names_directory(path)) {
        auto const parent = path.parent_path();
        if (parent == path) {
            return {};
        }
        path = parent;
    }
    return path.string();
}

// ---------------------------------------------------------------------------
// scripted_file_picker
// ---------------------------------------------------------------------------

scripted_file_picker::scripted_file_picker(script behaviour)
    : script_ {std::move(behaviour)}
{
}

auto scripted_file_picker::thread() const noexcept -> picker_thread
{
    return script_.thread;
}

auto scripted_file_picker::abortable() const noexcept -> bool
{
    return script_.abortable;
}

auto scripted_file_picker::pick(picker_request const& request) -> picker_outcome
{
    auto lock = std::unique_lock {mutex_};
    requests_.push_back(request);
    last_thread_ = std::this_thread::get_id();
    changed_.notify_all();

    auto outcome = picker_outcome::cancelled();
    switch (script_.behaviour) {
        case mode::choose:
            outcome = picker_outcome::chosen(script_.text);
            break;
        case mode::cancel:
            break;
        case mode::fail:
            outcome = picker_outcome::failed(script_.text);
            break;
        case mode::hang:
            changed_.wait(lock, [this]() -> bool { return finished_.has_value() || (aborted_ && script_.abortable); });
            if (finished_) {
                outcome = std::move(*finished_);
                finished_.reset();
            }
            break;
    }
    ++returned_;
    changed_.notify_all();
    return outcome;
}

void scripted_file_picker::abort() noexcept
{
    std::scoped_lock const lock {mutex_};
    aborted_ = true;
    changed_.notify_all();
}

void scripted_file_picker::finish(picker_outcome outcome)
{
    std::scoped_lock const lock {mutex_};
    finished_ = std::move(outcome);
    changed_.notify_all();
}

auto scripted_file_picker::requests() const -> std::vector<picker_request>
{
    std::scoped_lock const lock {mutex_};
    return requests_;
}

auto scripted_file_picker::last_thread() const -> std::thread::id
{
    std::scoped_lock const lock {mutex_};
    return last_thread_;
}

auto scripted_file_picker::wait_for_calls(std::size_t const count, std::chrono::milliseconds const timeout) const -> bool
{
    auto lock = std::unique_lock {mutex_};
    return changed_.wait_for(lock, timeout, [this, count]() -> bool { return requests_.size() >= count; });
}

auto scripted_file_picker::wait_for_return(std::chrono::milliseconds const timeout) const -> bool
{
    auto lock = std::unique_lock {mutex_};
    return changed_.wait_for(lock, timeout, [this]() -> bool { return returned_ > 0; });
}

auto make_scripted_picker(std::string_view const spec) -> std::shared_ptr<file_picker>
{
    auto const with_prefix = [spec](std::string_view const prefix) -> std::optional<std::string>
    {
        if (!spec.starts_with(prefix) || spec.size() == prefix.size()) {
            return std::nullopt;
        }
        return std::string {spec.substr(prefix.size())};
    };
    if (auto const path = with_prefix("choose:")) {
        return std::make_shared<scripted_file_picker>(
            scripted_file_picker::script {.behaviour = scripted_file_picker::mode::choose, .text = *path});
    }
    if (auto const message = with_prefix("fail:")) {
        return std::make_shared<scripted_file_picker>(
            scripted_file_picker::script {.behaviour = scripted_file_picker::mode::fail, .text = *message});
    }
    if (spec == "cancel") {
        return std::make_shared<scripted_file_picker>(scripted_file_picker::script {.behaviour = scripted_file_picker::mode::cancel});
    }
    if (spec == "hang") {
        return std::make_shared<scripted_file_picker>(
            scripted_file_picker::script {.behaviour = scripted_file_picker::mode::hang, .abortable = false});
    }
    return nullptr;
}

}  // namespace motif::slint_app
