#pragma once

#include <QObject>

namespace motif::app
{

// Exposes the KDDW main window's C++ DropArea view to QML.
// On Wayland, QDrag-based dock-widget drag requires the drop target window to have a
// QML DropArea item with dropAreaCpp wired up.  FloatingWindow.qml does this internally;
// for the main ApplicationWindow we have to do it ourselves.
class kddw_wayland_bridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* drop_area_view READ drop_area_view NOTIFY drop_area_view_changed)

  public:
    explicit kddw_wayland_bridge(QObject* parent = nullptr);

    auto drop_area_view() const -> QObject*;

    // Call from QML Component.onCompleted on the DockingArea — at that point the KDDW
    // MainWindow is fully initialised and DockRegistry knows about it.
    Q_INVOKABLE void refresh();

  Q_SIGNALS:
    void drop_area_view_changed();

  private:
    QObject* drop_area_view_ {nullptr};
};

}  // namespace motif::app
