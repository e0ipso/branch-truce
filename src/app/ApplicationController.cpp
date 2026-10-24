#include "ApplicationController.h"
#include "core/BuildInfo.h"
#include "storage/TextFormat.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QStandardPaths>
#include <algorithm>

namespace branch_truce {
ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent), files_(this),
      coordinator_(files_, QStandardPaths::findExecutable(QStringLiteral("git")), this),
      discovery_(QStandardPaths::findExecutable(QStringLiteral("git")), this)
{
    connect(&discovery_, &git::GitAdapter::finished, this, [this](const git::Result &result) {
        // Starting the next adapter operation may replace its stored result.
        const auto snapshot = result;
        if (snapshot.request.operation == git::Operation::DiscoverRepository) {
            discovered(snapshot);
            return;
        }
        auto &file = openingFiles_.at(nextFile_);
        if (snapshot.outcome != git::Outcome::Success) {
            file.support = Support::OutsideApp;
            file.detail = snapshot.diagnostic;
        } else {
            file.sources.push_back(snapshot.standardOutput);
            file.encoding = snapshot.conversionPolicy.attributes.value("working-tree-encoding");
        }
        ++nextStage_;
        materializeNext();
    });
}
QString ApplicationController::version() const
{
    return QString::fromUtf8(branch_truce::version().data(),
                             static_cast<qsizetype>(branch_truce::version().size()));
}

QString ApplicationController::stage2() const
{
    if (!repository_.supported) return {};
    return repository_.stage2.label + QStringLiteral(" · ") + QString::fromLatin1(repository_.stage2.commitId);
}

QString ApplicationController::stage3() const
{
    if (!repository_.supported) return {};
    return repository_.stage3.label + QStringLiteral(" · ") + QString::fromLatin1(repository_.stage3.commitId);
}

QString ApplicationController::workflow() const
{
    return tr("Continue the operation and commit in your usual Git workflow. "
              "This session covers only the work opened at launch.");
}

void ApplicationController::setState(QString state, QString detail)
{
    state_ = std::move(state);
    detail_ = std::move(detail);
    Q_EMIT launchChanged();
}

bool ApplicationController::openRepository(const QString &directory)
{
    if (state_ != QLatin1String("idle")) return false;
    setState(QStringLiteral("opening"), tr("Inspecting the selected local repository…"));
    git::Request request;
    request.directory = directory;
    if (!discovery_.start(request)) {
        setState(QStringLiteral("error"), tr("Could not start repository inspection."));
        return false;
    }
    return true;
}

void ApplicationController::discovered(const git::Result &result)
{
    repository_ = result.repository;
    switch (repository_.operation) {
    case git::RepositoryOperation::Merge: operation_ = tr("Merge"); break;
    case git::RepositoryOperation::Rebase: operation_ = tr("Rebase"); break;
    case git::RepositoryOperation::Unsupported: operation_ = tr("Unsupported operation"); break;
    case git::RepositoryOperation::None: break;
    }
    if (result.outcome != git::Outcome::Success) operation_.clear();

    QMap<QByteArray, QList<git::IndexEntry>> paths;
    for (const auto &entry : result.indexEntries) paths[entry.path].push_back(entry);
    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        OpeningFile file;
        file.path = it.key();
        file.stages = it.value();
        if (!repository_.supported) {
            file.support = Support::OutsideApp;
            file.detail = result.diagnostic;
        } else if (repository_.unsupportedPaths.contains(file.path)) {
            file.support = Support::OutsideApp;
            file.detail = repository_.unsupportedPaths.value(file.path);
        } else {
            const auto stage = [&](unsigned int number) {
                return std::find_if(file.stages.cbegin(), file.stages.cend(),
                                    [number](const auto &entry) { return entry.stage == number; });
            };
            const auto left = stage(2);
            const auto right = stage(3);
            const bool regular = std::all_of(file.stages.cbegin(), file.stages.cend(), [](const auto &entry) {
                return entry.mode == "100644" || entry.mode == "100755";
            });
            if (left == file.stages.cend() || right == file.stages.cend() || !regular
                || left->mode != right->mode) {
                file.support = Support::OutsideApp;
                file.detail = tr("Resolve this deletion, file type or permission conflict outside Branch Truce.");
            }
        }
        openingFiles_.push_back(std::move(file));
    }

    if (result.outcome != git::Outcome::Success || !repository_.supported) {
        std::vector<FileSession> files;
        for (const auto &file : openingFiles_) {
            files.emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), Draft{},
                               Support::OutsideApp, file.detail.toStdString());
        }
        if (!files.empty()) coordinator_.replaceSession(repository_.worktreeRoot, QStringLiteral("unsupported"), std::move(files));
        if (result.outcome != git::Outcome::Success) {
            setState(QStringLiteral("error"), tr("Could not open the selected work: %1").arg(result.diagnostic));
        } else if (repository_.operation == git::RepositoryOperation::None) {
            setState(QStringLiteral("no-session"), tr("No merge or rebase is in progress in this repository."));
        } else {
            setState(QStringLiteral("unsupported"), result.diagnostic);
        }
        return;
    }
    if (openingFiles_.empty()) {
        setState(QStringLiteral("no-session"), tr("This operation has no current unmerged files. Return to your Git workflow."));
        return;
    }
    materializeNext();
}

