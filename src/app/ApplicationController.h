#pragma once
#include <QObject>
#include <QString>

namespace branch_truce {
class ApplicationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
public:
    explicit ApplicationController(QObject *parent = nullptr);
    [[nodiscard]] QString version() const;
};
}
