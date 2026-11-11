// What startup captures for each conflict: mixed inventories, failed alignments and placeholders,
// and the sources, provenance and formats of merge, rebase and converted files.
#include "StartupRoutes.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"

using test_support::await;

using namespace startup_routes;

namespace {
void addInventoryCaptureCases(test_support::Suite &suite)
{
    suite.run(
        "mixed inventory retains every exact path sources initial results and persistent outstanding reasons", [] {
            Repository repo;
            const QMap<QString, std::array<QByteArray, 3>> additional{
                {":literal\tfile.txt", {"tab base\n", "tab main\n", "tab side\n"}},
                {":literal\\tfile.txt", {"slash base\n", "slash main\n", "slash side\n"}},
                {"binary.dat",
                 {QByteArray("base\0binary", 11), QByteArray("main\0binary", 11), QByteArray("side\0binary", 11)}},
                {"missing.txt", {"missing base\n", "missing main\n", "missing side\n"}}};
            repo.conflict("merge", additional);
            for (const auto &path : {QString(":literal\tfile.txt"), QString(":literal\\tfile.txt")})
                writeFile(repo.path(path), "preserved manual result for " + path.toUtf8() + '\n');
            CHECK(QFile::remove(repo.path("missing.txt")));
            const auto unmerged = repo.git({"diff", "--name-only", "--diff-filter=U", "-z"}).split('\0');
            QSet<QByteArray> expectedPaths;
            for (const auto &path : unmerged)
                if (!path.isEmpty()) expectedPaths.insert(path);
            CHECK(expectedPaths.size() == 6);
            const auto before = repo.snapshot();
            ScopedEnvironment environment(repo.environment);
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            auto *model = controller.store();
            CHECK(controller.files()->totalFiles() == 6 && controller.files()->outsideAppFiles() == 1
                  && controller.files()->loadErrorFiles() == 1);
            QSet<QByteArray> actualPaths;
            for (int row = 0; row < controller.files()->totalFiles(); ++row) {
                const auto index = controller.files()->index(row);
                const auto path = QByteArray::fromStdString(controller.files()->fileAt(row)->path());
                const auto id = controller.files()->data(index, SessionModel::FileIdRole).toString();
                actualPaths.insert(path);
                CHECK(id.toLatin1() == path.toHex() && controller.selection()->select(id));
                const auto selected = controller.selectedFile();
                CHECK(selected->pathBytes() == path && selected->fileId() == id);
                if (path == "binary.dat" || path == "missing.txt") {
                    const auto expected = path == "binary.dat" ? FileStates::SupportKind::OutsideApp
                                                               : FileStates::SupportKind::Unreadable;
                    CHECK(selected->support() == expected);
                    CHECK(!selected->supportDetail().isEmpty());
                    CHECK(selected->stage2().isEmpty() && selected->initialResult().isEmpty());
                    CHECK(await(model->replaceDraft(id.toStdString(), 1, {"unsafe edit", {}, 0}))
                          == Transition::Blocked);
                    continue;
                }
                CHECK(selected->supported());
                const auto versions = additional.value(QString::fromUtf8(path), {"base\n", "main\n", "side\n"});
                CHECK(selected->earlier().value("text").toString().toUtf8() == versions[0]);
                CHECK(selected->stage2().value("text").toString().toUtf8() == versions[1]);
                CHECK(selected->stage3().value("text").toString().toUtf8() == versions[2]);
                const auto initial = readFile(repo.path(QString::fromUtf8(path)));
                CHECK(selected->initialResult().value("text").toString().toUtf8() == initial);
                CHECK(selected->resultText().toUtf8() == initial);
                CHECK(!controller.selection()->select(QString::fromUtf8(path))
                      && controller.selection()->fileId() == id);
            }
            CHECK(actualPaths == expectedPaths);
            const auto selectedId = controller.selection()->fileId();
            CHECK(!controller.selection()->select("not-an-exact-file-id")
                  && controller.selection()->fileId() == selectedId);
            // Adapter success callbacks complete supported rows without touching Git in this fixture.
            // Publishing moves display rows. Capture stable identities before the first move.
            std::vector<std::string> supportedIds;
            for (int row = 0; row < controller.files()->totalFiles(); ++row) {
                const auto id = controller.files()
                                    ->data(controller.files()->index(row), SessionModel::FileIdRole)
                                    .toString()
                                    .toStdString();
                if (model->file(id)->support() == Support::Supported) supportedIds.push_back(id);
            }
            CHECK(supportedIds.size() == 4);
            for (const auto &id : supportedIds) {
                CHECK(await(model->replaceDraft(id, 1, {"reviewed supported result\n", {}, 0})) == Transition::Applied);
                const auto revision = model->file(id)->current().id;
                CHECK(await(model->beginMarkResolved(id, revision)) == Transition::Applied);
                CHECK(await(model->recordCheckpoint(id, revision, Outcome::Succeeded)) == Transition::Applied);
                CHECK(await(model->recordWrite(id, revision, Outcome::Succeeded)) == Transition::Applied);
                CHECK(await(model->recordStage(id, revision, Outcome::Succeeded, "fixture-verified-object", "100644"))
                      == Transition::Applied);
            }
            CHECK(controller.files()->resolvedFiles() == 4 && !controller.completion()->handoff().clean
                  && blockedFiles(controller) == 2);
            CHECK(controller.files()->outsideAppFiles() == 1 && controller.files()->loadErrorFiles() == 1);
            CHECK(controller.loader()->detail().contains("outside app remain outstanding"));
            for (const auto path : {"binary.dat", "missing.txt"}) {
                const auto &outstanding = openedFile(controller, path);
                CHECK(!outstanding.supportDetail().empty() && !outstanding.facts().resolved);
                CHECK(controller.selection()->select(QString::fromStdString(outstanding.id())));
                CHECK(!controller.selectedFile()->supportDetail().isEmpty());
            }
            CHECK(repo.snapshot() == before);
        });
    suite.run("a large admitted file correspondence failure does not abort loading the small files", [] {
        Repository repo;
        QByteArray large;
        // The prefix line plus this body stays at the 4000-line envelope.
        // Past that, the result is unreadable before CorrespondenceMap is constructed.
        for (int line = 0; line < 3999; ++line) large += "large line " + QByteArray::number(line) + '\n';
        repo.conflict("merge", {{"large.txt", {"base\n" + large, "main\n" + large, "side\n" + large}}});
        writeFile(repo.path("large.txt"), "preserved large manual result\n" + large);
        const auto before = repo.snapshot();
        const auto smallOther = readFile(repo.path("excluded.txt"));
        ScopedEnvironment environment(repo.environment);
        const auto fault = std::make_shared<const FailingLargeFile>();
        Application controller{[&] {
            auto config = test_support::windowConfig();
            config.correspondence = fault;
            return config;
        }()};
        openController(controller, repo);
        CHECK(fault->failures == 1);
        CHECK(controller.files()->totalFiles() == 3 && controller.files()->loadErrorFiles() == 1);
        const auto &failed = openedFile(controller, "large.txt");
        CHECK(failed.support() == Support::Unreadable && failed.snapshots() && !failed.correspondence());
        CHECK(failed.loadFailure() == LoadFailure{LoadFailureReason::OutOfMemory});
        CHECK(engine_text::supportDetail(failed).contains("not enough memory"));
        CHECK(failed.current().draft.text == ("preserved large manual result\n" + large).toStdString());
        CHECK(failed.snapshots()->stage2->worktree.utf8 == ("main\n" + large).toStdString());
        CHECK(failed.snapshots()->stage3->worktree.utf8 == ("side\n" + large).toStdString());
        for (const auto path : {"tracked.txt", "excluded.txt"}) {
            const auto &small = openedFile(controller, path);
            CHECK(small.support() == Support::Supported && small.correspondence() && small.snapshots());
            CHECK(small.snapshots()->stage2->worktree.utf8 == "main\n"
                  && small.snapshots()->stage3->worktree.utf8 == "side\n");
            CHECK(small.current().draft.text
                  == (QByteArray(path) == "tracked.txt" ? repo.manual : smallOther).toStdString());
            CHECK(controller.selection()->select(QString::fromStdString(small.id())));
            CHECK(controller.selectedFile()->stage2().value("text").toString() == "main\n");
        }
        CHECK(!controller.files()->hasPendingRecovery() && !controller.completion()->handoff().clean);
        CHECK(repo.snapshot() == before);
    });
}

void addCapturePersistenceCases(test_support::Suite &suite)
{
    suite.run(
        "failed placeholder recovery keeps captured files visible blocks editing and retries the captured state", [] {
            Repository repo;
            repo.conflict("merge");
            const auto before = repo.snapshot();
            ScopedEnvironment environment(repo.environment);
            for (const bool failRecovery : {false, true}) {
                // Each case must lack an earlier usable recovery store. A valid
                // store from the healthy case would bypass the intended fault.
                QTemporaryDir recoveryData;
                auto isolatedEnvironment = repo.environment;
                isolatedEnvironment.insert("XDG_DATA_HOME", recoveryData.path());
                ScopedEnvironment isolated(isolatedEnvironment);
                Application controller{test_support::windowConfig()};
                auto *model = controller.store();
                const auto location = model->recoveryDirectory();
                if (failRecovery) {
                    CHECK(QDir().mkpath(QFileInfo(location).path()));
                    writeFile(location, "block recovery storage with a regular file");
                }
                openController(controller, repo);
                CHECK(controller.files()->totalFiles() == 2 && model->hasPendingRecovery() == failRecovery);
                CHECK(controller.files()->loadErrorFiles() == 0 && controller.files()->outsideAppFiles() == 0);
                for (int row = 0; row < controller.files()->totalFiles(); ++row) {
                    CHECK(controller.files()
                              ->data(controller.files()->index(row), SessionModel::PendingInputRole)
                              .toBool()
                          == failRecovery);
                    CHECK(controller.files()
                              ->data(controller.files()->index(row), SessionModel::RecoveryErrorRole)
                              .toString()
                              .isEmpty()
                          == !failRecovery);
                }
                for (const auto path : {"tracked.txt", "excluded.txt"}) {
                    const auto &loaded = openedFile(controller, path);
                    CHECK(loaded.support() == Support::Supported && loaded.snapshots() && loaded.correspondence());
                    CHECK(loaded.supportDetail().empty());
                    CHECK(loaded.current().draft.text == readFile(repo.path(path)).toStdString());
                    CHECK(loaded.snapshots()->stage2->worktree.utf8 == "main\n"
                          && loaded.snapshots()->stage3->worktree.utf8 == "side\n");
                    CHECK(controller.selection()->select(QString::fromStdString(loaded.id())));
                    CHECK(controller.selectedFile()->stage2().value("text").toString() == "main\n");
                    CHECK(controller.selectedFile()->resultText().toStdString() == loaded.current().draft.text);
                    if (failRecovery) {
                        CHECK(model->pendingDraft(loaded.id())
                              && *model->pendingDraft(loaded.id()) == loaded.current().draft);
                        CHECK(await(model->retryRecovery(loaded.id())) == Transition::Blocked);
                        CHECK(await(model->replaceDraft(loaded.id(), 1, {"must wait for recovery", {}, 0}))
                              == Transition::Blocked);
                    }
                }
                if (failRecovery) {
                    CHECK(QFile::remove(location));
                    const auto first = openedFile(controller, "tracked.txt").id();
                    const auto second = openedFile(controller, "excluded.txt").id();
                    CHECK(model->pendingDraft(first) && model->pendingDraft(second));
                    CHECK(await(model->retryRecovery(first)) == Transition::Applied);
                    CHECK(!model->pendingDraft(first) && !model->pendingDraft(second));
                    const auto committed = storage::RecoveryStore(location).read();
                    CHECK(committed.succeeded && committed.transactions.size() == 2);
                    CHECK(await(model->retryRecovery(second)) == Transition::NoChange);
                    const auto unchanged = storage::RecoveryStore(location).read();
                    CHECK(unchanged.succeeded && unchanged.transactions.size() == committed.transactions.size());
                    for (qsizetype row = 0; row < committed.transactions.size(); ++row)
                        CHECK(unchanged.transactions[row].id == committed.transactions[row].id);
                }
                CHECK(!model->hasPendingRecovery());
                storage::RecoveryStore store(location);
                const auto recovered = store.read();
                CHECK(recovered.succeeded && recovered.transactions.size() == (failRecovery ? 2 : 4));
                // Successful placeholders may precede the two final captured-file records.
                for (qsizetype row = recovered.transactions.size() - 2; row < recovered.transactions.size(); ++row) {
                    const auto &state = recovered.transactions[row].state;
                    const auto sources = state.value("snapshots").toObject();
                    CHECK(!sources.isEmpty() && state.value("support") == "supported");
                    Problem error;
                    // A launch record inserts its whole text.
                    const auto encodedText = state.value("current")
                                                 .toObject()
                                                 .value("text")
                                                 .toObject()
                                                 .value("insert")
                                                 .toString()
                                                 .toLatin1();
                    CHECK(!store.payload(encodedText, &error).isEmpty() && error.isEmpty());
                }
                CHECK(await(model->replaceDraft(openedFile(controller).id(), 1, {"editable after recovery\n", {}, 0}))
                      == Transition::Applied);
                CHECK(controller.loader()->startupState() == SessionLoader::StartupState::Ready
                      && repo.snapshot() == before);
            }
        });
}

void addCaptureProvenanceCases(test_support::Suite &suite)
{
    suite.run("merge and rebase captures retain verified sources provenance and the manual launch result", [] {
        for (const auto operation : {"merge", "rebase-merge", "rebase-apply"}) {
            Repository repo;
            repo.conflict(operation);
            const auto launchIdentity = identity(repo.path("tracked.txt"));
            ScopedEnvironment environment(repo.environment);
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            const auto &selected = openedFile(controller);
            CHECK(selected.support() == Support::Supported && selected.snapshots());
            const auto expected = *selected.snapshots();
            CHECK(expected.path == "tracked.txt" && expected.resultMode == "100644");
            CHECK(expected.launchResult.text.utf8 == repo.manual.toStdString());
            CHECK(expected.launchResult.text.bytes == repo.manual.toStdString());
            CHECK(selected.current().draft.text == expected.launchResult.text.utf8);
            CHECK(expected.launchResult.identity == launchIdentity);
            CHECK(expected.launchResult.text.format
                  == SnapshotFormat{SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::Lf, true});
            const bool rebase = QString::fromLatin1(operation).startsWith("rebase");
            const auto &provenance = expected.provenance;
            CHECK(!provenance.sessionId.empty());
            CHECK(provenance.worktreeRoot == repo.root.toStdString());
            CHECK(provenance.gitDirectory == QFileInfo(repo.path(".git")).canonicalFilePath().toStdString());
            CHECK(provenance.operation
                  == (rebase ? SnapshotProvenance::Operation::Rebase : SnapshotProvenance::Operation::Merge));
            CHECK(provenance.stage2CommitId == repo.main.toStdString());
            CHECK(provenance.stage3CommitId == repo.side.toStdString());
            CHECK(provenance.baseCommitId
                  == repo.git({"rev-parse", QString::fromLatin1(repo.side) + '^'}).trimmed().toStdString());
            if (rebase) {
                CHECK(provenance.headName == "refs/heads/side");
                CHECK(provenance.originalHead == repo.side.toStdString());
                CHECK(provenance.onto == repo.main.toStdString());
            }
            CHECK(expected.earlier && expected.stage2 && expected.stage3);
            CHECK(expected.earlier->commitId == provenance.baseCommitId);
            CHECK(expected.stage2->commitId == provenance.stage2CommitId);
            CHECK(expected.stage3->commitId == provenance.stage3CommitId);
            CHECK(expected.earlier->label == "Common earlier version");
            CHECK(expected.stage2->label.starts_with(rebase ? "Current rebased state" : "Current checkout"));
            CHECK(expected.stage3->label.starts_with(rebase ? "Commit being replayed" : "Merged commit"));
            const std::array<const SourceSnapshot *, 3> sources{&*expected.earlier, &*expected.stage2,
                                                                &*expected.stage3};
            for (std::size_t i = 0; i < sources.size(); ++i) {
                const auto &source = *sources[i];
                const auto object =
                    repo.git({"rev-parse", ':' + QString::number(static_cast<unsigned int>(i + 1)) + ":tracked.txt"})
                        .trimmed();
                CHECK(source.objectId == object.toStdString() && source.mode == "100644");
                CHECK(source.rawBytes == repo.git({"cat-file", "blob", QString::fromLatin1(object)}).toStdString());
                CHECK(source.worktree.bytes == source.rawBytes && source.worktree.utf8 == source.rawBytes);
                CHECK(source.worktree.format
                      == SnapshotFormat{SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::Lf, true});
            }
            const auto &other = openedFile(controller, "excluded.txt");
            CHECK(other.snapshots() && other.snapshots()->provenance == provenance);
            auto *model = controller.store();
            auto edited = selected.current().draft;
            edited.text = "later manual edit\n";
            CHECK(await(model->replaceDraft(selected.id(), selected.current().id, edited)) == Transition::Applied);
            CHECK(*selected.snapshots() == expected);
            CHECK(await(model->undo(selected.id(), selected.current().id)) == Transition::Applied);
            CHECK(selected.current().draft.text == repo.manual.toStdString());
            CHECK(await(model->redo(selected.id(), selected.current().id)) == Transition::Applied);
            writeFile(repo.path("tracked.txt"), "external replacement\n");
            repo.git({"add", "--", "tracked.txt"});
            writeFile(repo.path(".gitattributes"), "tracked.txt -text\n");
            CHECK(*selected.snapshots() == expected);
        }
    });
    suite.run("converted source and manual result captures retain independent bytes and format metadata", [] {
        struct FormatCase {
            QByteArray attributes;
            QByteArray earlier;
            QByteArray main;
            QByteArray side;
            QByteArray launch;
            SnapshotFormat::Encoding encoding;
            bool sourceBom;
            bool launchBom;
        };
        const FormatCase cases[]{
            {"text eol=crlf", "base\r\n", "main\r\n", "side\r\n", "\xef\xbb\xbfmanual \xc3\xa9\r\nkept",
             SnapshotFormat::Encoding::Utf8, false, true},
            {"text eol=crlf working-tree-encoding=UTF-16", QByteArray::fromHex("fffe62006100730065000d000a00"),
             QByteArray::fromHex("fffe6d00610069006e000d000a00"), QByteArray::fromHex("fffe73006900640065000d000a00"),
             QByteArray::fromHex("fffe6d0061006e00750061006c002000e9000d000a006b00650070007400"),
             SnapshotFormat::Encoding::Utf16LittleEndian, true, true},
            {"text eol=crlf working-tree-encoding=UTF-16BE", QByteArray::fromHex("0062006100730065000d000a"),
             QByteArray::fromHex("006d00610069006e000d000a"), QByteArray::fromHex("0073006900640065000d000a"),
             QByteArray::fromHex("006d0061006e00750061006c002000e9000d000a006b006500700074"),
             SnapshotFormat::Encoding::Utf16BigEndian, false, false}};
        for (const auto &format : cases) {
            Repository repo;
            repo.git({"config", "core.autocrlf", "false"});
            repo.conflict("merge");
            writeFile(repo.path(".gitattributes"), "tracked.txt " + format.attributes + '\n');
            writeFile(repo.path("tracked.txt"), format.launch);
            const auto launchIdentity = identity(repo.path("tracked.txt"));
            ScopedEnvironment environment(repo.environment);
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            const auto &selected = openedFile(controller);
            CHECK(selected.support() == Support::Supported && selected.snapshots());
            const auto captured = *selected.snapshots();
            CHECK(captured.earlier && captured.stage2 && captured.stage3);
            const std::array<const SourceSnapshot *, 3> sources{&*captured.earlier, &*captured.stage2,
                                                                &*captured.stage3};
            const std::array<QByteArray, 3> checkout{format.earlier, format.main, format.side};
            const std::array<std::string, 3> canonical{"base\n", "main\n", "side\n"};
            for (std::size_t i = 0; i < sources.size(); ++i) {
                CHECK(sources[i]->rawBytes == canonical[i]);
                CHECK(sources[i]->worktree.utf8 == canonical[i]);
                CHECK(sources[i]->worktree.bytes == checkout[i].toStdString());
                CHECK(sources[i]->worktree.bytes != sources[i]->rawBytes);
                CHECK(sources[i]->worktree.format
                      == SnapshotFormat{format.encoding, format.sourceBom, SnapshotFormat::Newline::CrLf, true});
            }
            CHECK(selected.current().draft.text == "manual \xc3\xa9\nkept");
            CHECK(captured.launchResult.text.utf8 == selected.current().draft.text);
            CHECK(captured.launchResult.text.bytes == format.launch.toStdString());
            CHECK(captured.launchResult.text.format
                  == SnapshotFormat{format.encoding, format.launchBom, SnapshotFormat::Newline::CrLf, false});
            CHECK(captured.launchResult.identity == launchIdentity);
            CHECK(captured.conversion.attributes.at("text") == "set");
            CHECK(captured.conversion.attributes.at("eol") == "crlf");
            CHECK(captured.conversion.configuration.at("core.autocrlf") == "false");
            if (format.encoding == SnapshotFormat::Encoding::Utf8) {
                CHECK(!captured.conversion.attributes.contains("working-tree-encoding"));
            } else {
                CHECK(captured.conversion.attributes.at("working-tree-encoding")
                      == (format.encoding == SnapshotFormat::Encoding::Utf16LittleEndian ? "UTF-16" : "UTF-16BE"));
            }
            auto edited = selected.current().draft;
            edited.text += " later";
            CHECK(await(controller.store()->replaceDraft(selected.id(), selected.current().id, edited))
                  == Transition::Applied);
            CHECK(*selected.snapshots() == captured);
        }
    });
}

void addEarlierCaptureCases(test_support::Suite &suite)
{
    suite.run("add-add has an unavailable earlier source while an empty base remains captured", [] {
        for (const bool missing : {false, true}) {
            Repository repo;
            if (missing)
                repo.git({"rm", "--", "tracked.txt"});
            else {
                writeFile(repo.path("tracked.txt"), {});
                repo.git({"add", "--", "tracked.txt"});
            }
            repo.git({"commit", "--quiet", "-m", "prepare earlier source"});
            repo.conflict("merge");
            ScopedEnvironment environment(repo.environment);
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            const auto &selected = openedFile(controller);
            CHECK(selected.support() == Support::Supported && selected.snapshots());
            const auto expected = *selected.snapshots();
            CHECK(expected.earlier.has_value() == !missing);
            CHECK(expected.stage2 && expected.stage3);
            CHECK(expected.stage2->rawBytes == "main\n" && expected.stage3->rawBytes == "side\n");
            CHECK(selected.current().draft.text == repo.manual.toStdString());
            if (!missing) {
                CHECK(expected.earlier->rawBytes.empty());
                CHECK(expected.earlier->worktree.bytes.empty() && expected.earlier->worktree.utf8.empty());
                CHECK(!expected.earlier->objectId.empty() && expected.earlier->commitId.has_value());
                CHECK(!expected.earlier->worktree.format.finalNewline);
            }
            auto edited = selected.current().draft;
            edited.text = "edited after capture\n";
            auto *model = controller.store();
            CHECK(await(model->replaceDraft(selected.id(), selected.current().id, edited)) == Transition::Applied);
            CHECK(await(model->undo(selected.id(), selected.current().id)) == Transition::Applied);
            CHECK(await(model->redo(selected.id(), selected.current().id)) == Transition::Applied);
            CHECK(*selected.snapshots() == expected);
        }
    });
}
} // namespace

void startup_routes::addCaptureCases(test_support::Suite &suite, const QString &)
{
    addInventoryCaptureCases(suite);
    addCapturePersistenceCases(suite);
    addCaptureProvenanceCases(suite);
    addEarlierCaptureCases(suite);
}
