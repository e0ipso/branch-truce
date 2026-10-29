#pragma once

#include <QObject>
#include <QQuickItem>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
// A popup's content item can own visual descendants outside its QObject tree.
// Find its menu owner without depending on Qt Quick's private popup classes.
class NamedActionMenu : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit NamedActionMenu(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QObject *owner(QQuickItem *content) const
    {
        if (!content) return nullptr;
        for (auto *object = content->parent(); object; object = object->parent()) {
            if (object->property("contentItem").value<QQuickItem *>() == content
                && object->metaObject()->indexOfMethod("dismiss()") >= 0)
                return object;
        }
        return nullptr;
    }
};
}
