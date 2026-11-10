// Deferred large files keep their launch destination through admission and publication.
#include "TestHarness.h"
#include "app/Application.h"
#include "fixtures/AppSession.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/ConflictedRepository.h"
#include <QCoreApplication>
#include <QThread>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

using namespace branch_truce;
namespace {
class DestinationReads final : public storage::FileSystem {
public:
    mutable std::atomic<int> guiReads{0};
    mutable std::atomic<int> reads{0};
    ssize_t readAt(int fd, void *bytes, std::size_t size, off_t offset) const override
    {
        ++reads;
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) ++guiReads;
        return FileSystem::readAt(fd, bytes, size, offset);
    }
};
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("LargeFilePublicationTests");
    for (const bool outsideEdit : {false, true}) {
        suite.run("real-repository large-file load edit and resolution retain the exact launch", [outsideEdit] {
            test_support::ConflictedRepository repo;
            const auto prefix = QByteArray("common line\n").repeated(4001);
            repo.commit({{"tracked.txt", prefix + "base\n"}}, "base");
            repo.conflict({{"tracked.txt", prefix + "left\n"}}, {{"tracked.txt", prefix + "right\n"}});
            const auto launch = prefix + "manual result\n";
            test_support::writeFile(repo.path("tracked.txt"), launch);
            const auto files = std::make_shared<DestinationReads>();
            Application controller({.git = {test_support::gitExecutable(), git::GitEnvironment::from(repo.environment)},
                                    .recoveryDirectory = repo.path("recovery/session"),
                                    .mergetoolDirectory = repo.runtime.path() + "/mergetool",
                                    .files = files});
            test_support::openRepository(controller, repo.root);
            const auto id = test_support::fileIdOf("tracked.txt");
            CHECK(controller.store()->file(id)->support() == Support::LargeFile);
            const auto index = repo.git({"ls-files", "--stage", "-z"});
            if (outsideEdit) test_support::writeFile(repo.path("tracked.txt"), "outside edit\n");
            CHECK(test_support::await(controller.store()->loadLargeFile(id)) == Transition::Applied);
            const auto *file = controller.store()->file(id);
            CHECK(file->support() == Support::Supported
                  && file->snapshots()->launchResult.text.bytes == launch.toStdString());
            auto draft = file->current().draft;
            draft.text = "reviewed result\n";
            CHECK(test_support::await(controller.store()->replaceDraft(id, file->current().id, draft))
                  == Transition::Applied);
            CHECK(controller.publisher()->start(id, controller.store()->file(id)->current().id, true).transition
                  == Transition::Applied);
            test_support::waitUntil([&] { return !controller.publisher()->busy() && !controller.store()->recording(); },
                                    "large-file publication did not settle");
            const auto &publication = controller.store()->file(id)->publication();
            CHECK(controller.store()->file(id)->facts().resolved == !outsideEdit);
            CHECK(test_support::readFile(repo.path("tracked.txt"))
                  == (outsideEdit ? "outside edit\n" : "reviewed result\n"));
            if (outsideEdit) {
                CHECK(publication.writeOutcome() && publication.writeOutcome()->outcome == Outcome::Failed);
                CHECK(!publication.written() && !publication.staged()
                      && repo.git({"ls-files", "--stage", "-z"}) == index);
            } else {
                CHECK(publication.written() && publication.staged());
                CHECK(repo.git({"show", ":tracked.txt"}) == "reviewed result\n");
            }
            CHECK(files->guiReads == 0);
        });
    }
    suite.run("baseline capture refuses a changed launch before reading any file bytes", [] {
        QTemporaryDir directory;
        const auto root = QFileInfo(directory.path()).canonicalFilePath();
        test_support::writeFile(root + "/choices.txt", "launch\n");
        const auto launch = storage::captureDestination(root, "choices.txt");
        CHECK(launch.destination);
        auto snapshots = *test_support::choiceFile("launch\n", "other\n").snapshots();
        snapshots.launchResult.identity = launch.destination->identity();
        FileSession file("choices", "choices.txt", {"launch\n", {}, 0}, {.snapshots = snapshots});
        CHECK(::truncate(QFile::encodeName(root + "/choices.txt").constData(), off_t{1} << 40) == 0);
        const auto files = std::make_shared<DestinationReads>();
        const auto baselines = ResolutionPublisher::readBaselines(root, {file}, files);
        CHECK(!baselines.at("choices").destination && !baselines.at("choices").diagnostic.isEmpty());
        CHECK(files->reads == 0);
    });
    return suite.report();
}
