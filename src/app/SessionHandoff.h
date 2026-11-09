// The session handoff says which files remain outstanding and whether the session may finish or be left.
#pragma once

#include "FilePresentation.h"
#include "FileStates.h"
#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <cstddef>
#include <string>
#include <vector>

namespace branch_truce {
class SessionStore;

// One file under exactly one category, with its blockers and next action.
struct HandoffEntry {
    std::string fileId;
    QString path;
    FileStates::HandoffCategory category = FileStates::HandoffCategory::Published;
    QString status;
    QStringList blockers;
    bool unsaved = false;
    std::size_t undecided = 0;
    bool outstanding = false;
    FileStates::NextAction action = FileStates::NextAction::None;
    QString actionLabel;
};

struct HandoffGroup {
    FileStates::HandoffCategory category = FileStates::HandoffCategory::Published;
    std::vector<HandoffEntry> entries;
};

// Every file grouped in handoff order, plus what the session itself still
// waits on. clean is the only success claim.
struct Handoff {
    bool clean = false;
    int totalFiles = 0;
    int outstandingFiles = 0;
    QString outcome;
    // Session-level work that keeps a resolved session from finishing.
    QStringList sessionBlockers;
    bool canLeave = false;
    // Why leaving now could lose work, or empty.
    QString leaveBlocker;
    std::vector<HandoffGroup> groups;

    // The handoff as the views read it: clean, totalFiles, outstandingFiles,
    // counts per category key, groups of {category, title, entries}, outcome,
    // sessionBlockers, canLeave and leaveBlocker. Each entry carries fileId,
    // path, category, icon, status, blockers, unsaved, undecided, outstanding,
    // action (a FileStates::NextAction) and actionLabel.
    [[nodiscard]] QVariantMap toVariant() const;
};

// What the session waits on beyond its files.
struct SessionActivity {
    // A Mark resolved is running; its outcome is not known yet.
    bool resolutionRunning = false;
    // Launched paths Git still waits on without a decision.
    int unreleasedLaunches = 0;
    // The session already finished or was left.
    bool ended = false;
};

class SessionHandoff {
    Q_DECLARE_TR_FUNCTIONS(SessionHandoff)
public:
    [[nodiscard]] static Handoff build(const SessionStore &store, const SessionActivity &activity);
};
}
