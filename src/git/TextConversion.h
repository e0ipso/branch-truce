// Text conversion checks Git attributes and prepares source or result bytes under the captured policy.
#pragma once

#include "GitAdapter.h"
#include "storage/TextFormat.h"
#include <optional>

namespace branch_truce::git {
enum class TextOperation { ReadSource, PrepareResult, VerifyPublished };
enum class TextConversionFailure { None, UnsupportedText, UnsupportedConversion };
struct ConvertedText {
    QString worktreeRoot;
    QByteArray path;
    QByteArray mode; // Agreed stage-2/stage-3 result mode.
    QByteArray sourceMode; // Original selected stage mode, for a source snapshot.
    QByteArray canonicalObjectId;
    QByteArray canonicalBytes;
    storage::TextDocument document;
    ConversionPolicy policy;
};
struct TextConversionRequest {
    TextOperation operation = TextOperation::ReadSource;
    QString directory;
    QByteArray path;
    QByteArray objectId;
    unsigned int sourceStage = 2;
    quint64 sessionGeneration = 0;
    quint64 inputRevision = 0;
    QList<IndexEntry> stages; // Captured unmerged entries for this exact path.
    storage::TextFormat launchResultFormat; // Never substitute a side's format.
    QByteArray editedUtf8;
    std::optional<ConvertedText> expected; // Required for VerifyPublished.
};
struct TextConversionResult {
    TextConversionRequest request;
    Outcome outcome = Outcome::Rejected;
    TextConversionFailure failure = TextConversionFailure::None;
    QString diagnostic;
    std::optional<ConvertedText> text;
};

// Converts and verifies only. The publication coordinator still owns review,
// operation/index/destination baselines, guarded writes, staging and acknowledgment.
class TextConversion final : public QObject {
    Q_OBJECT
public:
    explicit TextConversion(const QString &executable, QObject *parent = nullptr);
    ~TextConversion() override;
    bool start(const TextConversionRequest &request);
    void cancel();
    [[nodiscard]] bool busy() const;
Q_SIGNALS:
    void finished(const branch_truce::git::TextConversionResult &result);
private:
    class State;
    std::unique_ptr<State> d;
};
} // namespace branch_truce::git
Q_DECLARE_METATYPE(branch_truce::git::TextConversionResult)
