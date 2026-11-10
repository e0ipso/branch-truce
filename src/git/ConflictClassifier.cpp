// Conflict classification separates editable text conflicts from files that need another tool.
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
void exclude(Conflict &conflict, ConflictSupport support, const Problem &reason)
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
    if (after.outcome != Outcome::Success || after.outputIncomplete || a.worktreeRoot != b.worktreeRoot
        || a.gitDirectory != b.gitDirectory || a.operation != b.operation || a.supported != b.supported
        || a.stage2.commitId != b.stage2.commitId || a.stage3.commitId != b.stage3.commitId
        || a.baseCommitId != b.baseCommitId || a.originalHead != b.originalHead || a.onto != b.onto
        || a.headName != b.headName || a.unsupportedPaths != b.unsupportedPaths
        || before.indexEntries.size() != after.indexEntries.size())
        return false;
    for (qsizetype i = 0; i < before.indexEntries.size(); ++i) {
        const auto &x = before.indexEntries[i];
        const auto &y = after.indexEntries[i];
        if (x.path != y.path || x.stage != y.stage || x.mode != y.mode || x.objectId != y.objectId) return false;
    }
    return true;
}
// Walk exact byte components without following symlinks or opening a FIFO/device.
// The read ceiling is the recovery payload budget. The line limit is applied
// to a complete read before that result becomes editable. A large line count is
// not a failure here; the session holds such a file until the user loads it.
SnapshotFileIdentity fileIdentity(const struct stat &info)
{
    return {static_cast<std::uint64_t>(info.st_dev),        static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::uint32_t>(info.st_mode),       static_cast<std::int64_t>(info.st_size),
            static_cast<std::int64_t>(info.st_mtim.tv_sec), static_cast<std::int64_t>(info.st_mtim.tv_nsec),
            static_cast<std::int64_t>(info.st_ctim.tv_sec), static_cast<std::int64_t>(info.st_ctim.tv_nsec)};
}
enum class ResultRead { Success, UnsupportedTopology, Failed };
ResultRead readResult(const QString &root, const QByteArray &path, QByteArray &bytes, SnapshotFileIdentity &identity)
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
        if (next < 0) return error == ELOOP || error == ENOTDIR ? ResultRead::UnsupportedTopology : ResultRead::Failed;
        fd = next;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        ::close(fd);
        return ResultRead::Failed;
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        return ResultRead::UnsupportedTopology;
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return ResultRead::Failed;
    }
    constexpr qsizetype limit = static_cast<qsizetype>(branch_truce::kMaxRecoveryPayloadBytes);
    bytes = file.read(limit + 1);
    struct stat after{};
    if (file.error() != QFileDevice::NoError || bytes.size() > limit || ::fstat(fd, &after) != 0
        || fileIdentity(info) != fileIdentity(after))
        return ResultRead::Failed;
    identity = fileIdentity(info);
    return ResultRead::Success;
}

Problem textEnvelopeFailure(const QByteArray &bytes)
{
    if (bytes.isEmpty()) return {};
    const auto assessment =
        branch_truce::assessText(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (assessment.limit == branch_truce::TextLimit::Within) return {};
    if (assessment.limit == branch_truce::TextLimit::LineBytes)
        return {ProblemCode::ClassificationLineTooLong,
                {QString::number(static_cast<qulonglong>(assessment.maxLineBytes)),
                 QString::number(static_cast<qulonglong>(branch_truce::kMaxLineBytes))}};
    return {ProblemCode::ClassificationTextTooLarge,
            {QString::number(static_cast<qulonglong>(assessment.bytes)),
             QString::number(static_cast<qulonglong>(branch_truce::kMaxRecoveryPayloadBytes))}};
}

void excludeUnreadResult(Conflict &conflict, ResultRead read)
{
    if (read == ResultRead::UnsupportedTopology) {
        exclude(conflict, ConflictSupport::TopologyConflict, ProblemCode::ClassificationResultTopology);
    } else {
        exclude(conflict, ConflictSupport::LoadFailure,
                {ProblemCode::ClassificationResultUnreadable,
                 {QString::number(static_cast<qulonglong>(branch_truce::kMaxRecoveryPayloadBytes))}});
    }
}
} // namespace

