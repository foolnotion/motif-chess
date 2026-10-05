#pragma once

#include <memory>
#include <string>

#include "motif/slint_app/file_picker.hpp"

// The operating system's file dialog through nativefiledialog-extended (NFD).
// This is the only translation unit that includes <nfd.h>; the executable links
// it and the service library and its tests never do. On Linux NFD talks to the
// xdg-desktop-portal file chooser over D-Bus, on macOS it runs NSOpenPanel, on
// Windows IFileDialog.
namespace motif::slint_app
{

class nfd_file_picker final : public file_picker
{
  public:
    // AppKit needs the main thread; the other backends block a worker.
    [[nodiscard]] auto thread() const noexcept -> picker_thread override;
    // NFD returns no handle to an open dialog, so nothing can close it.
    [[nodiscard]] auto abortable() const noexcept -> bool override;
    // NFD_Init, the open-file or pick-folder call, NFD_Quit, all on the calling
    // thread. Copies NFD's result and error text, frees them, clears the error.
    [[nodiscard]] auto pick(picker_request const& request) -> picker_outcome override;
};

// What NFD's error text means for the user. The portal backend reports
// "D-Bus file dialog interaction was ended abruptly with response code N" for
// every response other than OK and cancel. Code 2 is the portal's "ended in some
// other way", which the GTK backend sends when the dialog is closed with Escape,
// so it is a cancel and shows no error. Any other text, including other codes, is
// a failure that carries the text.
[[nodiscard]] auto picker_outcome_for_nfd_error(std::string message) -> picker_outcome;

// The picker the application uses: the scripted one named by MOTIF_FILE_PICKER
// when that is set to a recognised value (see make_scripted_picker), otherwise
// NFD. An unrecognised value is reported on stderr and ignored.
[[nodiscard]] auto make_default_file_picker() -> std::shared_ptr<file_picker>;

}  // namespace motif::slint_app
