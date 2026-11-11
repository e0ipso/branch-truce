// The Git adapter inspects repository state and performs guarded operations for conflict resolution.
#include "GitAdapter.h"
#include "GitAdapterState.h"
#include "GitParsing.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QThread>
#include <optional>

namespace branch_truce::git {
namespace {
using detail::exactPath;
using detail::objectId;
using detail::outputLimit;
using detail::relativeFile;
} // namespace

QString displayPath(const QByteArray &path)
{
    const auto decoded = QString::fromUtf8(path);
    const bool utf8 = decoded.toUtf8() == path;
    QByteArray label;
    for (const char value : path) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte == '\\')
            label += "\\\\";
        else if (byte == '\n')
            label += "\\n";
        else if (byte == '\r')
            label += "\\r";
        else if (byte == '\t')
            label += "\\t";
        else if (byte < 0x20 || byte == 0x7f || (!utf8 && byte >= 0x80)) {
            label += "\\x" + QByteArray(1, value).toHex();
        } else
            label += value;
    }
    return QString::fromUtf8(label);
}

void GitAdapter::State::begin()
{
    const auto &r = result.request;
    directory = QFileInfo(r.directory).canonicalFilePath();
    if (!executableValid || directory.isEmpty() || !QDir(directory).exists() || !QDir::isAbsolutePath(r.directory)
        || !exactPath(r.directory) || executable.startsWith(directory + QLatin1Char('/'))) {
        reject(ProblemCode::GitInvalidRequest);
        return;
    }
    spec = detail::operationSpec(r.operation);
    if (!spec) {
        reject(ProblemCode::GitUnknownOperation);
        return;
    }
    if ((spec->path && !relativeFile(r.path)) || (!spec->path && !r.path.isEmpty())
        || (spec->object && !objectId(r.objectId)) || (!spec->object && !r.objectId.isEmpty())
        || (!spec->input && !r.input.isEmpty())) {
        reject(ProblemCode::GitInvalidOperands);
        return;
    }
    if (r.input.size() > outputLimit) {
        reject({ProblemCode::GitInputTooLarge, {QString::number(r.input.size()), QString::number(outputLimit)}});
        return;
    }
    deadline.start(deadlineMs);
    launch(Phase::Version, {"--version"});
}
void GitAdapter::State::complete()
{
    deadline.stop();
    escalation.stop();
    if (temporaryObjects) {
        if (!temporaryObjects->remove()) {
            if (result.outcome == Outcome::Success) result.outcome = Outcome::ProcessFailure;
            addProblem({ProblemCode::GitCleanupFailed, {temporaryObjects->path()}});
        }
        temporaryObjects.reset();
    }
    if (result.request.operation == Operation::DiscoverRepository && result.outcome != Outcome::Success) {
        result.repository.operation = RepositoryOperation::Unsupported;
        result.repository.supported = false;
        if (result.repository.problem.isEmpty()) result.repository.problem = result.problem;
    }
    const auto completed = result;
    active = false;
    Q_EMIT q->finished(completed);
}
void GitAdapter::State::reject(Problem problem)
{
    result.outcome = Outcome::Rejected;
    result.problem = std::move(problem);
    complete();
}
void GitAdapter::State::addProblem(Problem problem)
{
    result.problem = result.problem.isEmpty() ? std::move(problem) : problem.wrapping(std::move(result.problem));
}
GitAdapter::GitAdapter(GitProgram program, QObject *parent)
    : GitRunner(parent), d(std::make_unique<State>(this, std::move(program)))
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
    d->spec = nullptr;
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
    d->stop(Outcome::Cancelled, ProblemCode::GitCancelled);
}
bool GitAdapter::busy() const { return d->active; }

GitRunners gitAdapters(GitProgram program)
{
    return [program = std::move(program)](QObject *parent) -> std::unique_ptr<GitRunner> {
        return std::make_unique<GitAdapter>(program, parent);
    };
}

Result runBlocking(const GitRunners &runners, const Request &request)
{
    const auto runner = runners(nullptr);
    QEventLoop loop;
    std::optional<Result> result;
    QObject::connect(runner.get(), &GitRunner::finished, &loop, [&](const Result &value) {
        result = value;
        loop.quit();
    });
    if (!runner->start(request)) {
        Result refused;
        refused.request = request;
        refused.problem = ProblemCode::GitStartRefused;
        return refused;
    }
    // Every accepted request completes within the runner's deadline.
    loop.exec();
    return result.value_or(Result{});
}

Result runBlocking(const GitProgram &program, const Request &request)
{
    return runBlocking(gitAdapters(program), request);
}
} // namespace branch_truce::git
