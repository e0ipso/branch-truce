// The selection names the one file the window shows and every command acts on.
#include "Selection.h"
#include "SessionModel.h"

namespace branch_truce {
Selection::Selection(const SessionModel &files, const SelectionGuard &guard, QObject *parent)
    : QObject(parent), files_(files), guard_(guard)
{
    connect(&files_, &SessionModel::summaryChanged, this, &Selection::filesChanged);
}

int Selection::row() const { return files_.rowOf(id()); }

bool Selection::select(const QString &fileId)
{
    if (!files_.store().file(fileId.toStdString())) return false;
    if (fileId_ == fileId) return true;
    if (guard_.holdsSelection()) return false;
    fileId_ = fileId;
    Q_EMIT changed();
    return true;
}

void Selection::filesChanged()
{
    if (files_.store().file(id())) return;
    auto next = files_.fileIdAt(0);
    if (!preferred_.isEmpty() && files_.store().file(preferred_.toStdString())) {
        next = preferred_;
        preferred_.clear();
    }
    if (next == fileId_) return;
    fileId_ = next;
    Q_EMIT changed();
}
}
