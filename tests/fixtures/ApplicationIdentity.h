#pragma once

// Sets the application and organization names for one case and restores both when the case
// leaves the scope, failed or not. The standard locations, and so the recovery root, follow them.

#include <QCoreApplication>
#include <QString>

namespace test_support {

class ScopedApplicationIdentity {
public:
    ScopedApplicationIdentity(const QString &application, const QString &organization)
        : application_(QCoreApplication::applicationName()), organization_(QCoreApplication::organizationName())
    {
        QCoreApplication::setApplicationName(application);
        QCoreApplication::setOrganizationName(organization);
    }
    ~ScopedApplicationIdentity()
    {
        QCoreApplication::setApplicationName(application_);
        QCoreApplication::setOrganizationName(organization_);
    }
    ScopedApplicationIdentity(const ScopedApplicationIdentity &) = delete;
    ScopedApplicationIdentity &operator=(const ScopedApplicationIdentity &) = delete;

private:
    QString application_;
    QString organization_;
};

} // namespace test_support
