#pragma once

#include "TestHarness.h"
#include "recovery/RecoveryRecord.h"
#include "storage/RecoveryStore.h"
#include <QFile>
#include <QJsonDocument>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace test_support {
// The current draft each record of a journal holds, in journal order. A record stores its text as
// a change from the file's previous record, so the drafts are rebuilt in order. Mutation records
// change no text and repeat the draft before them.
inline std::vector<branch_truce::Draft> recordedDrafts(const branch_truce::storage::RecoveryStore &store)
{
    const auto read = store.read();
    CHECK(read.succeeded);
    std::map<std::string, std::string> texts;
    std::vector<branch_truce::Draft> drafts;
    for (const auto &stored : read.transactions) {
        const branch_truce::RecoveryRecordReader record(store, stored.state);
        if (record.mutation()) {
            drafts.push_back(drafts.empty() ? branch_truce::Draft{} : drafts.back());
            continue;
        }
        const auto id = record.fileId();
        auto draft = record.draft(record.action() == branch_truce::RecoveryAction::Open ? std::string{} : texts[id]);
        texts[id] = draft.text;
        drafts.push_back(std::move(draft));
    }
    return drafts;
}

// A lease on a store directory for a case that appends records itself; fails the case when the
// store is held.
inline std::shared_ptr<branch_truce::storage::RecoveryLease> leasedStore(const QString &directory)
{
    auto lease = branch_truce::storage::RecoveryLease::acquire(directory);
    CHECK(lease != nullptr);
    return lease;
}

// Model an intact frame written by a different schema or a corrupt producer.
// Rehashing the final frame lets tests reach semantic validation independently
// of the journal's checksum and hash-chain guards.
template <typename Mutator> void rewriteLastRecoveryState(const QString &directory, Mutator mutate)
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
    line =
        QJsonDocument(QJsonObject{{"record", QString::fromLatin1(body.toBase64())},
                                  {"sha256", QString::fromLatin1(branch_truce::storage::RecoveryStore::digest(body))}})
            .toJson(QJsonDocument::Compact);
    const auto bytes = lines.join('\n');
    CHECK(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(journal.write(bytes) == bytes.size());
}
}
