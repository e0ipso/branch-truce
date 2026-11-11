// Parent staging holds the path a waiting git mergetool parent stages and the written revision offered to it.
#include "publication/ParentStaging.h"

namespace branch_truce {
void ParentStaging::assign(QByteArray path)
{
    path_ = std::move(path);
    offer_.reset();
}

void ParentStaging::clear() { assign({}); }

std::optional<ParentOffer> ParentStaging::offerFor(const std::string &fileId) const
{
    if (!offer_ || offer_->fileId != fileId) return std::nullopt;
    return offer_;
}
}
