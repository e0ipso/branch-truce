// Parent staging holds the path a waiting git mergetool parent stages and the written revision offered to it.
#pragma once

#include "core/RevisionId.h"
#include "git/TextConversion.h"
#include <QByteArray>
#include <QString>
#include <optional>
#include <string>

namespace branch_truce {

// A written revision handed to the parent to stage: the launcher releases Git
// with it, and the index must later show exactly its object.
struct ParentOffer {
    std::string fileId;
    QByteArray path;
    RevisionId revision = 0;
    QByteArray mode;
    QByteArray canonicalObjectId;
    QString worktreeRoot;
    git::ConvertedText prepared;
};

// The one path Git, not Branch Truce, stages, and the offer made for it. A
// new assignment withdraws the offer. Nothing here runs Git or writes files.
class ParentStaging {
public:
    // An empty path ends the assignment.
    void assign(QByteArray path);
    void clear();
    [[nodiscard]] bool owns(const QByteArray &path) const { return !path_.isEmpty() && path_ == path; }
    [[nodiscard]] const QByteArray &path() const { return path_; }
    void offer(ParentOffer offer) { offer_ = std::move(offer); }
    void withdraw() { offer_.reset(); }
    // The offer for fileId, when one is held.
    [[nodiscard]] std::optional<ParentOffer> offerFor(const std::string &fileId) const;

private:
    QByteArray path_;
    std::optional<ParentOffer> offer_;
};
}
