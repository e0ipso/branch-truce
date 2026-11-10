// The application composes the window's services and models and hands each view what it uses.
#include "Application.h"

namespace branch_truce {
Application::Application(Config config, QObject *parent)
    : QObject(parent),
      store_(std::move(config.recoveryDirectory), config.files, this),
      files_(store_, this),
      coordinator_(store_, config.sessionGit ? config.sessionGit : git::gitAdapters(config.gitExecutable), this),
      alignment_(store_, this),
      publisher_(store_, coordinator_, config.gitExecutable, config.files, this),
      mergetool_(
          publisher_, store_, coordinator_, [this] { return loader_.repository(); },
          {config.mergetoolDirectory, config.gitEnvironment, config.gitExecutable}, this),
      editor_(store_, this),
      selection_(files_, editor_, this),
      selectedFile_(files_, selection_, this),
      feedback_(store_, selection_, this),
      choices_(store_, files_, editor_, selection_, feedback_, this),
      navigator_(store_, files_, editor_, selection_, this),
      commands_(store_, selection_, editor_, publisher_, mergetool_, feedback_, this),
      completion_(store_, publisher_, mergetool_, feedback_, this),
      mergetoolStatus_(mergetool_, publisher_, files_, selection_, this),
      loader_({store_, coordinator_, selection_, publisher_, mergetool_, completion_}, config.gitExecutable, this,
              std::move(config.correspondence))
{
    // The editor shows the selected file, and reloads it after any change.
    const auto show = [this] { editor_.show(selection_.fileId()); };
    connect(&selection_, &Selection::changed, this, show);
    connect(&files_, &SessionModel::summaryChanged, this, show);
    // The marker blocker follows the selected file's revision.
    connect(&files_, &SessionModel::summaryChanged, &feedback_, &CommandFeedback::changed);
    // The publisher's message is shown before the launchers react to its step.
    connect(&publisher_, &ResolutionPublisher::changed, this, [this](const QString &message) {
        feedback_.show(message);
        mergetool_.onPublisherChanged();
    });
    connect(&commands_, &FileCommands::resolutionChanged, &mergetoolStatus_, &MergetoolStatus::changed);
    connect(&commands_, &FileCommands::resolutionChanged, &completion_, &SessionCompletion::changed);
    // A new session drops the publisher's attempt, confirmation and offer.
    connect(&coordinator_, &SessionCoordinator::sessionChanged, &commands_, &FileCommands::resolutionChanged);
    connect(&editor_, &ResultEditorAdapter::notice, &feedback_, &CommandFeedback::show);
    connect(&editor_, &ResultEditorAdapter::loadRefused, this, [this](const QString &message) {
        if (feedback_.message() != message) feedback_.show(message);
    });
    connect(&loader_, &SessionLoader::notice, &feedback_, &CommandFeedback::show);
    connect(&mergetool_, &MergetoolSession::notice, &feedback_, &CommandFeedback::show);
}

QVariantMap Application::windowProperties()
{
    return {{QStringLiteral("files"), QVariant::fromValue(&files_)},
            {QStringLiteral("loader"), QVariant::fromValue(&loader_)},
            {QStringLiteral("selection"), QVariant::fromValue(&selection_)},
            {QStringLiteral("selectedFile"), QVariant::fromValue(&selectedFile_)},
            {QStringLiteral("feedback"), QVariant::fromValue(&feedback_)},
            {QStringLiteral("commands"), QVariant::fromValue(&commands_)},
            {QStringLiteral("choices"), QVariant::fromValue(&choices_)},
            {QStringLiteral("navigator"), QVariant::fromValue(&navigator_)},
            {QStringLiteral("editor"), QVariant::fromValue(&editor_)},
            {QStringLiteral("completion"), QVariant::fromValue(&completion_)},
            {QStringLiteral("mergetool"), QVariant::fromValue(&mergetoolStatus_)}};
}
}
