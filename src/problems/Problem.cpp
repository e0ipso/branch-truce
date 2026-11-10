// A problem is a typed failure with the values its message names, which ProblemText turns into translated text.
#include "problems/Problem.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <array>

namespace branch_truce {
namespace {
constexpr std::array kNames{
#define BRANCH_TRUCE_PROBLEM_NAME(name) #name,
    BRANCH_TRUCE_PROBLEMS(BRANCH_TRUCE_PROBLEM_NAME)
#undef BRANCH_TRUCE_PROBLEM_NAME
};

// Encoded problems are JSON objects with this key; no message of the engine
// or an adapter is one.
const auto kKey = QStringLiteral("problem");

QJsonObject toJson(const Problem &problem)
{
    QJsonObject object{{kKey, problemName(problem.code)},
                       {QStringLiteral("arguments"), QJsonArray::fromStringList(problem.arguments)}};
    if (problem.cause) object.insert(QStringLiteral("cause"), toJson(*problem.cause));
    return object;
}

std::optional<Problem> fromJson(const QJsonObject &object, int depth)
{
    if (depth > 8 || !object.value(kKey).isString() || !object.value(QStringLiteral("arguments")).isArray())
        return std::nullopt;
    const auto name = object.value(kKey).toString();
    Problem problem;
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        if (name == QLatin1String(kNames[index])) problem.code = static_cast<ProblemCode>(index + 1);
    }
    if (problem.isEmpty()) return std::nullopt;
    for (const auto &argument : object.value(QStringLiteral("arguments")).toArray()) {
        if (!argument.isString()) return std::nullopt;
        problem.arguments.push_back(argument.toString());
    }
    if (object.contains(QStringLiteral("cause"))) {
        const auto cause = fromJson(object.value(QStringLiteral("cause")).toObject(), depth + 1);
        if (!cause) return std::nullopt;
        problem.cause = std::make_shared<const Problem>(*cause);
    }
    return problem;
}
}

QString problemName(ProblemCode code)
{
    const auto index = static_cast<std::size_t>(code);
    return index == 0 || index > kNames.size() ? QString{} : QString::fromLatin1(kNames[index - 1]);
}

Problem Problem::wrapping(Problem inner) const
{
    auto outer = *this;
    outer.cause = std::make_shared<const Problem>(std::move(inner));
    return outer;
}

bool Problem::operator==(const Problem &other) const
{
    return code == other.code && arguments == other.arguments && !cause == !other.cause
        && (!cause || *cause == *other.cause);
}

bool Problem::involves(ProblemCode problemCode) const
{
    return code == problemCode || (cause && cause->involves(problemCode));
}

QString Problem::encoded() const
{
    if (isEmpty()) return {};
    return QString::fromUtf8(QJsonDocument(toJson(*this)).toJson(QJsonDocument::Compact));
}

std::optional<Problem> Problem::decode(const QString &text)
{
    if (!text.startsWith(QLatin1Char('{'))) return std::nullopt;
    const auto document = QJsonDocument::fromJson(text.toUtf8());
    return document.isObject() ? fromJson(document.object(), 0) : std::nullopt;
}
} // namespace branch_truce
