#pragma once

// Driving an Application the way a window does, for the suites that open real repositories.
//
//   waitOpened(application, what)          runs the event loop until the loader stops opening.
//   openRepository(application, root, ...) asks the loader to open root and waits for it.
//   fileIdOf(path)                         the ID a loaded file gets: its path bytes in hex.
//   loadedFile(application, path)          the loaded file at path; a missing one fails the case.

#include "TestHarness.h"
#include "app/Application.h"
#include "fixtures/EventLoop.h"

#include <QByteArray>
#include <QString>

#include <string>

namespace test_support {

// Opening takes about 0.5 s on an idle host. At load averages of 25 to 36 on 8 cores it averaged
// 2.5 s and some opens took over 10 s, so the wait uses the shared deadline.
inline void waitOpened(branch_truce::Application &application, const std::string &what)
{
    auto *loader = application.loader();
    waitUntil([loader] { return !loader->opening(); },
              [&] { return what + " did not finish opening: " + loader->detail().toStdString(); });
}

// Opens root as a window would and waits until the loader settles, in any state.
inline void openRepository(branch_truce::Application &application, const QString &root,
                           const QString &preferredFileId = {}, const QString &recoverySession = {})
{
    CHECK(application.loader()->openRepository(root, preferredFileId, recoverySession));
    waitOpened(application, "the repository " + root.toStdString());
}

inline std::string fileIdOf(const QByteArray &path) { return path.toHex().toStdString(); }

inline const branch_truce::FileSession &loadedFile(branch_truce::Application &application,
                                                   const QByteArray &path = "tracked.txt")
{
    const auto *file = application.store()->file(fileIdOf(path));
    if (!file) throw Failure("no loaded file " + path.toStdString());
    return *file;
}

} // namespace test_support
