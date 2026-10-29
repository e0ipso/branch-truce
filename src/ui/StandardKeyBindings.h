#pragma once

#include <QKeySequence>
#include <QObject>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
// Expose the platform's ordered standard bindings so actions can advertise the
// primary shortcut while registering each alternate without a duplicate grab.
class StandardKeyBindings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit StandardKeyBindings(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QStringList sequences(int standardKey) const
    {
        QStringList result;
        for (const auto &sequence : QKeySequence::keyBindings(static_cast<QKeySequence::StandardKey>(standardKey)))
            result.append(sequence.toString(QKeySequence::PortableText));
        return result;
    }
};
}