class ConflictClassifier::State {
public:
    State(ConflictClassifier *owner, const GitRunners &runners)
        : q(owner), adapter(runners(owner)), conversion(runners, owner)
    {
        QObject::connect(adapter.get(), &GitRunner::finished, q,
                         [this](const Result &observed) { discovered(observed); });
        QObject::connect(&conversion, &TextConversion::finished, q,
                         [this](const TextConversionResult &observed) { converted(observed); });
    }
    void complete(Outcome outcome, const Problem &problem = {})
    {
        result.outcome = outcome;
        result.problem = problem;
        if (outcome != Outcome::Success) {
            for (auto &conflict : result.conflicts) {
                if (conflict.editable()) exclude(conflict, ConflictSupport::InspectionFailure, problem);
            }
        }
        active = false;
        const auto completed = result;
        Q_EMIT q->finished(completed);
    }
    void discover()
    {
        if (cancelled) {
            complete(Outcome::Cancelled, ProblemCode::ClassificationCancelled);
            return;
        }
        const auto &r = result.request;
        if (!adapter->start({Operation::DiscoverRepository, r.directory, {}, {}, r.sessionGeneration, r.inputRevision}))
            complete(Outcome::ProcessFailure, ProblemCode::ClassificationBusy);
    }
    void discovered(const Result &observed)
    {
        if (cancelled) {
            complete(Outcome::Cancelled, ProblemCode::ClassificationCancelled);
            return;
        }
        if (rechecking) {
            if (!sameSnapshot(snapshot, observed)) {
                complete(Outcome::InspectionFailure, ProblemCode::ClassificationChanged);
                return;
            }
            for (auto &conflict : result.conflicts) {
                if (!conflict.editable()) continue;
                QByteArray current;
                SnapshotFileIdentity identity;
                const auto read = readResult(result.repository.worktreeRoot, conflict.path, current, identity);
                const auto envelope = read == ResultRead::Success ? textEnvelopeFailure(current) : Problem{};
                if (read != ResultRead::Success) {
                    excludeUnreadResult(conflict, read);
                } else if (!envelope.isEmpty()) {
                    current.clear();
                    exclude(conflict, ConflictSupport::LoadFailure, envelope);
                } else if (current != conflict.result->originalBytes || identity != conflict.resultIdentity) {
                    exclude(conflict, ConflictSupport::TopologyConflict, ProblemCode::ClassificationResultChanged);
                } else if (conflict.sources.front().policy.configuration != observed.conversionPolicy.configuration) {
                    exclude(conflict, ConflictSupport::UnsupportedConversion,
                            ProblemCode::ClassificationConfigurationChanged);
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
            const auto reason = Problem{ProblemCode::ClassificationInspectionFailed}.wrapping(observed.problem);
            for (auto &conflict : result.conflicts) exclude(conflict, ConflictSupport::InspectionFailure, reason);
            complete(observed.outcome == Outcome::Success ? Outcome::OutputLimit : observed.outcome, reason);
            return;
        }
        QSet<QByteArray> ambiguous;
        for (const auto &conflict : result.conflicts) {
            auto slash = conflict.path.indexOf('/');
            while (slash >= 0) {
                const auto parent = conflict.path.first(slash);
                if (positions.contains(parent)) {
                    ambiguous.insert(parent);
                    ambiguous.insert(conflict.path);
                }
                slash = conflict.path.indexOf('/', slash + 1);
            }
        }
        for (auto &conflict : result.conflicts) {
            std::array<const IndexEntry *, 4> stages{};
            bool valid = detail::relativeFile(conflict.path);
            bool regular = true;
            for (const auto &entry : conflict.stages) {
                if (entry.stage < 1 || entry.stage > 3 || stages[entry.stage] || !detail::objectId(entry.objectId)) {
                    valid = false;
                    break;
                }
                stages[entry.stage] = &entry;
                regular = regular && (entry.mode == "100644" || entry.mode == "100755");
            }
            if (!valid)
                exclude(conflict, ConflictSupport::InspectionFailure, ProblemCode::ClassificationInvalidStages);
            else if (!regular)
                exclude(conflict, ConflictSupport::UnsupportedType, ProblemCode::ClassificationNonRegular);
            else if (!stages[2] || !stages[3])
                exclude(conflict, ConflictSupport::MissingSide, ProblemCode::ClassificationMissingSide);
            else if (stages[2]->mode != stages[3]->mode)
                exclude(conflict, ConflictSupport::ModeConflict, ProblemCode::ClassificationModeConflict);
            else if (ambiguous.contains(conflict.path))
                exclude(conflict, ConflictSupport::TopologyConflict, ProblemCode::ClassificationTopology);
            else if (result.repository.unsupportedPaths.contains(conflict.path))
                exclude(conflict, ConflictSupport::UnverifiedSource,
                        result.repository.unsupportedPaths.value(conflict.path));
            else if (!result.repository.supported)
                exclude(conflict, ConflictSupport::UnsupportedOperation,
                        Problem{ProblemCode::ClassificationUnsupportedOperation}.wrapping(result.repository.problem));
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
                const auto read =
                    readResult(result.repository.worktreeRoot, conflict.path, launchBytes, launchIdentity);
                if (read == ResultRead::Success) conflict.launchSize = launchIdentity.size;
                const auto envelope = read == ResultRead::Success ? textEnvelopeFailure(launchBytes) : Problem{};
                if (read == ResultRead::Success && envelope.isEmpty()) {
                    source = 0;
                    readSource();
                    return;
                }
                if (!envelope.isEmpty()) {
                    launchBytes.clear();
                    exclude(conflict, ConflictSupport::LoadFailure, envelope);
                } else
                    excludeUnreadResult(conflict, read);
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
        if (!conversion.start(request)) complete(Outcome::ProcessFailure, ProblemCode::ClassificationConversionBusy);
    }
    void converted(const TextConversionResult &observed)
    {
        if (cancelled) {
            complete(Outcome::Cancelled, ProblemCode::ClassificationCancelled);
            return;
        }
        auto &conflict = result.conflicts[offset];
        if (observed.outcome == Outcome::OutputLimit) {
            // Over the size envelope, like a result too large to load, not unreadable.
            exclude(conflict, ConflictSupport::LoadFailure,
                    {ProblemCode::ClassificationStageTooLarge,
                     {QString::number(conflict.stages[source].stage),
                      QString::number(static_cast<qulonglong>(branch_truce::kMaxGitReadBytes))}});
        } else if (observed.outcome != Outcome::Success || !observed.text) {
            const auto support = observed.outcome != Outcome::Rejected       ? ConflictSupport::InspectionFailure
                : observed.failure == TextConversionFailure::UnsupportedText ? ConflictSupport::UnsupportedText
                                                                             : ConflictSupport::UnsupportedConversion;
            exclude(
                conflict, support,
                Problem{ProblemCode::ClassificationStageNotEditable, {QString::number(conflict.stages[source].stage)}}
                    .wrapping(observed.problem));
        } else if (!conflict.sources.isEmpty() && conflict.sources.front().policy != observed.text->policy) {
            exclude(conflict, ConflictSupport::UnsupportedConversion, ProblemCode::ClassificationPoliciesChanged);
        } else {
            conflict.sources.append(*observed.text);
            if (++source < conflict.stages.size()) {
                readSource();
                return;
            }
            const auto decoded = storage::decodeText(
                launchBytes, conflict.sources.front().policy.attributes.value("working-tree-encoding"));
            if (decoded.document) {
                conflict.result = decoded.document;
                conflict.resultIdentity = launchIdentity;
            } else
                exclude(conflict, ConflictSupport::UnsupportedText, decoded.problem);
        }
        ++offset;
        next();
    }
    ConflictClassifier *q;
    std::unique_ptr<GitRunner> adapter;
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

ConflictClassifier::ConflictClassifier(const GitRunners &runners, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, runners))
{
    qRegisterMetaType<ConflictClassificationResult>();
}
ConflictClassifier::ConflictClassifier(GitProgram program, QObject *parent)
    : ConflictClassifier(gitAdapters(std::move(program)), parent)
{}
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
    d->adapter->cancel();
    d->conversion.cancel();
}
bool ConflictClassifier::busy() const { return d->active; }
} // namespace branch_truce::git
