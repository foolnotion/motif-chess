#include <QObject>
#include <QString>

#include "motif/app/kddw_wayland_bridge.hpp"

#include <kddockwidgets/core/DockRegistry.h>
#include <kddockwidgets/core/DropArea.h>
#include <kddockwidgets/core/MainWindow.h>
#include <kddockwidgets/qtcommon/View.h>

namespace motif::app
{

kddw_wayland_bridge::kddw_wayland_bridge(QObject* parent)
    : QObject(parent)
{
}

auto kddw_wayland_bridge::drop_area_view() const -> QObject*
{
    return drop_area_view_;
}

void kddw_wayland_bridge::refresh()
{
    using KDDockWidgets::DockRegistry;
    using KDDockWidgets::QtCommon::View_qt;

    auto* main_window = DockRegistry::self()->mainWindowByName(QStringLiteral("main_dock_area"));
    if (main_window == nullptr) {
        return;
    }

    auto* drop_area = main_window->dropArea();
    if (drop_area == nullptr) {
        return;
    }

    auto* view = View_qt::asQObject(drop_area->view());
    if (view != drop_area_view_) {
        drop_area_view_ = view;
        Q_EMIT drop_area_view_changed();
    }
}

}  // namespace motif::app
