// Recovery records encode a file's state at an acknowledgment boundary and read each field back typed.
#pragma once

#include "core/SessionState.h"
#include "recovery/RecoveryNames.h"
#include "storage/RecoveryStore.h"
#include <QJsonObject>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace branch_truce {

// The two publication facts not exposed as user-facing outcomes. Replay tracks
// them at the same domain transitions that change PublicationState.
struct RecoveryBoundary {
    RevisionId floor = 1;
    bool reviewSaved = false;
};
[[nodiscard]] storage::RecoveryTransaction recoveryCheckpoint(const storage::RecoveryStore &store);
[[nodiscard]] storage::RecoveryTransaction recoveryFileCheckpoint(const FileSession &file, RecoveryBoundary boundary);

// The repository mutation a mutation record is about. An intent has no
// observation; the observation record repeats the intent with one.
struct MutationFact {
    QString id;
    MutationKind kind = MutationKind::Publication;
    std::optional<MutationObservation> observation;
    bool operator==(const MutationFact &) const = default;
};

// The record of file's complete state after action. The current text is
// stored as its change from previousText, the current text before action,
// so a keystroke stores the typed bytes rather than the whole file. Other
// revisions are named by their IDs; replay rebuilds them. Only the open
// record, whose previous text is empty, stores the immutable captures.
[[nodiscard]] storage::RecoveryTransaction recoveryTransaction(const FileSession &file, RecoveryAction action,
                                                               const QString &transactionId,
                                                               std::string_view previousText,
                                                               const std::optional<MutationFact> &mutation = {});

// What a record that cannot be read or replayed reports.
class RecoveryRecordError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    explicit RecoveryRecordError(Problem value) : std::runtime_error(""), problem(std::move(value)) {}
    Problem problem;
};

// Throws RecoveryRecordError with detail unless condition holds.
void requireRecord(bool condition, const char *detail);

// Reads the fields of one state that recoveryTransaction wrote, resolving
// payload references through the store. Every method throws
// RecoveryRecordError with the reason when its field is missing, malformed,
// names a value this build does not know or names a missing payload.
class RecoveryRecordReader {
public:
    RecoveryRecordReader(const storage::RecoveryStore &store, const QJsonObject &state,
                         qint64 retainedBytes = 256 * 1024 * 1024,
                         const QMap<QByteArray, QByteArray> *encodedPayloads = nullptr);
    [[nodiscard]] std::pair<FileSession, RecoveryBoundary>
    checkpointFile(std::shared_ptr<const CorrespondenceAlgorithm> correspondence = {}) const;
    [[nodiscard]] RecoveryAction action() const;
    [[nodiscard]] std::string fileId() const;
    [[nodiscard]] std::string path() const;
    [[nodiscard]] Support support() const;
    [[nodiscard]] std::string supportDetail() const;
    // The captures an open record stores; empty for a file without them.
    [[nodiscard]] std::optional<FileSnapshots> snapshots() const;
    // Reads only the captured root payload, for inventory filtering. Full replay
    // still validates all captures, operations and session identities.
    [[nodiscard]] std::string worktreeRoot() const;
    // The identity of the correspondence algorithm an open record's session aligned with.
    [[nodiscard]] std::string alignment() const;
    // The current draft. previousText is the current text before this record.
    [[nodiscard]] Draft draft(std::string_view previousText) const;
    // The current revision as the record names it, with its draft and hand edits.
    [[nodiscard]] RecordedRevision revision(std::string_view previousText) const;
    [[nodiscard]] std::set<std::string> handEdited() const;
    [[nodiscard]] std::vector<ByteRange> handEditedRanges() const;
    [[nodiscard]] bool reviewConfirmedUndecided() const;
    [[nodiscard]] OperationOutcome checkpointOutcome() const;
    [[nodiscard]] OperationOutcome writeOutcome() const;
    [[nodiscard]] OperationOutcome stageOutcome() const;
    // The staged object ID and mode.
    [[nodiscard]] std::pair<std::string, std::string> stagedObject() const;
    [[nodiscard]] std::optional<MutationFact> mutation() const;

private:
    [[nodiscard]] QByteArray payload(const QByteArray &digest, Problem *problem, qint64 maxBytes) const;
    [[nodiscard]] std::string bytes(const QJsonValue &value) const;
    [[nodiscard]] quint64 number(const QJsonValue &value) const;
    [[nodiscard]] qint64 signedNumber(const QJsonValue &value) const;
    [[nodiscard]] bool boolean(const QJsonValue &value) const;
    [[nodiscard]] OperationOutcome outcome(const QJsonValue &value) const;
    [[nodiscard]] TextSnapshot text(const QJsonValue &value) const;
    [[nodiscard]] std::optional<SourceSnapshot> source(const QJsonValue &value) const;
    [[nodiscard]] std::map<std::string, std::string> mapping(const QJsonValue &value) const;
    const storage::RecoveryStore &store_;
    const QJsonObject &state_;
    mutable qint64 remainingBytes_;
    const QMap<QByteArray, QByteArray> *encodedPayloads_;
};
}
