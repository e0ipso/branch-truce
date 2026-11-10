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
    // What main resolves from the process once. Nothing here reads the
    // process environment itself.
    struct Config {
        // The Git every collaborator and worker task runs, and the
        // configuration files it reads. A waiting launcher must have started
        // its Git with the same files. The executable is empty when Git is not
        // on PATH.
        git::GitProgram git;
        // This session's recovery store; see newRecoveryDirectory().
        QString recoveryDirectory;
        // Where the session listens for mergetool launchers and they leave
        // their inputs; see mergetoolSocketDirectory().
        QString mergetoolDirectory;
        // Loaded files align their sources with this, or with the engine's default.
        std::shared_ptr<const CorrespondenceAlgorithm> correspondence = {};
        // The file system the recovery store and repository publication write through.
        std::shared_ptr<const storage::FileSystem> files = storage::systemFileSystem();
        // Builds every Git runner the session uses: classification, conversion,
        // queued tasks and recovery checks. Empty runs GitAdapters for git.
        git::GitRunners gitRunners = {};
    };
    explicit Application(Config config, QObject *parent = nullptr);
    // Stops every running Git process of the session at once, so they end
    // within one grace period while the collaborators go.
    ~Application() override;

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
    // First, so it outlives every collaborator that runs Git.
    std::shared_ptr<git::GitProcesses> gitProcesses_;
    // The coordinator's workers; they outlive it, so it can wait for them.
    QThreadPool workers_;
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
