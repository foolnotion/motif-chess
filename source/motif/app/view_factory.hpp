#pragma once

#include <QUrl>

#include <kddockwidgets/qtquick/ViewFactory.h>

namespace motif::app
{

// Overrides the default KDDW title bar with a compact HiDPI-correct one.
class view_factory : public KDDockWidgets::QtQuick::ViewFactory
{
    Q_OBJECT
  public:
    [[nodiscard]] auto titleBarFilename() const -> QUrl override;
    [[nodiscard]] auto dockwidgetFilename() const -> QUrl override;
    [[nodiscard]] auto separatorFilename() const -> QUrl override;
};

}  // namespace motif::app
