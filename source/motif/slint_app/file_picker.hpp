#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The toolkit-neutral seam between the Slint host and the operating system's
// file dialog (Story 13.6). The real picker wraps nativefiledialog-extended in
// nfd_file_picker.cpp, which is linked into the application only; the host and
// its tests see nothing but this interface, so tests and live checks substitute
// the scripted picker below. No Slint or NFD type appears here.
namespace motif::slint_app
{

enum class picker_kind : std::uint8_t
{
    // A directory: a database bundle to open, or where to create one.
    folder,
    // One existing file: a PGN file or an engine executable.
    file,
};

// One entry of the dialog's file-type list. `spec` is a comma-separated list of
// extensions without dots, for example "pgn". The dialog adds its own all-files
// entry.
struct picker_filter
{
    std::string name;
    std::string spec;

    [[nodiscard]] auto operator==(picker_filter const&) const -> bool = default;
};

struct picker_request
{
    picker_kind kind {picker_kind::file};
    // The directory the dialog opens in; empty lets the system choose.
    std::string start_path;
    std::vector<picker_filter> filters;

    [[nodiscard]] auto operator==(picker_request const&) const -> bool = default;
};

enum class picker_result : std::uint8_t
{
    chosen,
    cancelled,
    // The dialog could not be shown, or the system reported an error.
    failed,
};

struct picker_outcome
{
    picker_result result {picker_result::cancelled};
    // The chosen path, for `chosen`.
    std::string path;
    // The reason, for `failed`: the toolkit's own text, ASCII, possibly empty.
    std::string message;

    [[nodiscard]] static auto chosen(std::string path) -> picker_outcome;
    [[nodiscard]] static auto cancelled() -> picker_outcome;
    [[nodiscard]] static auto failed(std::string message) -> picker_outcome;

    [[nodiscard]] auto operator==(picker_outcome const&) const -> bool = default;
};

// Which thread a picker's dialog must run on.
enum class picker_thread : std::uint8_t
{
    // A worker thread: the dialog blocks until the user answers.
    worker,
    // The event-loop thread: the platform's dialog needs it (AppKit).
    event_loop,
};

class file_picker
{
  public:
    file_picker() = default;
    virtual ~file_picker() = default;
    file_picker(file_picker const&) = delete;
    auto operator=(file_picker const&) -> file_picker& = delete;
    file_picker(file_picker&&) = delete;
    auto operator=(file_picker&&) -> file_picker& = delete;

    [[nodiscard]] virtual auto thread() const noexcept -> picker_thread { return picker_thread::worker; }

    // True when abort() makes an open dialog return. The system's dialogs cannot
    // be closed from outside, so the real picker says no.
    [[nodiscard]] virtual auto abortable() const noexcept -> bool { return false; }

    // Shows the dialog and blocks until the user answers or it fails. Called on
    // the thread `thread()` names, one dialog at a time.
    [[nodiscard]] virtual auto pick(picker_request const& request) -> picker_outcome = 0;

    // Makes a blocked pick() return `cancelled`. Only called when abortable().
    virtual void abort() noexcept {}
};

// The directory a dialog should open in for the text of a path field: the
// directory itself, the parent of a file, or for a path that does not exist the
// nearest ancestor that does. Empty when the text is empty or nothing exists.
[[nodiscard]] auto picker_start_path(std::string_view field_text) -> std::string;

// A picker that answers from a script instead of showing a dialog: for the
// headless tests, and for live checks through MOTIF_FILE_PICKER.
class scripted_file_picker final : public file_picker
{
  public:
    enum class mode : std::uint8_t
    {
        choose,
        cancel,
        fail,
        // Blocks until finish(), or abort() when abortable.
        hang,
    };

    struct script
    {
        mode behaviour {mode::cancel};
        // The chosen path for `choose`, the message for `fail`.
        std::string text;
        bool abortable {true};
        picker_thread thread {picker_thread::worker};
    };

    explicit scripted_file_picker(script behaviour);

    [[nodiscard]] auto thread() const noexcept -> picker_thread override;
    [[nodiscard]] auto abortable() const noexcept -> bool override;
    [[nodiscard]] auto pick(picker_request const& request) -> picker_outcome override;
    void abort() noexcept override;

    // Ends a blocked `hang` pick() with this outcome. When none is blocked, the
    // next `hang` pick() returns it at once.
    void finish(picker_outcome outcome);

    // The requests received, in order.
    [[nodiscard]] auto requests() const -> std::vector<picker_request>;
    // The thread of the latest pick() call.
    [[nodiscard]] auto last_thread() const -> std::thread::id;
    // Blocks until pick() has been entered `count` times, or the timeout passes.
    [[nodiscard]] auto wait_for_calls(std::size_t count, std::chrono::milliseconds timeout) const -> bool;
    // Blocks until a pick() has returned.
    [[nodiscard]] auto wait_for_return(std::chrono::milliseconds timeout) const -> bool;

  private:
    script script_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<picker_request> requests_;
    std::thread::id last_thread_;
    std::size_t returned_ {0};
    bool aborted_ {false};
    std::optional<picker_outcome> finished_;
};

// The scripted picker named by a MOTIF_FILE_PICKER value: "choose:<path>",
// "cancel", "fail:<message>" or "hang". The hang ignores abort() exactly as the
// system's dialogs do, which shows the exit path. Null for any other value.
[[nodiscard]] auto make_scripted_picker(std::string_view spec) -> std::shared_ptr<file_picker>;

}  // namespace motif::slint_app
