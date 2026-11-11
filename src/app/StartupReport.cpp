// The startup report describes the opened session as one JSON object for smoke tests and scripts.
#include "StartupReport.h"
#include "SessionLoader.h"
#include "SessionModel.h"
#include <QJsonArray>

namespace branch_truce {
namespace {
QString startupStateName(SessionLoader::StartupState state)
{
    switch (state) {
    case SessionLoader::StartupState::Idle: return QStringLiteral("idle");
    case SessionLoader::StartupState::Opening: return QStringLiteral("opening");
    case SessionLoader::StartupState::Ready: return QStringLiteral("ready");
    case SessionLoader::StartupState::ReadyWithoutMergetool: return QStringLiteral("ready-without-mergetool");
    case SessionLoader::StartupState::NoSession: return QStringLiteral("no-session");
    case SessionLoader::StartupState::Unsupported: return QStringLiteral("unsupported");
    case SessionLoader::StartupState::Error: return QStringLiteral("error");
    }
    return {};
}

QString supportName(Support support)
{
    switch (support) {
    case Support::Supported: return QStringLiteral("supported");
    case Support::OutsideApp: return QStringLiteral("outside-app");
    case Support::Unreadable: return QStringLiteral("unreadable");
    case Support::LargeFile: return QStringLiteral("large-file");
    }
    return {};
}
}

QJsonObject startupReport(const SessionLoader &loader, const SessionModel &files)
{
    QJsonArray listed;
    for (int row = 0; row < files.totalFiles(); ++row) {
        const auto index = files.index(row, 0);
        const auto *file = files.fileAt(row);
        listed.push_back(QJsonObject{
            {QStringLiteral("path"), files.data(index, SessionModel::PathRole).toString()},
            {QStringLiteral("support"), supportName(file->support())},
            {QStringLiteral("detail"), files.data(index, SessionModel::SupportDetailRole).toString()},
            {QStringLiteral("text"), QString::fromUtf8(QByteArray::fromStdString(file->current().draft.text))}});
    }
    return {{QStringLiteral("state"), startupStateName(loader.startupState())},
            {QStringLiteral("repository"), loader.repositoryRoot()},
            {QStringLiteral("operation"), loader.operation()},
            {QStringLiteral("detail"), loader.detail()},
            {QStringLiteral("stage2"), loader.stage2()},
            {QStringLiteral("stage3"), loader.stage3()},
            {QStringLiteral("stage2CommitId"), loader.stage2CommitId()},
            {QStringLiteral("stage3CommitId"), loader.stage3CommitId()},
            {QStringLiteral("workflow"), loader.workflow()},
            {QStringLiteral("resolved"), files.resolvedFiles()},
            {QStringLiteral("files"), listed}};
}
}
