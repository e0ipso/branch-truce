// Conflict classification separates editable text conflicts from files that need another tool.
#pragma once

#include "TextConversion.h"
#include "core/FileSnapshot.h"

namespace branch_truce::git {
enum class ConflictSupport {
    SupportedText, MissingSide, ModeConflict, UnsupportedType, TopologyConflict,
    UnsupportedText, UnsupportedConversion, UnverifiedSource, UnsupportedOperation,
    InspectionFailure, LoadFailure
};
struct Conflict {
    QByteArray path; // Exact repository filename bytes, including unsupported paths.
    QList<IndexEntry> stages;
    ConflictSupport support = ConflictSupport::InspectionFailure;
    QString reason; // Empty only for supported text. Unsupported work stays outstanding.
    QByteArray mode; // Agreed stage-2/stage-3 mode, never inferred from the result.
    QList<ConvertedText> sources; // Present only after every available stage passes.
    std::optional<storage::TextDocument> result; // Preserves launch-time manual edits.
    std::optional<SnapshotFileIdentity> resultIdentity; // Same stable read as result.
    // Display-only size from the stable launch read, retained even if text admission fails.
    qint64 launchSize = -1;
    [[nodiscard]] bool editable() const { return support == ConflictSupport::SupportedText; }
};
struct ConflictClassificationRequest {
    QString directory;
    quint64 sessionGeneration = 0;
    quint64 inputRevision = 0;
};
struct ConflictClassificationResult {
    ConflictClassificationRequest request;
    Outcome outcome = Outcome::Rejected;
    RepositoryContext repository;
    QList<Conflict> conflicts;
    QString diagnostic;
};

// Read-only editing admission: discovery alone verifies operation identity, and
// TextConversion alone converts a selected source. Use this inventory to admit
// a complete conflict, retaining every excluded path and its outside-app reason.
// This is a snapshot, not permission to publish without fresh mutation guards.
class ConflictClassifier final : public QObject {
    Q_OBJECT
public:
    explicit ConflictClassifier(const QString &executable, QObject *parent = nullptr);
    ~ConflictClassifier() override;
    bool start(const ConflictClassificationRequest &request);
    void cancel();
    [[nodiscard]] bool busy() const;
Q_SIGNALS:
    void finished(const branch_truce::git::ConflictClassificationResult &result);
private:
    class State;
    std::unique_ptr<State> d;
};
} // namespace branch_truce::git
Q_DECLARE_METATYPE(branch_truce::git::ConflictClassificationResult)
