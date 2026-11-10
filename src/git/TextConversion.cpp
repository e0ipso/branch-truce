// Text conversion checks Git attributes and prepares source or result bytes under the captured policy.
#include "TextConversion.h"
#include "GitParsing.h"

#include <QThread>
#include <QTimer>
#include <array>

namespace branch_truce::git {
namespace {
bool attributeAbsent(const QByteArray &value) { return value.isEmpty() || value == "unspecified" || value == "unset"; }
bool supportedPolicy(const ConversionPolicy &policy, Problem &problem)
{
    const auto text = policy.attributes.value("text");
    const auto ident = policy.attributes.value("ident");
    const auto eol = policy.attributes.value("eol");
    if ((!attributeAbsent(text) && text != "set" && text != "auto") || (!attributeAbsent(ident) && ident != "set")
        || (!attributeAbsent(eol) && eol != "lf" && eol != "crlf") || (text == "unset" && !attributeAbsent(eol))) {
        problem = ProblemCode::ConversionUnsupportedAttributes;
        return false;
    }
    return true;
}
bool captureMode(const TextConversionRequest &request, ConvertedText &text, Problem &problem)
{
    std::array<const IndexEntry *, 4> stages{};
    for (const auto &entry : request.stages) {
        if (entry.path != request.path || entry.stage < 1 || entry.stage > 3 || stages[entry.stage]
            || (entry.mode != "100644" && entry.mode != "100755") || !detail::objectId(entry.objectId)) {
            problem = ProblemCode::ConversionStageEntries;
            return false;
        }
        stages[entry.stage] = &entry;
    }
    if (!stages[2] || !stages[3] || stages[2]->mode != stages[3]->mode) {
        problem = ProblemCode::ConversionModeDisagrees;
        return false;
    }
    text.mode = stages[2]->mode;
    if (request.operation == TextOperation::ReadSource) {
        if (request.sourceStage < 1 || request.sourceStage > 3 || !stages[request.sourceStage]
            || stages[request.sourceStage]->objectId != request.objectId) {
            problem = ProblemCode::ConversionSourceIdentity;
            return false;
        }
        text.sourceMode = stages[request.sourceStage]->mode;
    }
    return true;
}
} // namespace

class TextConversion::State {
public:
    enum class Step {
        Attributes,
        Source,
        RawSource,
        Canonical,
        Index,
        PublishedRaw,
        PublishedCheckout,
        FinalPolicy,
        FinalIndex
    };
    State(TextConversion *owner, const GitRunners &runners) : q(owner), adapter(runners(owner))
    {
        QObject::connect(adapter.get(), &GitRunner::finished, q, [this](const Result &result) { consume(result); });
    }
    void complete(Outcome outcome, Problem problem = {})
    {
        result.outcome = outcome;
        result.problem = std::move(problem);
        if (outcome == Outcome::Success) result.text = text;
        active = false;
        const auto completed = result;
        Q_EMIT q->finished(completed);
    }
    void reject(Problem problem, TextConversionFailure failure = TextConversionFailure::UnsupportedConversion)
    {
        result.failure = failure;
        complete(Outcome::Rejected, std::move(problem));
    }
    void run(Step next, Operation operation, const QByteArray &object = {}, const QByteArray &input = {})
    {
        step = next;
        const auto &request = result.request;
        Request gitRequest;
        gitRequest.operation = operation;
        gitRequest.directory = request.directory;
        gitRequest.path = operation == Operation::ReadObject ? QByteArray{} : request.path;
        gitRequest.objectId = object;
        gitRequest.input = input;
        gitRequest.sessionGeneration = request.sessionGeneration;
        gitRequest.inputRevision = request.inputRevision;
        if (!adapter->start(gitRequest)) complete(Outcome::ProcessFailure, ProblemCode::ConversionBusy);
    }
    void begin()
    {
        if (cancelled) {
            complete(Outcome::Cancelled, ProblemCode::ConversionCancelled);
            return;
        }
        const auto &request = result.request;
        text.path = request.path;
        if (!detail::relativeFile(request.path)) {
            reject(ProblemCode::ConversionInvalidPath);
            return;
        }
        if (request.operation == TextOperation::VerifyPublished) {
            if (!request.expected || request.expected->worktreeRoot.isEmpty() || request.expected->path != request.path
                || (request.expected->mode != "100644" && request.expected->mode != "100755")
                || !detail::objectId(request.expected->canonicalObjectId)) {
                reject(ProblemCode::ConversionMissingSnapshot);
                return;
            }
            text = *request.expected;
            const auto serialized = storage::serializeText(text.document.utf8, text.document.format);
            if (!serialized.document || serialized.document->originalBytes != text.document.originalBytes
                || serialized.document->format != text.document.format) {
                reject(ProblemCode::ConversionSnapshotBytes);
                return;
            }
            run(Step::Index, Operation::InspectIndex);
        } else if (request.operation == TextOperation::ReadSource
                   || request.operation == TextOperation::PrepareResult) {
            Problem problem;
            if (!captureMode(request, text, problem))
                reject(problem);
            else if (request.operation == TextOperation::ReadSource)
                run(Step::Source, Operation::MaterializeSource, request.objectId);
            else
                run(Step::Attributes, Operation::ReadAttributes);
        } else
            reject(ProblemCode::ConversionUnknownOperation);
    }
    void consume(const Result &observed)
    {
        if (cancelled) {
            complete(Outcome::Cancelled, ProblemCode::ConversionCancelled);
            return;
        }
        if (observed.outcome != Outcome::Success || observed.outputIncomplete) {
            auto problem = observed.problem;
            // Git's own words about why it failed, which only it can give.
            if (!observed.standardError.isEmpty())
                problem =
                    Problem{ProblemCode::GitFailedWithOutput, {QString::fromUtf8(observed.standardError).trimmed()}}
                        .wrapping(std::move(problem));
            complete(observed.outcome == Outcome::Success ? Outcome::OutputLimit : observed.outcome, problem);
            return;
        }
        if (!text.worktreeRoot.isEmpty() && observed.worktreeRoot != text.worktreeRoot) {
            reject(ProblemCode::ConversionWorktreeChanged);
            return;
        }
        const auto &request = result.request;
        Problem problem;
        switch (step) {
        case Step::Attributes: {
            text.worktreeRoot = observed.worktreeRoot;
            text.policy = observed.conversionPolicy;
            if (!supportedPolicy(text.policy, problem)) {
                reject(problem);
                return;
            }
            const auto format =
                storage::resultFormat(request.launchResultFormat, text.policy.attributes.value("working-tree-encoding"),
                                      text.policy.attributes.value("eol"), problem);
            if (!format) {
                reject(problem);
                return;
            }
            const auto serialized = storage::serializeText(request.editedUtf8, *format);
            if (!serialized.document) {
                reject(serialized.problem);
                return;
            }
            text.document = *serialized.document;
            run(Step::Canonical, Operation::CanonicalizeText, {}, text.document.originalBytes);
            break;
        }
        case Step::Source: {
            text.worktreeRoot = observed.worktreeRoot;
            text.policy = observed.conversionPolicy;
            if (!supportedPolicy(text.policy, problem)) {
                reject(problem);
                return;
            }
            const auto decoded =
                storage::decodeText(observed.standardOutput, text.policy.attributes.value("working-tree-encoding"));
            if (!decoded.document) {
                reject(decoded.problem, TextConversionFailure::UnsupportedText);
                return;
            }
            text.document = *decoded.document;
            run(Step::RawSource, Operation::ReadObject, request.objectId);
            break;
        }
        case Step::RawSource:
            if (observed.conversionPolicy.configuration != text.policy.configuration) {
                reject(ProblemCode::ConversionConfigurationChanged);
                return;
            }
            text.canonicalBytes = observed.standardOutput;
            run(Step::Canonical, Operation::CanonicalizeText, {}, text.document.originalBytes);
            break;
        case Step::Canonical:
            if (observed.conversionPolicy != text.policy) {
                reject(ProblemCode::ConversionPolicyChangedReload);
            } else if (observed.standardOutput != text.document.originalBytes) {
                reject(ProblemCode::ConversionChangesBytes);
            } else if (request.operation == TextOperation::ReadSource
                       && (observed.canonicalObjectId != request.objectId
                           || observed.canonicalBytes != text.canonicalBytes)) {
                reject(ProblemCode::ConversionNoRoundTrip);
            } else {
                text.canonicalBytes = observed.canonicalBytes;
                text.canonicalObjectId = observed.canonicalObjectId;
                run(Step::FinalPolicy, Operation::ReadAttributes);
            }
            break;
        case Step::Index:
        case Step::FinalIndex:
            if (observed.indexEntries.size() != 1 || observed.indexEntries.front().stage != 0
                || observed.indexEntries.front().path != request.path || observed.indexEntries.front().mode != text.mode
                || observed.indexEntries.front().objectId != text.canonicalObjectId) {
                reject(ProblemCode::ConversionStagedIdentity);
            } else if (step == Step::FinalIndex)
                complete(Outcome::Success);
            else
                run(Step::PublishedRaw, Operation::ReadObject, text.canonicalObjectId);
            break;
        case Step::PublishedRaw:
            if (observed.standardOutput != text.canonicalBytes)
                reject(ProblemCode::ConversionStagedBytes);
            else
                run(Step::PublishedCheckout, Operation::MaterializeSource, text.canonicalObjectId);
            break;
        case Step::PublishedCheckout:
            if (observed.conversionPolicy != text.policy || observed.standardOutput != text.document.originalBytes) {
                reject(ProblemCode::ConversionStagedCheckout);
            } else
                run(Step::FinalPolicy, Operation::ReadAttributes);
            break;
        case Step::FinalPolicy:
            if (observed.conversionPolicy != text.policy) {
                reject(ProblemCode::ConversionPolicyChanged);
            } else if (request.operation == TextOperation::VerifyPublished)
                run(Step::FinalIndex, Operation::InspectIndex);
            else
                complete(Outcome::Success);
            break;
        }
    }
    TextConversion *q;
    std::unique_ptr<GitRunner> adapter;
    TextConversionResult result;
    ConvertedText text;
    Step step = Step::Attributes;
    bool active = false;
    bool cancelled = false;
};

TextConversion::TextConversion(const GitRunners &runners, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, runners))
{
    qRegisterMetaType<TextConversionResult>();
}
TextConversion::TextConversion(GitProgram program, QObject *parent)
    : TextConversion(gitAdapters(std::move(program)), parent)
{}
TextConversion::~TextConversion() = default;
bool TextConversion::start(const TextConversionRequest &request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->active) return false;
    d->result = {};
    d->result.request = request;
    d->text = {};
    d->cancelled = false;
    d->active = true;
    QTimer::singleShot(0, this, [this] { d->begin(); });
    return true;
}
void TextConversion::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active) return;
    d->cancelled = true;
    d->adapter->cancel();
}
bool TextConversion::busy() const { return d->active; }
} // namespace branch_truce::git
