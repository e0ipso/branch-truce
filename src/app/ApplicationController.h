#pragma once
#include "SessionModel.h"
#include "SessionCoordinator.h"
#include "git/ConflictClassifier.h"
#include <QObject>
#include <QString>

namespace branch_truce {
class ApplicationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(branch_truce::SessionModel *files READ files CONSTANT)
    Q_PROPERTY(branch_truce::SessionCoordinator *coordinator READ coordinator CONSTANT)
    Q_PROPERTY(QString startupState READ startupState NOTIFY launchChanged)
    Q_PROPERTY(bool opening READ opening NOTIFY launchChanged)
    Q_PROPERTY(QString repositoryRoot READ repositoryRoot NOTIFY launchChanged)
    Q_PROPERTY(QString operation READ operation NOTIFY launchChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY launchChanged)
    Q_PROPERTY(QString stage2 READ stage2 NOTIFY launchChanged)
    Q_PROPERTY(QString stage3 READ stage3 NOTIFY launchChanged)
    Q_PROPERTY(QString workflow READ workflow CONSTANT)
public:
    explicit ApplicationController(QObject *parent = nullptr);
    [[nodiscard]] QString version() const;
    [[nodiscard]] SessionModel *files() noexcept { return &files_; }
    [[nodiscard]] SessionCoordinator *coordinator() noexcept { return &coordinator_; }
    [[nodiscard]] QString startupState() const { return state_; }
    [[nodiscard]] bool opening() const { return state_ == QLatin1String("opening"); }
    [[nodiscard]] QString repositoryRoot() const { return repository_.worktreeRoot; }
    [[nodiscard]] QString operation() const { return operation_; }
    [[nodiscard]] QString detail() const { return detail_; }
    [[nodiscard]] QString stage2() const;
    [[nodiscard]] QString stage3() const;
    [[nodiscard]] QString workflow() const;
    // One launched repository session per controller. Paths are launch directories,
    // never inferred from a filename or a Git environment override.
    bool openRepository(const QString &directory);

Q_SIGNALS:
    void launchChanged();

private:
    void classified(const git::ConflictClassificationResult &result);
    void loadWorkingFiles(QList<git::Conflict> conflicts);
    void setState(QString state, QString detail);
    SessionModel files_;
    SessionCoordinator coordinator_;
    git::ConflictClassifier classifier_;
    git::RepositoryContext repository_;
    QString state_ = QStringLiteral("idle");
    QString operation_;
    QString detail_;
};
}
