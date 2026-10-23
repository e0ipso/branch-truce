#pragma once
#include "SessionModel.h"
#include <QObject>
#include <QString>

namespace branch_truce {
class ApplicationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(branch_truce::SessionModel *files READ files CONSTANT)
public:
    explicit ApplicationController(QObject *parent = nullptr);
    [[nodiscard]] QString version() const;
    [[nodiscard]] SessionModel *files() noexcept { return &files_; }
private:
    SessionModel files_;
};
}
