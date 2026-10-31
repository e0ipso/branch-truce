// The Git adapter inspects repository state and performs guarded operations for conflict resolution.
#include "GitAdapter.h"
#include "GitAdapterState.h"
#include "GitParsing.h"

#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>

namespace branch_truce::git {
namespace {
using detail::deadlineMs;
using detail::exactPath;
using detail::objectId;
using detail::outputLimit;
using detail::relativeFile;
QMutex mutationMutex;
QSet<QString> mutatingGitDirectories;
} // namespace

QString displayPath(const QByteArray &path)
{
    const auto decoded = QString::fromUtf8(path);
    const bool utf8 = decoded.toUtf8() == path;
    QByteArray label;
    for (const char value : path) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte == '\\') label += "\\\\";
        else if (byte == '\n') label += "\\n";
        else if (byte == '\r') label += "\\r";
        else if (byte == '\t') label += "\\t";
        else if (byte < 0x20 || byte == 0x7f || (!utf8 && byte >= 0x80)) {
            label += "\\x" + QByteArray(1, value).toHex();
        } else label += value;
    }
    return QString::fromUtf8(label);
}

void GitAdapter::State::begin()
{
    const auto &r = result.request;
    directory = QFileInfo(r.directory).canonicalFilePath();
    if (!executableValid || directory.isEmpty() || !QDir(directory).exists()
        || !QDir::isAbsolutePath(r.directory) || !exactPath(r.directory)
        || executable.startsWith(directory + QLatin1Char('/'))) {
        reject(QStringLiteral("An absolute directory and trusted executable outside it are required"));
        return;
    }
    if (operationArguments().isEmpty()) {
        reject(QStringLiteral("Unknown Git operation"));
        return;
    }
    const bool pathRequired = r.operation == Operation::InspectIndex || r.operation == Operation::ReadAttributes
        || r.operation == Operation::MaterializeSource || r.operation == Operation::CanonicalizeText || stagingPolicy();
    const bool objectRequired = r.operation == Operation::ReadObject || r.operation == Operation::MaterializeSource;
    if ((pathRequired && !relativeFile(r.path)) || (!pathRequired && !r.path.isEmpty())
        || (objectRequired && !objectId(r.objectId)) || (!objectRequired && !r.objectId.isEmpty())
        || (r.operation != Operation::CanonicalizeText && !r.input.isEmpty())) {
        reject(QStringLiteral("Invalid operands for the named Git operation"));
        return;
    }
    if (r.input.size() > outputLimit) {
        reject(QStringLiteral("Canonicalization input has %1 bytes, exceeding the %2 byte input limit")
            .arg(r.input.size()).arg(outputLimit));
        return;
    }
    deadline.start(deadlineMs);
    launch(Phase::Version, {"--version"});
}
void GitAdapter::State::complete()
{
    deadline.stop();
    escalation.stop();
    releaseMutation();
    if (temporaryObjects) {
        if (!temporaryObjects->remove()) {
            if (result.outcome == Outcome::Success) result.outcome = Outcome::ProcessFailure;
            result.diagnostic += QStringLiteral("; could not remove private conversion objects: ") + temporaryObjects->path();
        }
        temporaryObjects.reset();
    }
    if (result.request.operation == Operation::DiscoverRepository && result.outcome != Outcome::Success) {
        result.repository.operation = RepositoryOperation::Unsupported;
        result.repository.supported = false;
        if (result.repository.diagnostic.isEmpty()) result.repository.diagnostic = result.diagnostic;
    }
    const auto completed = result;
    active = false;
    Q_EMIT q->finished(completed);
}
void GitAdapter::State::reject(const QString &message)
{
    result.outcome = Outcome::Rejected;
    result.diagnostic = message;
    complete();
}
void GitAdapter::State::reserveMutation(const QString &key)
{
    // On success mutationKey holds the reservation until complete() releases it.
    const QMutexLocker lock(&mutationMutex);
    if (mutatingGitDirectories.contains(key)) {
        result.diagnostic = QStringLiteral("Another app-owned mutation is active for this worktree");
    } else {
        mutationKey = key;
        mutatingGitDirectories.insert(key);
    }
}
void GitAdapter::State::releaseMutation()
{
    if (!mutationKey.isEmpty()) {
        const QMutexLocker lock(&mutationMutex);
        mutatingGitDirectories.remove(mutationKey);
        mutationKey.clear();
    }
}

GitAdapter::GitAdapter(const QString &executable, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, executable))
{
    qRegisterMetaType<Result>();
}
GitAdapter::~GitAdapter() = default;
bool GitAdapter::start(const Request &request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->active) {
        return false;
    }
    d->result = Result{};
    d->discovery.reset();
    d->result.request = request;
    d->bytesRead = 0;
    d->stopping = false;
    d->mutationStarted = false;
    d->active = true;
    QTimer::singleShot(0, this, [this] {
        if (d->stopping) {
            d->complete();
        } else {
            d->begin();
        }
    });
    return true;
}
void GitAdapter::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active) {
        return;
    }
    d->result.cancellationRequested = true;
    if (d->phase == State::Phase::Inspection && d->mutationStarted) {
        // Keep the completed operation's outcome and diagnostic while bounded
        // reinspection determines the observed index mutation.
        return;
    }
    d->stop(Outcome::Cancelled, QStringLiteral("Git request cancelled"));
}
bool GitAdapter::busy() const { return d->active; }
} // namespace branch_truce::git
