#pragma once
#include "SessionModel.h"
#include "SessionCoordinator.h"
#include <QObject>
#include <QString>

namespace branch_truce {
class ApplicationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(branch_truce::SessionModel *files READ files CONSTANT)
    Q_PROPERTY(branch_truce::SessionCoordinator *coordinator READ coordinator CONSTANT)
public:
    explicit ApplicationController(QObject *parent = nullptr);
    [[nodiscard]] QString version() const;
    [[nodiscard]] SessionModel *files() noexcept { return &files_; }
    [[nodiscard]] SessionCoordinator *coordinator() noexcept { return &coordinator_; }
private:
    SessionModel files_;
    SessionCoordinator coordinator_;
};
}
