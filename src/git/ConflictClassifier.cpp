#include "ConflictClassifier.h"
#include "GitParsing.h"
#include "core/ResourceLimits.h"

#include <QFile>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace branch_truce::git {
namespace {
void exclude(Conflict &conflict, ConflictSupport support, const QString &reason)
{
    conflict.support = support;
    conflict.reason = reason;
    conflict.sources.clear();
    conflict.result.reset();
    conflict.resultIdentity.reset();
}
bool sameSnapshot(const Result &before, const Result &after)
{
    const auto &a = before.repository;
    const auto &b = after.repository;
    if (after.outcome != Outcome::Success || after.outputIncomplete
        || a.worktreeRoot != b.worktreeRoot || a.gitDirectory != b.gitDirectory
        || a.operation != b.operation || a.supported != b.supported
        || a.stage2.commitId != b.stage2.commitId || a.stage3.commitId != b.stage3.commitId
        || a.baseCommitId != b.baseCommitId || a.originalHead != b.originalHead
        || a.onto != b.onto || a.headName != b.headName || a.unsupportedPaths != b.unsupportedPaths
        || before.indexEntries.size() != after.indexEntries.size()) return false;
    for (qsizetype i = 0; i < before.indexEntries.size(); ++i) {
        const auto &x = before.indexEntries[i];
        const auto &y = after.indexEntries[i];
        if (x.path != y.path || x.stage != y.stage || x.mode != y.mode || x.objectId != y.objectId) return false;
    }
    return true;
}
// Walk exact byte components without following symlinks or opening a FIFO/device.
// The read ceiling is the recovery payload budget. Line and file limits are
// applied to a complete read before that result becomes editable.
SnapshotFileIdentity fileIdentity(const struct stat &info)
{
    return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
        static_cast<std::uint32_t>(info.st_mode), static_cast<std::int64_t>(info.st_size),
        static_cast<std::int64_t>(info.st_mtim.tv_sec), static_cast<std::int64_t>(info.st_mtim.tv_nsec),
        static_cast<std::int64_t>(info.st_ctim.tv_sec), static_cast<std::int64_t>(info.st_ctim.tv_nsec)};
}
enum class ResultRead { Success, UnsupportedTopology, Failed };
ResultRead readResult(const QString &root, const QByteArray &path, QByteArray &bytes,
                      SnapshotFileIdentity &identity)
{
    int fd = ::open(QFile::encodeName(root).constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return ResultRead::Failed;
    const auto parts = path.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? O_NONBLOCK : O_DIRECTORY);
        const int next = ::openat(fd, parts[i].constData(), flags);
        const int error = errno;
        ::close(fd);
        if (next < 0) return error == ELOOP || error == ENOTDIR
            ? ResultRead::UnsupportedTopology : ResultRead::Failed;
        fd = next;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) { ::close(fd); return ResultRead::Failed; }
    if (!S_ISREG(info.st_mode)) { ::close(fd); return ResultRead::UnsupportedTopology; }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); return ResultRead::Failed; }
    constexpr qsizetype limit = static_cast<qsizetype>(branch_truce::kMaxRecoveryPayloadBytes);
    bytes = file.read(limit + 1);
    struct stat after {};
    if (file.error() != QFileDevice::NoError || bytes.size() > limit
        || ::fstat(fd, &after) != 0 || fileIdentity(info) != fileIdentity(after)) return ResultRead::Failed;
    identity = fileIdentity(info);
    return ResultRead::Success;
}

QString textEnvelopeFailure(const QByteArray &bytes)
{
    if (bytes.isEmpty()) return {};
    const auto assessment = branch_truce::assessText(
        std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (assessment.limit == branch_truce::TextLimit::Within) return {};
    return QString::fromStdString("Load error: " + branch_truce::textLimitDiagnostic(assessment)
        + "; the captured file was not loaded");
}

void excludeUnreadResult(Conflict &conflict, ResultRead read)
{
    if (read == ResultRead::UnsupportedTopology) {
        exclude(conflict, ConflictSupport::TopologyConflict,
            QStringLiteral("Outside app: current result must be a regular file without symlink traversal"));
    } else {
        exclude(conflict, ConflictSupport::LoadFailure,
            QStringLiteral("Load error: current result could not be read completely and stably within the 8388608 byte limit; check the file and reopen the session"));
    }
}
} // namespace

