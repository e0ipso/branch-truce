#include "TextConversion.h"
#include "GitParsing.h"

#include <QThread>
#include <QTimer>
#include <array>

namespace branch_truce::git {
namespace {
bool attributeAbsent(const QByteArray &value)
{
    return value.isEmpty() || value == "unspecified" || value == "unset";
}
bool supportedPolicy(const ConversionPolicy &policy, QString &diagnostic)
{
    const auto text = policy.attributes.value("text");
    const auto ident = policy.attributes.value("ident");
    const auto eol = policy.attributes.value("eol");
    if ((!attributeAbsent(text) && text != "set" && text != "auto")
        || (!attributeAbsent(ident) && ident != "set")
        || (!attributeAbsent(eol) && eol != "lf" && eol != "crlf")
        || (text == "unset" && !attributeAbsent(eol))) {
        diagnostic = QStringLiteral("Outside app: unsupported or contradictory text/eol/ident attributes");
        return false;
    }
    return true;
}
bool captureMode(const TextConversionRequest &request, ConvertedText &text, QString &diagnostic)
{
    std::array<const IndexEntry *, 4> stages{};
    for (const auto &entry : request.stages) {
        if (entry.path != request.path || entry.stage < 1 || entry.stage > 3 || stages[entry.stage]
            || (entry.mode != "100644" && entry.mode != "100755") || !detail::objectId(entry.objectId)) {
            diagnostic = QStringLiteral("Outside app: source stages require unique regular-file entries at the exact path");
            return false;
        }
        stages[entry.stage] = &entry;
    }
    if (!stages[2] || !stages[3] || stages[2]->mode != stages[3]->mode) {
        diagnostic = QStringLiteral("Outside app: stages 2 and 3 require an agreed regular-file mode");
        return false;
    }
    text.mode = stages[2]->mode;
    if (request.operation == TextOperation::ReadSource) {
        if (request.sourceStage < 1 || request.sourceStage > 3 || !stages[request.sourceStage]
            || stages[request.sourceStage]->objectId != request.objectId) {
            diagnostic = QStringLiteral("Outside app: selected source does not match its captured stage identity");
            return false;
        }
        text.sourceMode = stages[request.sourceStage]->mode;
    }
    return true;
}
} // namespace

class TextConversion::State {
public:
    enum class Step { Attributes, Source, RawSource, Canonical, Index, PublishedRaw, PublishedCheckout, FinalPolicy, FinalIndex };
    State(TextConversion *owner, const QString &executable) : q(owner), adapter(executable, owner)
    {
        QObject::connect(&adapter, &GitAdapter::finished, q, [this](const Result &result) { consume(result); });
    }
    void complete(Outcome outcome, const QString &diagnostic = {})
    {
        result.outcome = outcome;
        result.diagnostic = diagnostic;
        if (outcome == Outcome::Success) result.text = text;
        active = false;
        const auto completed = result;
        Q_EMIT q->finished(completed);
    }
    void reject(const QString &diagnostic, TextConversionFailure failure = TextConversionFailure::UnsupportedConversion)
    {
        result.failure = failure;
        complete(Outcome::Rejected, diagnostic);
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
        if (!adapter.start(gitRequest)) complete(Outcome::ProcessFailure, QStringLiteral("Conversion adapter was unexpectedly busy"));
    }
    void begin()
    {
        if (cancelled) {
            complete(Outcome::Cancelled, QStringLiteral("Text conversion cancelled"));
            return;
        }
        const auto &request = result.request;
        text.path = request.path;
        if (!detail::relativeFile(request.path)) {
            reject(QStringLiteral("Outside app: invalid exact conversion path"));
            return;
        }
        if (request.operation == TextOperation::VerifyPublished) {
            if (!request.expected || request.expected->worktreeRoot.isEmpty() || request.expected->path != request.path
                || (request.expected->mode != "100644" && request.expected->mode != "100755")
                || !detail::objectId(request.expected->canonicalObjectId)) {
                reject(QStringLiteral("Outside app: missing or inconsistent prepared publication snapshot"));
                return;
            }
            text = *request.expected;
            const auto serialized = storage::serializeText(text.document.utf8, text.document.format);
            if (!serialized.document || serialized.document->originalBytes != text.document.originalBytes
                || serialized.document->format != text.document.format) {
                reject(QStringLiteral("Outside app: prepared publication text no longer matches its serialized bytes"));
                return;
            }
            run(Step::Index, Operation::InspectIndex);
        } else if (request.operation == TextOperation::ReadSource || request.operation == TextOperation::PrepareResult) {
            QString diagnostic;
            if (!captureMode(request, text, diagnostic)) reject(diagnostic);
            else if (request.operation == TextOperation::ReadSource) run(Step::Source, Operation::MaterializeSource, request.objectId);
            else run(Step::Attributes, Operation::ReadAttributes);
        } else reject(QStringLiteral("Unknown text conversion operation"));
    }
    void consume(const Result &observed)
    {
        if (cancelled) {
            complete(Outcome::Cancelled, QStringLiteral("Text conversion cancelled"));
            return;
        }
        if (observed.outcome != Outcome::Success || observed.outputIncomplete) {
            auto diagnostic = observed.diagnostic;
            if (!observed.standardError.isEmpty()) diagnostic += QStringLiteral(": ") + QString::fromUtf8(observed.standardError).trimmed();
            complete(observed.outcome == Outcome::Success ? Outcome::OutputLimit : observed.outcome,
                     diagnostic);
            return;
        }
        if (!text.worktreeRoot.isEmpty() && observed.worktreeRoot != text.worktreeRoot) {
            reject(QStringLiteral("Outside app: conversion worktree identity changed"));
            return;
        }
        const auto &request = result.request;
        QString diagnostic;
        switch (step) {
        case Step::Attributes: {
            text.worktreeRoot = observed.worktreeRoot;
            text.policy = observed.conversionPolicy;
            if (!supportedPolicy(text.policy, diagnostic)) { reject(diagnostic); return; }
            const auto format = storage::resultFormat(request.launchResultFormat,
                text.policy.attributes.value("working-tree-encoding"), text.policy.attributes.value("eol"), diagnostic);
            if (!format) { reject(diagnostic); return; }
            const auto serialized = storage::serializeText(request.editedUtf8, *format);
            if (!serialized.document) { reject(serialized.diagnostic); return; }
            text.document = *serialized.document;
            run(Step::Canonical, Operation::CanonicalizeText, {}, text.document.originalBytes);
            break;
        }
        case Step::Source: {
            text.worktreeRoot = observed.worktreeRoot;
            text.policy = observed.conversionPolicy;
            if (!supportedPolicy(text.policy, diagnostic)) { reject(diagnostic); return; }
            const auto decoded = storage::decodeText(observed.standardOutput, text.policy.attributes.value("working-tree-encoding"));
            if (!decoded.document) { reject(decoded.diagnostic, TextConversionFailure::UnsupportedText); return; }
            text.document = *decoded.document;
            run(Step::RawSource, Operation::ReadObject, request.objectId);
            break;
        }
        case Step::RawSource:
            if (observed.conversionPolicy.configuration != text.policy.configuration) {
                reject(QStringLiteral("Outside app: conversion configuration changed while reading a source"));
                return;
            }
            text.canonicalBytes = observed.standardOutput;
            run(Step::Canonical, Operation::CanonicalizeText, {}, text.document.originalBytes);
            break;
        case Step::Canonical:
            if (observed.conversionPolicy != text.policy) {
                reject(QStringLiteral("Outside app: conversion attributes/configuration changed; reload before publishing"));
            } else if (observed.standardOutput != text.document.originalBytes) {
                reject(QStringLiteral("Outside app: clean/checkout changes the reviewed worktree bytes, including EOL, BOM or ident expansion"));
            } else if (request.operation == TextOperation::ReadSource
                       && (observed.canonicalObjectId != request.objectId || observed.canonicalBytes != text.canonicalBytes)) {
                reject(QStringLiteral("Outside app: source clean conversion does not round-trip to the captured canonical object"));
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
                reject(QStringLiteral("Outside app: staged canonical identity or executable mode differs from the reviewed result"));
            } else if (step == Step::FinalIndex) complete(Outcome::Success);
            else run(Step::PublishedRaw, Operation::ReadObject, text.canonicalObjectId);
            break;
        case Step::PublishedRaw:
            if (observed.standardOutput != text.canonicalBytes) reject(QStringLiteral("Outside app: staged canonical bytes differ from the reviewed result"));
            else run(Step::PublishedCheckout, Operation::MaterializeSource, text.canonicalObjectId);
            break;
        case Step::PublishedCheckout:
            if (observed.conversionPolicy != text.policy || observed.standardOutput != text.document.originalBytes) {
                reject(QStringLiteral("Outside app: staged checkout or conversion policy differs from the reviewed result"));
            } else run(Step::FinalPolicy, Operation::ReadAttributes);
            break;
        case Step::FinalPolicy:
            if (observed.conversionPolicy != text.policy) {
                reject(QStringLiteral("Outside app: conversion policy changed during verification"));
            } else if (request.operation == TextOperation::VerifyPublished) run(Step::FinalIndex, Operation::InspectIndex);
            else complete(Outcome::Success);
            break;
        }
    }
    TextConversion *q;
    GitAdapter adapter;
    TextConversionResult result;
    ConvertedText text;
    Step step = Step::Attributes;
    bool active = false;
    bool cancelled = false;
};

TextConversion::TextConversion(const QString &executable, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, executable))
{
    qRegisterMetaType<TextConversionResult>();
}
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
    d->adapter.cancel();
}
bool TextConversion::busy() const { return d->active; }
} // namespace branch_truce::git
