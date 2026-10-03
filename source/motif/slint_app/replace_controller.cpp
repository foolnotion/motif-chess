#include <functional>
#include <string_view>
#include <utility>

#include "motif/slint_app/replace_controller.hpp"

#include "motif/slint_app/replace_gate.hpp"

namespace motif::slint_app
{

replace_controller::replace_controller(hooks callbacks)
    : hooks_(std::move(callbacks))
{
}

void replace_controller::request(std::string_view const action_label, std::function<void()> action)
{
    if (gate_.request(hooks_.edited(), std::move(action)) == replace_gate::outcome::ran) {
        return;
    }
    hooks_.show(action_label);
}

void replace_controller::accept()
{
    hooks_.hide();
    gate_.accept();
}

void replace_controller::cancel()
{
    gate_.cancel();
    hooks_.hide();
    hooks_.restore_focus();
}

auto replace_controller::pending() const noexcept -> bool
{
    return gate_.pending();
}

}  // namespace motif::slint_app
