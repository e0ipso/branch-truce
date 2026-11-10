#pragma once

// Journeys register under a group; a run selects one group or all of them. Real merges and
// rebases make the journeys slow, so CTest registers each group as its own entry with a timeout
// sized for it (tests/CMakeLists.txt). A new journey is a function and one add() in its group's
// file; a new group is a file, one call in main() and one name in the CMake list.

#include "TestHarness.h"
#include "acceptance/JourneyScene.h"

#include <QElapsedTimer>

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace journey {

class JourneySuite {
public:
    using Journey = std::function<void(Sandbox &)>;

    void add(std::string group, std::string name, Journey journey)
    {
        journeys_.push_back({std::move(group), std::move(name), std::move(journey)});
    }
    bool hasGroup(const std::string &group) const
    {
        for (const auto &entry : journeys_)
            if (entry.group == group) return true;
        return false;
    }
    // Runs the journeys of `group`, or all of them, each in a sandbox of its own, and prints each
    // one's wall time so the CTest timeouts can be set from a recorded run.
    int run(const std::optional<std::string> &group)
    {
        test_support::Suite suite("JourneyAcceptanceTests");
        for (const auto &entry : journeys_) {
            if (group && entry.group != *group) continue;
            QElapsedTimer clock;
            clock.start();
            suite.run(entry.name, [&entry] {
                Sandbox sandbox;
                entry.run(sandbox);
            });
            if (!test_support::options().list) std::cout << "TIME " << clock.elapsed() << " ms " << entry.name << '\n';
        }
        return suite.report();
    }

private:
    struct Entry {
        std::string group;
        std::string name;
        Journey run;
    };
    std::vector<Entry> journeys_;
};

void addReviewJourneys(JourneySuite &suite);
void addHandoffJourneys(JourneySuite &suite);
void addWindowJourneys(JourneySuite &suite);

} // namespace journey
