// The application composes the window's services and models and hands each view what it uses.
#pragma once
#include "ChoicePreviewFlow.h"
#include "CommandFeedback.h"
#include "FileCommands.h"
#include "FileNavigator.h"
#include "MergetoolStatus.h"
#include "ResultEditorAdapter.h"
#include "SelectedFile.h"
#include "Selection.h"
#include "SessionCompletion.h"
#include "SessionLoader.h"
#include "SessionModel.h"
#include "core/Correspondence.h"
#include "mergetool/MergetoolSession.h"
#include "publication/ResolutionPublisher.h"
#include "session/SessionCoordinator.h"
#include "session/SessionStore.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <memory>

namespace branch_truce {

// The composition root of one window's session. It owns every collaborator,
// builds them in dependency order and connects the few signals that cross
// between them. Views get the objects themselves through windowProperties(),
// never this class.
class Application final : public QObject {
    Q_OBJECT
public:
    struct Config {
        // The Git every collaborator and worker task runs; main resolves it
        // once. Empty when Git is not on PATH.
        QString gitExecutable;
        // This session's recovery store; see newRecoveryDirectory().
        QString recoveryDirectory;
        // Loaded files align their sources with this, or with the engine's default.
        std::shared_ptr<const CorrespondenceAlgorithm> correspondence;
        // The configuration files Git reads. A waiting launcher must have
        // started its Git with the same ones. The process environment selects
        // them by default, as it does for the Git the session's tasks run.
        git::GitEnvironment gitEnvironment = git::GitEnvironment::fromProcess();
        // Where the session listens for mergetool launchers and they leave their inputs.
        QString mergetoolDirectory = mergetoolSocketDirectory(qgetenv("XDG_RUNTIME_DIR"));
        // The file system the recovery store and repository publication write through.
        std::shared_ptr<const storage::FileSystem> files = storage::systemFileSystem();
        // The runner of each Git task the session queues, such as staging a resolved file. Empty
        // runs gitExecutable.
        git::GitRunners sessionGit = {};
    };
    explicit Application(Config config, QObject *parent = nullptr);

    // Main.qml's required properties, by name.
    [[nodiscard]] QVariantMap windowProperties();

    [[nodiscard]] SessionStore *store() noexcept { return &store_; }
    [[nodiscard]] SessionModel *files() noexcept { return &files_; }
    [[nodiscard]] SessionCoordinator *coordinator() noexcept { return &coordinator_; }
    [[nodiscard]] ResolutionPublisher *publisher() noexcept { return &publisher_; }
    [[nodiscard]] MergetoolSession *mergetoolSession() noexcept { return &mergetool_; }
    [[nodiscard]] ResultEditorAdapter *editor() noexcept { return &editor_; }
    [[nodiscard]] Selection *selection() noexcept { return &selection_; }
    [[nodiscard]] SelectedFile *selectedFile() noexcept { return &selectedFile_; }
    [[nodiscard]] CommandFeedback *feedback() noexcept { return &feedback_; }
    [[nodiscard]] ChoicePreviewFlow *choices() noexcept { return &choices_; }
    [[nodiscard]] FileNavigator *navigator() noexcept { return &navigator_; }
    [[nodiscard]] FileCommands *commands() noexcept { return &commands_; }
    [[nodiscard]] SessionCompletion *completion() noexcept { return &completion_; }
    [[nodiscard]] MergetoolStatus *mergetool() noexcept { return &mergetoolStatus_; }
    [[nodiscard]] SessionLoader *loader() noexcept { return &loader_; }

private:
    SessionStore store_;
    SessionModel files_;
    SessionCoordinator coordinator_;
    ResolutionPublisher publisher_;
    MergetoolSession mergetool_;
    ResultEditorAdapter editor_;
    Selection selection_;
    SelectedFile selectedFile_;
    CommandFeedback feedback_;
    ChoicePreviewFlow choices_;
    FileNavigator navigator_;
    FileCommands commands_;
    SessionCompletion completion_;
    MergetoolStatus mergetoolStatus_;
    SessionLoader loader_;
};
}
