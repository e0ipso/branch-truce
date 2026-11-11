#pragma once

// The conflicted files several journeys open.

#include "acceptance/JourneyScene.h"

#include <QByteArray>

#include <optional>
#include <vector>

namespace journey {

// A file whose two conflicts sit on lines 2 and 6, around three shared bridge lines.
inline constexpr char storyBase[] = "header\nbase first\nbridge one\nbridge two\nbridge three\nbase later\ntail\n";
inline constexpr char storyLeft[] = "header\nleft first\nbridge one\nbridge two\nbridge three\nleft later\ntail\n";
inline constexpr char storyRight[] = "header\nright first\nbridge one\nbridge two\nbridge three\nright later\ntail\n";

// Three supported files and one modify/delete file the app cannot edit.
inline std::vector<PathSpec> journeyPaths()
{
    // A line far wider than any pane, so wrapping changes its layout.
    const QByteArray longLine = QByteArray(420, 'L') + "\n";
    return {
        // The user replaced Git's conflict output with the common version before launch.
        {"story.txt", storyBase, storyLeft, storyRight, storyBase},
        {"notes.txt", QByteArray("notes base\n"), QByteArray("notes left\n"), QByteArray("notes right\n"),
         QByteArray("notes edited before launch \xc3\xa9\n")},
        // Git's own conflict output stays the launch result.
        {"reading.txt", "REPEAT one\n" + longLine + "REPEAT two\nspace\n",
         "REPEAT one\n" + longLine + "REPEAT left\nspace \n", "REPEAT one\n" + longLine + "REPEAT right\nspace\t\n",
         std::nullopt},
        // Modified on the left and deleted on the right: outside the app.
        {"removed.txt", QByteArray("removed base\n"), QByteArray("removed edited on the left\n"), std::nullopt,
         std::nullopt},
    };
}

// Six files the user chose by hand before launch, one of them in a directory a journey locks, and
// one modify/delete file.
inline std::vector<PathSpec> handoffPaths()
{
    std::vector<PathSpec> paths;
    for (const auto *name : {"done.txt", "newer.txt", "saved.txt", "typed.txt", "written.txt", "locked/failing.txt"}) {
        const QByteArray stem(name);
        paths.push_back({name, stem + " base\n", stem + " left\n", stem + " right\n", stem + " chosen by hand\n"});
    }
    paths.push_back({"removed.txt", QByteArray("removed base\n"), QByteArray("removed edited on the left\n"),
                     std::nullopt, std::nullopt});
    return paths;
}

// One file each side changed, which the user chose by hand before launch.
inline PathSpec chosenByHand(const char *name)
{
    const QByteArray file(name);
    const auto stem = file.left(file.indexOf('.'));
    return {name, stem + " base\n", stem + " left\n", stem + " right\n", stem + " chosen by hand\n"};
}

} // namespace journey
