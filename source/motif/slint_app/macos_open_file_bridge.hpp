#pragma once

#include <functional>
#include <string>

namespace motif::slint_app
{

using macos_open_file_handler = std::function<void(std::string)>;

// Registers the process-wide macOS open-document Apple-event handler. The
// handler executes on the AppKit thread and must forward work to the UI loop.
void install_macos_open_file_handler(macos_open_file_handler handler);

}  // namespace motif::slint_app
