#pragma once

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTextDocument>
#include <ctime>
#include <vector>

namespace test_support {
// Public signal delivery milestones, not exclusive CPU attribution. Run only in a
// separate diagnostic replay: even clock reads must stay out of the budget sample.
// Connections use this GUI-thread context, including signals from other threads.
class TypingTrace final : public QObject {
public:
    using Milestone = std::size_t;

    Milestone add(const QString &name)
    {
        milestones_.push_back({name, {}});
        return milestones_.size() - 1;
    }

    void watchDocument(const QString &pane, QTextDocument *document)
    {
        const auto text = add(pane + QStringLiteral(".contentsChange.text"));
        const auto nonText = add(pane + QStringLiteral(".contentsChange.noCharacters"));
        const auto changed = add(pane + QStringLiteral(".contentsChanged"));
        const auto update = add(pane + QStringLiteral(".layout.update"));
        const auto size = add(pane + QStringLiteral(".layout.documentSizeChanged"));
        connect(document, &QTextDocument::contentsChange, this, [this, text, nonText](int, int removed, int added) {
            mark(removed || added ? text : nonText);
        });
        connect(document, &QTextDocument::contentsChanged, this, [this, changed] { mark(changed); });
        if (auto *layout = document->documentLayout()) {
            connect(layout, &QAbstractTextDocumentLayout::update, this, [this, update] { mark(update); });
            connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged, this, [this, size] { mark(size); });
        }
    }

    void begin()
    {
        for (auto &milestone : milestones_) milestone.sample = {};
        wall_.start();
        cpuStart_ = processCpuUs();
        cpuClockValid_ = cpuStart_ >= 0;
        active_ = true;
    }

    void mark(Milestone id)
    {
        if (!active_) return;
        auto &sample = milestones_[id].sample;
        const auto cpu = processCpuUs();
        if (cpu < cpuStart_ || cpu < 0) cpuClockValid_ = false;
        const Stamp stamp{wall_.nsecsElapsed() / 1000, cpu - cpuStart_};
        if (sample.count++ == 0) sample.first = stamp;
        sample.last = stamp;
    }

    void finish() { active_ = false; }

    QJsonObject json() const
    {
        QJsonArray phases;
        for (const auto &milestone : milestones_) {
            const auto &sample = milestone.sample;
            QJsonObject phase{{"phase", milestone.name}, {"count", sample.count}};
            if (sample.count) {
                phase.insert("first_wall_us", sample.first.wall);
                phase.insert("last_wall_us", sample.last.wall);
                if (cpuClockValid_) {
                    phase.insert("first_process_cpu_us", sample.first.cpu);
                    phase.insert("last_process_cpu_us", sample.last.cpu);
                }
            }
            phases.append(phase);
        }
        return {{"cpu_clock_valid", cpuClockValid_}, {"phases", phases}};
    }

private:
    static qint64 processCpuUs()
    {
        timespec now {};
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0) return -1;
        return static_cast<qint64>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
    }

    struct Stamp {
        qint64 wall = 0;
        qint64 cpu = 0;
    };
    struct Sample {
        int count = 0;
        Stamp first;
        Stamp last;
    };
    struct NamedSample {
        QString name;
        Sample sample;
    };
    std::vector<NamedSample> milestones_;
    QElapsedTimer wall_;
    qint64 cpuStart_ = -1;
    bool cpuClockValid_ = false;
    bool active_ = false;
};
}
