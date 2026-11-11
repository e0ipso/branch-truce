// The startup report describes the opened session as one JSON object for smoke tests and scripts.
#pragma once
#include <QJsonObject>

namespace branch_truce {
class SessionLoader;
class SessionModel;

// The launch state and every listed file: state, repository, operation,
// detail, the stage identities, workflow, resolved and files, each with path,
// support (supported, outside-app, unreadable or large-file), detail and text.
[[nodiscard]] QJsonObject startupReport(const SessionLoader &loader, const SessionModel &files);
}
