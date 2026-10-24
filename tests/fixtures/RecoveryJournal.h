#pragma once

#include "storage/RecoveryStore.h"
#include "TestHarness.h"
#include <QFile>
#include <QJsonDocument>

namespace test_support {
// Model an intact frame written by a different schema or a corrupt producer.
// Rehashing the final frame lets tests reach semantic validation independently
// of the journal's checksum and hash-chain guards.
template<typename Mutator>
void rewriteLastRecoveryState(const QString &directory, Mutator mutate)
{
    QFile journal(directory + "/journal");
    CHECK(journal.open(QIODevice::ReadOnly));
    auto lines = journal.readAll().split('\n');
    CHECK(journal.error() == QFileDevice::NoError && lines.size() >= 3 && lines.back().isEmpty());
    journal.close();
    auto &line = lines[lines.size() - 2];
    const auto frame = QJsonDocument::fromJson(line).object();
    auto record = QJsonDocument::fromJson(QByteArray::fromBase64(frame.value("record").toString().toLatin1())).object();
    auto state = record.value("state").toObject();
    CHECK(!state.isEmpty());
    mutate(state);
    record.insert("state", state);
    const auto body = QJsonDocument(record).toJson(QJsonDocument::Compact);
    line = QJsonDocument(QJsonObject{{"record", QString::fromLatin1(body.toBase64())},
        {"sha256", QString::fromLatin1(branch_truce::storage::RecoveryStore::digest(body))}}).toJson(QJsonDocument::Compact);
    const auto bytes = lines.join('\n');
    CHECK(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(journal.write(bytes) == bytes.size());
}
}