class ConflictClassifier::State {
public:
    State(ConflictClassifier *owner, const QString &executable)
        : q(owner), adapter(executable, owner), conversion(executable, owner)
    {
        QObject::connect(&adapter, &GitAdapter::finished, q, [this](const Result &observed) { discovered(observed); });
        QObject::connect(&conversion, &TextConversion::finished, q, [this](const TextConversionResult &observed) { converted(observed); });
    }
    void complete(Outcome outcome, const QString &diagnostic = {})
    {
        result.outcome = outcome;
        result.diagnostic = diagnostic;
        if (outcome != Outcome::Success) {
            for (auto &conflict : result.conflicts) {
                if (conflict.editable()) exclude(conflict, ConflictSupport::InspectionFailure, diagnostic);
            }
        }
        active = false;
        const auto completed = result;
        Q_EMIT q->finished(completed);
    }
    void discover()
    {
        if (cancelled) { complete(Outcome::Cancelled, QStringLiteral("Outside app: conflict inspection cancelled")); return; }
        const auto &r = result.request;
        if (!adapter.start({Operation::DiscoverRepository, r.directory, {}, {}, r.sessionGeneration, r.inputRevision}))
            complete(Outcome::ProcessFailure, QStringLiteral("Outside app: conflict discovery was unexpectedly busy"));
    }
    void discovered(const Result &observed)
    {
        if (cancelled) { complete(Outcome::Cancelled, QStringLiteral("Outside app: conflict inspection cancelled")); return; }
        if (rechecking) {
            if (!sameSnapshot(snapshot, observed)) {
                complete(Outcome::InspectionFailure, QStringLiteral("Outside app: operation or index changed during classification; inspect again"));
                return;
            }
            for (auto &conflict : result.conflicts) {
                if (!conflict.editable()) continue;
                QByteArray current;
                SnapshotFileIdentity identity;
                const auto read = readResult(result.repository.worktreeRoot, conflict.path, current, identity);
                const auto envelope = read == ResultRead::Success ? textEnvelopeFailure(current) : QString{};
                if (read != ResultRead::Success) {
                    excludeUnreadResult(conflict, read);
                } else if (!envelope.isEmpty()) {
                    current.clear();
                    exclude(conflict, ConflictSupport::LoadFailure, envelope);
                } else if (current != conflict.result->originalBytes || identity != conflict.resultIdentity) {
                    exclude(conflict, ConflictSupport::TopologyConflict,
                        QStringLiteral("Outside app: current result changed or became unreadable during classification; inspect again"));
                } else if (conflict.sources.front().policy.configuration != observed.conversionPolicy.configuration) {
                    exclude(conflict, ConflictSupport::UnsupportedConversion,
                        QStringLiteral("Outside app: conversion configuration changed during classification; inspect again"));
                }
            }
            complete(Outcome::Success);
            return;
        }
        snapshot = observed;
        result.repository = observed.repository;
        QHash<QByteArray, qsizetype> positions;
        for (const auto &entry : observed.indexEntries) {
            if (!positions.contains(entry.path)) {
                positions.insert(entry.path, result.conflicts.size());
                Conflict conflict;
                conflict.path = entry.path;
                result.conflicts.append(conflict);
            }
            result.conflicts[positions.value(entry.path)].stages.append(entry);
        }
        if (observed.outcome != Outcome::Success || observed.outputIncomplete) {
            const auto reason = QStringLiteral("Outside app: repository inspection failed: ") + observed.diagnostic;
            for (auto &conflict : result.conflicts) exclude(conflict, ConflictSupport::InspectionFailure, reason);
            complete(observed.outcome == Outcome::Success ? Outcome::OutputLimit : observed.outcome, reason);
            return;
        }
        QSet<QByteArray> ambiguous;
        for (const auto &conflict : result.conflicts) {
            auto slash = conflict.path.indexOf('/');
            while (slash >= 0) {
                const auto parent = conflict.path.first(slash);
                if (positions.contains(parent)) { ambiguous.insert(parent); ambiguous.insert(conflict.path); }
                slash = conflict.path.indexOf('/', slash + 1);
            }
        }
        for (auto &conflict : result.conflicts) {
            std::array<const IndexEntry *, 4> stages{};
            bool valid = detail::relativeFile(conflict.path);
            bool regular = true;
            for (const auto &entry : conflict.stages) {
                if (entry.stage < 1 || entry.stage > 3 || stages[entry.stage] || !detail::objectId(entry.objectId)) { valid = false; break; }
                stages[entry.stage] = &entry;
                regular = regular && (entry.mode == "100644" || entry.mode == "100755");
            }
            if (!valid) exclude(conflict, ConflictSupport::InspectionFailure, QStringLiteral("Outside app: invalid or duplicate conflict stages"));
            else if (!regular) exclude(conflict, ConflictSupport::UnsupportedType, QStringLiteral("Outside app: symlink, submodule or other non-regular source type requires external resolution"));
            else if (!stages[2] || !stages[3]) exclude(conflict, ConflictSupport::MissingSide, QStringLiteral("Outside app: missing stage 2 or 3 requires a deletion or destination decision"));
            else if (stages[2]->mode != stages[3]->mode) exclude(conflict, ConflictSupport::ModeConflict, QStringLiteral("Outside app: stages 2 and 3 disagree on the executable mode"));
            else if (ambiguous.contains(conflict.path)) exclude(conflict, ConflictSupport::TopologyConflict, QStringLiteral("Outside app: conflicting file and directory paths require a topology decision"));
            else if (result.repository.unsupportedPaths.contains(conflict.path)) exclude(conflict, ConflictSupport::UnverifiedSource, result.repository.unsupportedPaths.value(conflict.path));
            else if (!result.repository.supported) exclude(conflict, ConflictSupport::UnsupportedOperation, QStringLiteral("Outside app: ") + result.repository.diagnostic);
            else {
                conflict.mode = stages[2]->mode;
                conflict.support = ConflictSupport::SupportedText;
            }
        }
        next();
    }
    void next()
    {
        while (offset < result.conflicts.size()) {
            auto &conflict = result.conflicts[offset];
            if (conflict.editable()) {
                const auto read = readResult(result.repository.worktreeRoot, conflict.path, launchBytes, launchIdentity);
                const auto envelope = read == ResultRead::Success ? textEnvelopeFailure(launchBytes) : QString{};
                if (read == ResultRead::Success && envelope.isEmpty()) {
                    source = 0;
                    readSource();
                    return;
                }
                if (!envelope.isEmpty()) {
                    launchBytes.clear();
                    exclude(conflict, ConflictSupport::LoadFailure, envelope);
                } else excludeUnreadResult(conflict, read);
            }
            ++offset;
        }
        rechecking = true;
        discover();
    }
    void readSource()
    {
        auto &conflict = result.conflicts[offset];
        const auto &entry = conflict.stages[source];
        TextConversionRequest request;
        request.directory = result.repository.worktreeRoot;
        request.path = conflict.path;
        request.objectId = entry.objectId;
        request.sourceStage = entry.stage;
        request.stages = conflict.stages;
        request.sessionGeneration = result.request.sessionGeneration;
        request.inputRevision = result.request.inputRevision;
        if (!conversion.start(request)) complete(Outcome::ProcessFailure, QStringLiteral("Outside app: source conversion was unexpectedly busy"));
    }
    void converted(const TextConversionResult &observed)
    {
        if (cancelled) { complete(Outcome::Cancelled, QStringLiteral("Outside app: conflict inspection cancelled")); return; }
        auto &conflict = result.conflicts[offset];
        if (observed.outcome != Outcome::Success || !observed.text) {
            const auto support = observed.outcome != Outcome::Rejected ? ConflictSupport::InspectionFailure
                : observed.failure == TextConversionFailure::UnsupportedText ? ConflictSupport::UnsupportedText
                : ConflictSupport::UnsupportedConversion;
            exclude(conflict, support, QStringLiteral("Outside app: stage %1 cannot be edited: %2").arg(conflict.stages[source].stage).arg(observed.diagnostic));
        } else if (!conflict.sources.isEmpty() && conflict.sources.front().policy != observed.text->policy) {
            exclude(conflict, ConflictSupport::UnsupportedConversion, QStringLiteral("Outside app: source conversion policies changed during classification"));
        } else {
            conflict.sources.append(*observed.text);
            if (++source < conflict.stages.size()) { readSource(); return; }
            const auto decoded = storage::decodeText(launchBytes, conflict.sources.front().policy.attributes.value("working-tree-encoding"));
            if (decoded.document) {
                conflict.result = decoded.document;
                conflict.resultIdentity = launchIdentity;
            }
            else exclude(conflict, ConflictSupport::UnsupportedText, decoded.diagnostic);
        }
        ++offset;
        next();
    }
    ConflictClassifier *q;
    GitAdapter adapter;
    TextConversion conversion;
    ConflictClassificationResult result;
    Result snapshot;
    QByteArray launchBytes;
    SnapshotFileIdentity launchIdentity;
    qsizetype offset = 0;
    qsizetype source = 0;
    bool active = false;
    bool cancelled = false;
    bool rechecking = false;
};

ConflictClassifier::ConflictClassifier(const QString &executable, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, executable))
{
    qRegisterMetaType<ConflictClassificationResult>();
}
ConflictClassifier::~ConflictClassifier() = default;
bool ConflictClassifier::start(const ConflictClassificationRequest &request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->active) return false;
    d->result = {};
    d->result.request = request;
    d->snapshot = {};
    d->launchBytes.clear();
    d->offset = 0;
    d->source = 0;
    d->cancelled = false;
    d->rechecking = false;
    d->active = true;
    QTimer::singleShot(0, this, [this] { d->discover(); });
    return true;
}
void ConflictClassifier::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active) return;
    d->cancelled = true;
    d->adapter.cancel();
    d->conversion.cancel();
}
bool ConflictClassifier::busy() const { return d->active; }
} // namespace branch_truce::git
