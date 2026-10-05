#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/slint_app/nfd_file_picker.hpp"

#include <fmt/base.h>
#include <nfd.h>

namespace motif::slint_app
{

namespace
{

// NFD's error text is a process-wide global: read it on the thread that made the
// call, copy it, and clear it so the next dialog starts clean.
auto take_error(std::string_view const fallback) -> std::string
{
    auto const* const text = NFD_GetError();
    auto message = text != nullptr ? std::string {text} : std::string {};
    NFD_ClearError();
    if (message.empty()) {
        message = fallback;
    }
    return message;
}

// NFD_Init and NFD_Quit belong on the thread that shows the dialog (COM on
// Windows needs it). One session per dialog also gives fresh error state, and
// lets a portal that appears later work without restarting the program.
class nfd_session
{
  public:
    nfd_session()
        : result_ {NFD_Init()}
    {
    }

    ~nfd_session()
    {
        // Whatever path the dialog took, leave no error text behind in NFD's global.
        NFD_ClearError();
        if (ok()) {
            NFD_Quit();
        }
    }

    nfd_session(nfd_session const&) = delete;
    auto operator=(nfd_session const&) -> nfd_session& = delete;
    nfd_session(nfd_session&&) = delete;
    auto operator=(nfd_session&&) -> nfd_session& = delete;

    [[nodiscard]] auto ok() const -> bool { return result_ == NFD_OKAY; }

  private:
    nfdresult_t result_;
};

auto folder_dialog(nfdu8char_t** const path, picker_request const& request) -> nfdresult_t
{
    auto arguments = nfdpickfolderu8args_t {};
    arguments.defaultPath = request.start_path.empty() ? nullptr : request.start_path.c_str();
    return NFD_PickFolderU8_With(path, &arguments);
}

auto file_dialog(nfdu8char_t** const path, picker_request const& request) -> nfdresult_t
{
    auto filters = std::vector<nfdu8filteritem_t> {};
    filters.reserve(request.filters.size());
    for (auto const& filter : request.filters) {
        filters.push_back(nfdu8filteritem_t {.name = filter.name.c_str(), .spec = filter.spec.c_str()});
    }
    auto arguments = nfdopendialogu8args_t {};
    arguments.filterList = filters.empty() ? nullptr : filters.data();
    arguments.filterCount = static_cast<nfdfiltersize_t>(filters.size());
    arguments.defaultPath = request.start_path.empty() ? nullptr : request.start_path.c_str();
    return NFD_OpenDialogU8_With(path, &arguments);
}

}  // namespace

auto nfd_file_picker::thread() const noexcept -> picker_thread
{
#ifdef __APPLE__
    return picker_thread::event_loop;
#else
    return picker_thread::worker;
#endif
}

auto nfd_file_picker::abortable() const noexcept -> bool
{
    return false;
}

auto nfd_file_picker::pick(picker_request const& request) -> picker_outcome
{
    auto const session = nfd_session {};
    if (!session.ok()) {
        return picker_outcome::failed(take_error("The file dialog could not be initialised"));
    }

    nfdu8char_t* path = nullptr;
    auto const result = request.kind == picker_kind::folder ? folder_dialog(&path, request) : file_dialog(&path, request);
    switch (result) {
        case NFD_OKAY: {
            auto chosen = path != nullptr ? std::string {path} : std::string {};
            NFD_FreePathU8(path);
            // NFD answers OKAY with a path; an empty one is a cancel, never a field value.
            return chosen.empty() ? picker_outcome::cancelled() : picker_outcome::chosen(std::move(chosen));
        }
        case NFD_CANCEL:
            return picker_outcome::cancelled();
        case NFD_ERROR:
            break;
    }
    return picker_outcome_for_nfd_error(take_error("The file dialog reported an error"));
}

auto picker_outcome_for_nfd_error(std::string message) -> picker_outcome
{
    constexpr auto prefix = std::string_view {"D-Bus file dialog interaction was ended abruptly with response code "};
    // The portal's "ended in some other way", with or without NFD's full stop.
    auto const text = std::string_view {message};
    if (text.starts_with(prefix)) {
        auto const code = text.substr(prefix.size());
        if (code == "2" || code == "2.") {
            return picker_outcome::cancelled();
        }
    }
    return picker_outcome::failed(std::move(message));
}

auto make_default_file_picker() -> std::shared_ptr<file_picker>
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once at startup, before any thread exists.
    if (char const* const value = std::getenv("MOTIF_FILE_PICKER"); value != nullptr && *value != '\0') {
        if (auto scripted = make_scripted_picker(value)) {
            return scripted;
        }
        fmt::print(stderr,
                   "warning: MOTIF_FILE_PICKER={} is not choose:<path>, cancel, fail:<message> or hang; using the system file dialog\n",
                   value);
    }
    return std::make_shared<nfd_file_picker>();
}

}  // namespace motif::slint_app
