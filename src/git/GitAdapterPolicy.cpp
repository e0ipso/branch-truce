// Operation policy for GitAdapter: which checks each operation needs, its
// fixed Git arguments, and the configuration and attribute policy checks.
#include "GitAdapterState.h"
#include "GitParsing.h"

namespace branch_truce::git {
namespace {
using detail::inspectionLimit;
using detail::parseIndex;
using detail::relativeFile;

bool falseValue(const QByteArray &value)
{
    const auto lower = value.toLower();
    return lower == "false" || lower == "no" || lower == "off" || lower == "0";
}
} // namespace

bool GitAdapter::State::isMutation() const { return result.request.operation == Operation::StageFile; }
bool GitAdapter::State::stagingPolicy() const
{
    return isMutation() || result.request.operation == Operation::CheckParentStaging;
}
bool GitAdapter::State::objectPolicy() const
{
    const auto operation = result.request.operation;
    return operation == Operation::DiscoverRepository
        || operation == Operation::ReadObject || operation == Operation::ReadAttributes
        || operation == Operation::ListUnmerged || operation == Operation::InspectIndex
        || operation == Operation::Status || operation == Operation::MaterializeSource
        || operation == Operation::CanonicalizeText || stagingPolicy();
}
bool GitAdapter::State::filterPolicy() const
{
    return result.request.operation == Operation::Status
        || result.request.operation == Operation::MaterializeSource
        || result.request.operation == Operation::CanonicalizeText || stagingPolicy();
}
QList<QByteArray> GitAdapter::State::indexArguments() const
{
    return {"ls-files", "--stage", "-z", "--", result.request.path};
}
QList<QByteArray> GitAdapter::State::operationArguments() const
{
    const auto &r = result.request;
    switch (r.operation) {
    case Operation::DiscoverRepository:
        return {"rev-parse", "--path-format=absolute", "--show-toplevel", "--absolute-git-dir"};
    case Operation::ListUnmerged:
        return {"ls-files", "--unmerged", "-z"};
    case Operation::Status:
        return {"status", "--porcelain=v2", "-z", "--untracked-files=no", "--ignore-submodules=all"};
    case Operation::InspectIndex:
        return indexArguments();
    case Operation::ReadObject:
        return {"cat-file", "blob", r.objectId};
    case Operation::ReadConfiguration:
        return {"config", "--null", "--list", "--includes"};
    case Operation::ReadAttributes:
        return {"check-attr", "-z", "--all", "--", r.path};
    case Operation::MaterializeSource:
        return {"cat-file", "--filters", "--path=" + r.path, r.objectId};
    case Operation::StageFile:
        return {"add", "--", r.path};
    case Operation::CheckParentStaging:
        return {"check-attr", "-z", "--all", "--", r.path};
    case Operation::CanonicalizeText:
        return {"hash-object", "-w", "--path=" + r.path, "--stdin"};
    }
    return {};
}
bool GitAdapter::State::validIndex(const QByteArray &bytes, bool resolved) const
{
    const auto entries = parseIndex(bytes);
    if (!entries) return false;
    for (const auto &entry : *entries) {
        if ((entry.mode != "100644" && entry.mode != "100755")
            || (resolved && entry.stage != 0) || entry.path != result.request.path) {
            return false;
        }
    }
    return !resolved || entries->size() == 1;
}
bool GitAdapter::State::checkConfiguration()
{
    configuration.clear();
    for (const auto &entry : phaseOutput.split('\0')) {
        if (entry.isEmpty()) {
            continue;
        }
        const auto split = entry.indexOf('\n');
        // Git normalizes section/key case, but driver/remote subsections
        // are case-sensitive. The last value is the effective setting.
        const auto key = entry.left(split);
        const auto value = split < 0 ? QByteArray{} : entry.mid(split + 1);
        configuration.insert(key, value);
    }
    for (auto entry = configuration.cbegin(); entry != configuration.cend(); ++entry) {
        const auto &key = entry.key();
        const auto &value = entry.value();
        if (key == "core.autocrlf" || key == "core.eol" || key == "core.safecrlf"
            || key == "core.checkroundtripencoding" || key == "core.filemode" || key == "core.attributesfile"
            || (key.startsWith("filter.") && (key.endsWith(".clean") || key.endsWith(".smudge")
                || key.endsWith(".process") || key.endsWith(".required")))) {
            result.conversionPolicy.configuration.insert(key, value);
        }
        if ((objectPolicy() && (key == "extensions.partialclone"
                         || (key.startsWith("remote.") && key.endsWith(".promisor") && !falseValue(value))))
            || (stagingPolicy() && key == "core.fsmonitor" && !falseValue(value))) {
            result.diagnostic = QStringLiteral("Outside app: unsupported partial repository or filesystem monitor setting: ")
                + QString::fromUtf8(key);
            return false;
        }
    }
    return true;
}
bool GitAdapter::State::hasFilterConfiguration() const
{
    for (auto entry = result.conversionPolicy.configuration.cbegin();
         entry != result.conversionPolicy.configuration.cend(); ++entry) {
        if (entry.key().startsWith("filter.")) return true;
    }
    return false;
}
bool GitAdapter::State::checkFilter(const QByteArray &path, const QByteArray &driver)
{
    // set/unset can be literal driver values, so a matching helper is denied.
    const auto prefix = "filter." + driver;
    if (!configuration.value(prefix + ".clean").isEmpty()
        || !configuration.value(prefix + ".smudge").isEmpty()
        || !configuration.value(prefix + ".process").isEmpty()
        || (configuration.contains(prefix + ".required") && !falseValue(configuration.value(prefix + ".required")))) {
        result.diagnostic = QStringLiteral("Outside app: custom filter '%1' is unsupported for %2")
            .arg(displayPath(driver), displayPath(path));
        return false;
    }
    return true;
}
bool GitAdapter::State::consumeStatusAttributes(const QByteArray &chunk)
{
    phaseOutput += chunk;
    qsizetype consumed = 0;
    for (;;) {
        const auto pathEnd = phaseOutput.indexOf('\0', consumed);
        if (pathEnd < 0) break;
        const auto nameEnd = phaseOutput.indexOf('\0', pathEnd + 1);
        if (nameEnd < 0) break;
        const auto valueEnd = phaseOutput.indexOf('\0', nameEnd + 1);
        if (valueEnd < 0) break;
        const auto path = phaseOutput.mid(consumed, pathEnd - consumed);
        const auto name = phaseOutput.mid(pathEnd + 1, nameEnd - pathEnd - 1);
        if (!relativeFile(path) || name.isEmpty()) {
            result.diagnostic = QStringLiteral("Outside app: invalid filter attribute inspection");
            return false;
        }
        if (name == "filter" && !checkFilter(path, phaseOutput.mid(nameEnd + 1, valueEnd - nameEnd - 1))) return false;
        consumed = valueEnd + 1;
    }
    phaseOutput.remove(0, consumed);
    // Bound an unfinished record, rather than the complete policy listing.
    if (phaseOutput.size() > inspectionLimit) {
        result.diagnostic = QStringLiteral("Outside app: oversized filter attribute record");
        return false;
    }
    return true;
}
bool GitAdapter::State::captureAttributes()
{
    const auto fields = phaseOutput.split('\0');
    if (fields.isEmpty() || !fields.back().isEmpty() || (fields.size() - 1) % 3 != 0) return false;
    result.conversionPolicy.attributes.clear();
    for (qsizetype i = 0; i + 2 < fields.size(); i += 3) {
        if (fields[i] != result.request.path || fields[i + 1].isEmpty()) return false;
        const auto &name = fields[i + 1];
        if (name == "text" || name == "eol" || name == "ident" || name == "working-tree-encoding" || name == "filter") {
            if (result.conversionPolicy.attributes.contains(name)) return false;
            result.conversionPolicy.attributes.insert(name, fields[i + 2]);
        }
    }
    return true;
}
bool GitAdapter::State::checkFilters()
{
    if (result.request.operation == Operation::Status) {
        if (phaseOutput.isEmpty()) return true;
        result.diagnostic = QStringLiteral("Outside app: invalid filter attribute inspection");
        return false;
    }
    if (!captureAttributes()) {
        result.diagnostic = QStringLiteral("Outside app: invalid conversion attribute inspection");
        return false;
    }
    const auto fields = phaseOutput.split('\0');
    if (fields.isEmpty() || !fields.back().isEmpty() || (fields.size() - 1) % 3 != 0) {
        result.diagnostic = QStringLiteral("Outside app: invalid filter attribute inspection");
        return false;
    }
    for (qsizetype i = 0; i + 2 < fields.size(); i += 3) {
        if (!relativeFile(fields[i]) || fields[i] != result.request.path) {
            result.diagnostic = QStringLiteral("Outside app: filter attributes do not match the requested path");
            return false;
        }
        if (fields[i + 1] != "filter") continue;
        if (!checkFilter(fields[i], fields[i + 2])) return false;
    }
    return true;
}
} // namespace branch_truce::git
