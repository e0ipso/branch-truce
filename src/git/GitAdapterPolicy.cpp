// Operation policy for GitAdapter: what each operation takes, checks and runs,
// and the configuration and attribute policy checks.
#include "GitAdapterState.h"
#include "GitParsing.h"

#include <array>

namespace branch_truce::git {
namespace {
using detail::Flow;
using detail::OperationSpec;
using detail::Output;
using detail::parseIndex;
using detail::relativeFile;

bool falseValue(const QByteArray &value)
{
    const auto lower = value.toLower();
    return lower == "false" || lower == "no" || lower == "off" || lower == "0";
}
} // namespace

const OperationSpec *detail::operationSpec(Operation operation)
{
    using R = const Request &;
    // In the order of the Operation enum.
    static const std::array<OperationSpec, 8> specs{{
        // DiscoverRepository
        {.flow = Flow::Discovery,
         .arguments = [](R) -> QList<QByteArray> {
             return {"rev-parse", "--path-format=absolute", "--show-toplevel", "--absolute-git-dir"};
         }},
        // InspectIndex
        {.path = true,
         .output = Output::IndexEntries,
         .arguments = [](R r) -> QList<QByteArray> { return indexArguments(r.path); }},
        // ReadObject
        {.object = true, .arguments = [](R r) -> QList<QByteArray> { return {"cat-file", "blob", r.objectId}; }},
        // ReadAttributes
        {.path = true,
         .output = Output::Attributes,
         .arguments = [](R r) -> QList<QByteArray> { return {"check-attr", "-z", "--all", "--", r.path}; }},
        // MaterializeSource
        {.path = true,
         .object = true,
         .filters = true,
         .arguments = [](R r) -> QList<QByteArray> {
             return {"cat-file", "--filters", "--path=" + r.path, r.objectId};
         }},
        // StageFile
        {.path = true,
         .filters = true,
         .staging = true,
         .flow = Flow::Stage,
         .arguments = [](R r) -> QList<QByteArray> { return {"add", "--", r.path}; }},
        // CheckParentStaging: its filter check reads the path's attributes; nothing else runs.
        {.path = true,
         .filters = true,
         .staging = true,
         .flow = Flow::Preflight,
         .arguments = [](R r) -> QList<QByteArray> { return {"check-attr", "-z", "--all", "--", r.path}; }},
        // CanonicalizeText
        {.path = true,
         .input = true,
         .filters = true,
         .flow = Flow::Canonicalize,
         .output = Output::CanonicalObject,
         .arguments = [](R r) -> QList<QByteArray> { return {"hash-object", "-w", "--path=" + r.path, "--stdin"}; }},
    }};
    const auto index = static_cast<std::size_t>(operation);
    return index < specs.size() ? &specs[index] : nullptr;
}
QList<QByteArray> detail::indexArguments(const QByteArray &path) { return {"ls-files", "--stage", "-z", "--", path}; }

bool isMutation(Operation operation)
{
    const auto *spec = detail::operationSpec(operation);
    return spec && spec->flow == detail::Flow::Stage;
}

bool GitAdapter::State::validIndex(const QByteArray &bytes, bool resolved) const
{
    const auto entries = parseIndex(bytes);
    if (!entries) return false;
    for (const auto &entry : *entries) {
        if ((entry.mode != "100644" && entry.mode != "100755") || (resolved && entry.stage != 0)
            || entry.path != result.request.path) {
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
            || (key.startsWith("filter.")
                && (key.endsWith(".clean") || key.endsWith(".smudge") || key.endsWith(".process")
                    || key.endsWith(".required")))) {
            result.conversionPolicy.configuration.insert(key, value);
        }
        // Every operation reads objects, and none may fetch one lazily.
        if (key == "extensions.partialclone"
            || (key.startsWith("remote.") && key.endsWith(".promisor") && !falseValue(value))
            || (spec->staging && key == "core.fsmonitor" && !falseValue(value))) {
            result.problem = {ProblemCode::GitPartialOrMonitor, {QString::fromUtf8(key)}};
            return false;
        }
    }
    return true;
}
bool GitAdapter::State::checkFilter(const QByteArray &path, const QByteArray &driver)
{
    // set/unset can be literal driver values, so a matching helper is denied.
    const auto prefix = "filter." + driver;
    if (!configuration.value(prefix + ".clean").isEmpty() || !configuration.value(prefix + ".smudge").isEmpty()
        || !configuration.value(prefix + ".process").isEmpty()
        || (configuration.contains(prefix + ".required") && !falseValue(configuration.value(prefix + ".required")))) {
        result.problem = {ProblemCode::GitCustomFilter, {displayPath(driver), displayPath(path)}};
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
        if (name == "text" || name == "eol" || name == "ident" || name == "working-tree-encoding" || name == "filter"
            || name == "conflict-marker-size") {
            if (result.conversionPolicy.attributes.contains(name)) return false;
            result.conversionPolicy.attributes.insert(name, fields[i + 2]);
        }
    }
    return true;
}
bool GitAdapter::State::checkFilters()
{
    if (!captureAttributes()) {
        result.problem = ProblemCode::GitAttributeInspection;
        return false;
    }
    const auto fields = phaseOutput.split('\0');
    if (fields.isEmpty() || !fields.back().isEmpty() || (fields.size() - 1) % 3 != 0) {
        result.problem = ProblemCode::GitFilterAttributes;
        return false;
    }
    for (qsizetype i = 0; i + 2 < fields.size(); i += 3) {
        if (!relativeFile(fields[i]) || fields[i] != result.request.path) {
            result.problem = ProblemCode::GitFilterPath;
            return false;
        }
        if (fields[i + 1] != "filter") continue;
        if (!checkFilter(fields[i], fields[i + 2])) return false;
    }
    return true;
}
} // namespace branch_truce::git
