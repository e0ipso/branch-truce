#include "ApplicationController.h"
#include "core/BuildInfo.h"
#include <QStandardPaths>

namespace branch_truce {
ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent), files_(this),
      coordinator_(files_, QStandardPaths::findExecutable(QStringLiteral("git")), this) {}
QString ApplicationController::version() const
{
    return QString::fromUtf8(branch_truce::version().data(),
                             static_cast<qsizetype>(branch_truce::version().size()));
}
}
