// File presentation turns a stored file's facts and recovery state into the states and sentences views show.
#pragma once

#include "FileStates.h"
#include "session/SessionStore.h"
#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <cstddef>

namespace branch_truce {

// Reads one store. Every row is a storage row of that store. Recovery
// problems outrank every revision fact: until they clear, the recorded state
// may not be the state the user last saw.
class FilePresentation {
    Q_DECLARE_TR_FUNCTIONS(FilePresentation)
public:
    // The progress facts of a file, with pending input and recovery errors applied.
    struct Progress {
        // Contribution decisions retained for review and handoff, even without markers.
        std::size_t undecided;
        bool saved;
        bool reviewed;
        bool resolved;
        bool earlierStaged;
        // Current conflict regions used by the file row and conflict stepper.
        std::size_t conflicts = 0;
        // Every choice is decided and no marker remains; Mark resolved is next.
        bool settled = false;
    };
    // The handoff group a file belongs to and what it offers to do next.
    struct Route {
        FileStates::HandoffCategory category = FileStates::HandoffCategory::Published;
        FileStates::NextAction action = FileStates::NextAction::None;
        QString actionLabel;
    };

    explicit FilePresentation(const SessionStore &store) : store_(store) {}

    [[nodiscard]] static FileStates::SupportKind supportKind(Support support);
    // The category's key in handoff data, its icon and its handoff title.
    [[nodiscard]] static QString categoryKey(FileStates::HandoffCategory category);
    [[nodiscard]] static QString categoryIcon(FileStates::HandoffCategory category);
    [[nodiscard]] static QString categoryTitle(FileStates::HandoffCategory category);

    // Resolved with no pending input or recovery error.
    [[nodiscard]] bool resolved(std::size_t row) const;
    [[nodiscard]] bool unsaved(std::size_t row) const;
    [[nodiscard]] Progress progress(std::size_t row) const;
    // Revision IDs and progress flags for the progress role.
    [[nodiscard]] QVariantMap progressData(std::size_t row) const;
    [[nodiscard]] FileStates::StatusKind statusKind(std::size_t row) const;
    [[nodiscard]] FileStates::RecoveryState recoveryState(std::size_t row) const;
    // The status sentence of statusKind.
    [[nodiscard]] QString status(std::size_t row) const;
    // The row's one short status line.
    [[nodiscard]] QString statusLine(std::size_t row) const;
    // The status sentence, the draft state, then one {icon, text, tone} per
    // progress fact. tone is neutral, positive, negative or warning.
    [[nodiscard]] QVariantList chips(std::size_t row) const;
    // One sentence for a tooltip or an accessible description.
    [[nodiscard]] QString summary(std::size_t row) const;
    // Why the file keeps the session from finishing.
    [[nodiscard]] QStringList blockers(std::size_t row) const;
    [[nodiscard]] Route route(std::size_t row) const;

private:
    const SessionStore &store_;
};
}
