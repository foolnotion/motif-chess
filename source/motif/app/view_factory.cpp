#include <QUrl>

#include "motif/app/view_factory.hpp"

namespace motif::app
{

auto view_factory::titleBarFilename() const -> QUrl
{
    return {QStringLiteral("qrc:/qt/qml/com/motif/app/qml/MotifTitleBar.qml")};
}

auto view_factory::dockwidgetFilename() const -> QUrl
{
    return {QStringLiteral("qrc:/qt/qml/com/motif/app/qml/MotifDockWidget.qml")};
}

auto view_factory::separatorFilename() const -> QUrl
{
    return {QStringLiteral("qrc:/qt/qml/com/motif/app/qml/MotifSeparator.qml")};
}

}  // namespace motif::app
