// App paths name the standard locations the application keeps its own data in.
#include "AppPaths.h"
#include <QStandardPaths>
#include <QUuid>

namespace branch_truce {
QString recoveryRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/Branch Truce/recovery/v1");
}

QString newRecoveryDirectory()
{
    return recoveryRoot() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces);
}
}
