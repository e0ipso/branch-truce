#pragma once

#include "core/SessionState.h"
#include "storage/RecoveryStore.h"
#include <QJsonValue>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace branch_truce {
// Complete state at one acknowledgment boundary. Original sources and exact
// byte strings use content-addressed payloads, including non-UTF-8 path bytes.
[[nodiscard]] storage::RecoveryTransaction recoveryTransaction(
    const FileSession &file, const QString &action, const QString &transactionId);

// Reads the fields of a state recoveryTransaction wrote, resolving payload
// references through the store. Every method throws std::runtime_error with
// the reason when a field is missing, malformed or names a missing payload.
class RecoveryRecordDecoder {
public:
    explicit RecoveryRecordDecoder(const storage::RecoveryStore &store) : store_(store) {}
    [[nodiscard]] std::string bytes(const QJsonValue &value) const;
    [[nodiscard]] bool boolean(const QJsonValue &value) const;
    template<typename Enum> [[nodiscard]] Enum enumeration(const QJsonValue &value, int maximum) const
    {
        const auto integer = value.toInt(-1);
        if (!value.isDouble() || value.toDouble() != integer || integer < 0 || integer > maximum)
            throw std::runtime_error("Unsupported recovery enum value.");
        return static_cast<Enum>(integer);
    }
    // The recorded marker count must be a valid integer, but the result
    // always holds zero: FileSession counts the markers in the text again.
    [[nodiscard]] Draft draft(const QJsonValue &value) const;
    // Records written before hand edits were journaled have none.
    [[nodiscard]] std::set<std::string> handEdited(const QJsonValue &value) const;
    // Records without hand edits outside choice regions have no ranges.
    // FileSession checks that they fit the recorded text.
    [[nodiscard]] std::vector<ByteRange> handEditedRanges(const QJsonValue &value) const;
    [[nodiscard]] std::optional<FileSnapshots> snapshots(const QJsonValue &value) const;
    [[nodiscard]] OperationOutcome outcome(const QJsonValue &value) const;
    // A canonical decimal string, as every 64-bit integer is recorded.
    [[nodiscard]] quint64 number(const QJsonValue &value) const;

private:
    [[nodiscard]] qint64 signedNumber(const QJsonValue &value) const;
    [[nodiscard]] TextSnapshot text(const QJsonValue &value) const;
    [[nodiscard]] std::optional<SourceSnapshot> source(const QJsonValue &value) const;
    [[nodiscard]] std::map<std::string, std::string> mapping(const QJsonValue &value) const;
    const storage::RecoveryStore &store_;
};
}