void ApplicationController::materializeNext()
{
    while (nextFile_ < openingFiles_.size()) {
        auto &file = openingFiles_[nextFile_];
        if (file.support == Support::Supported && nextStage_ < file.stages.size()) {
            git::Request request;
            request.operation = git::Operation::MaterializeSource;
            request.directory = repository_.worktreeRoot;
            request.path = file.path;
            request.objectId = file.stages[nextStage_].objectId;
            if (!discovery_.start(request)) {
                setState(QStringLiteral("error"), tr("Could not inspect conflict sources."));
            }
            return;
        }
        ++nextFile_;
        nextStage_ = 0;
    }
    loadWorkingFiles();
}

void ApplicationController::loadWorkingFiles()
{
    std::vector<FileSession> pending;
    for (const auto &file : openingFiles_) {
        pending.emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), Draft{},
                             Support::Unreadable, "Reading the current working file");
    }
    const auto identity = operation_ + QLatin1Char(':') + QString::fromLatin1(repository_.stage2.commitId)
        + QLatin1Char(':') + QString::fromLatin1(repository_.stage3.commitId);
    const auto root = repository_.worktreeRoot;
    if (!coordinator_.replaceSession(root, identity, std::move(pending))) {
        setState(QStringLiteral("error"), tr("Could not bind the session to the discovered worktree."));
        return;
    }
    auto loaded = std::make_shared<std::vector<FileSession>>();
    const auto anchor = openingFiles_.front().path.toHex().toStdString();
    const auto task = coordinator_.submit(anchor, 1,
        [files = std::move(openingFiles_), root, loaded](const TaskInput &, const TaskControl &control) mutable {
            for (auto &file : files) {
                if (control.cancellationRequested()) return TaskResult{TaskStatus::Cancelled, {}, {}, {}, {}};
                Draft draft;
                for (const auto &source : file.sources) {
                    const auto decoded = storage::decodeText(source, file.encoding);
                    if (!decoded.document) {
                        file.support = Support::OutsideApp;
                        file.detail = decoded.diagnostic;
                        break;
                    }
                }
                if (file.support == Support::Supported) {
                    const auto path = QFile::decodeName(file.path);
                    bool regular = QFile::encodeName(path) == file.path;
                    auto fullPath = root;
                    const auto parts = path.split(QLatin1Char('/'));
                    for (qsizetype i = 0; regular && i < parts.size(); ++i) {
                        fullPath = QDir(fullPath).filePath(parts[i]);
                        const QFileInfo info(fullPath);
                        regular = !info.isSymLink() && (i + 1 == parts.size() ? info.isFile() : info.isDir());
                    }
                    QFile input(fullPath);
                    constexpr qint64 limit = 8 * 1024 * 1024;
                    if (!regular || !input.open(QIODevice::ReadOnly)) {
                        file.support = Support::Unreadable;
                        file.detail = QStringLiteral("Could not read a regular working file without changing its filename or following symlinks.");
                    } else {
                        const auto bytes = input.read(limit + 1);
                        if (input.error() != QFileDevice::NoError || bytes.size() > limit) {
                            file.support = Support::Unreadable;
                            file.detail = QStringLiteral("The working file could not be read within the 8 MiB limit.");
                        } else {
                            const auto decoded = storage::decodeText(bytes, file.encoding);
                            if (!decoded.document) {
                                file.support = Support::OutsideApp;
                                file.detail = decoded.diagnostic;
                            } else {
                                draft.text = decoded.document->utf8.toStdString();
                                const auto lines = decoded.document->utf8.split('\n');
                                draft.unresolvedMarkers = static_cast<std::size_t>(std::count_if(
                                    lines.cbegin(), lines.cend(), [](const auto &line) { return line.startsWith("<<<<<<<"); }));
                            }
                        }
                    }
                }
                loaded->emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), std::move(draft),
                                     file.support, file.detail.toStdString());
            }
            return TaskResult{};
        }, [this, root, identity, loaded](const TaskCompletion &completion) {
            if (completion.result.status != TaskStatus::Succeeded
                || !coordinator_.replaceSession(root, identity, std::move(*loaded))) {
                setState(QStringLiteral("error"), tr("Could not load the current conflict files."));
                return;
            }
            setState(QStringLiteral("ready"), tr("Current conflict files are open. Files marked outside app remain outstanding."));
        });
    if (task == 0) setState(QStringLiteral("error"), tr("Could not schedule conflict loading."));
}
}
