#include "ApplicationController.h"
#include "core/BuildInfo.h"

namespace branch_truce {
ApplicationController::ApplicationController(QObject *parent) : QObject(parent) {}
QString ApplicationController::version() const
{
    return QString::fromUtf8(branch_truce::version().data(),
                             static_cast<qsizetype>(branch_truce::version().size()));
}
}
