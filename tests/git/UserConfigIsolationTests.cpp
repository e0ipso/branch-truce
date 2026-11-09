// Whether the Git that GitAdapter starts can read the invoking account's or the system's Git
// configuration. GitAdapter builds its child environment itself, from the account's passwd home,
// and leaves the system configuration on, so its Git reads ~/.gitconfig, ~/.config/git/config and
// /etc/gitconfig, which no test fixture controls. Every Git suite's result then depends on the
// developer's own settings, such as core.autocrlf, core.hooksPath or filter.lfs.*.
//
// This case fails until GitAdapter takes that environment from its caller. CMake registers it
// with WILL_FAIL; remove that when the adapter's Git stops reading the account's configuration,
// and construct the adapter below with the fixture's environment if the new seam needs one.
#include "TestHarness.h"
#include "git/GitFixtures.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>

#include <pwd.h>
#include <unistd.h>

using namespace git_fixture;

namespace {
// The configuration files Git reads for one recorded environment, besides the repository's own.
QStringList configurationRead(const QHash<QByteArray, QString> &variables)
{
    QStringList files;
    if (variables.value("GIT_CONFIG_NOSYSTEM") != QStringLiteral("1")) files << QStringLiteral("/etc/gitconfig");
    if (variables.contains("GIT_CONFIG_GLOBAL")) return files << variables.value("GIT_CONFIG_GLOBAL");
    const auto home = variables.value("HOME");
    const auto xdg = variables.value("XDG_CONFIG_HOME");
    return files << (xdg.isEmpty() ? home + QStringLiteral("/.config") : xdg) + QStringLiteral("/git/config")
                 << home + QStringLiteral("/.gitconfig");
}

void theAdaptersGitReadsNoAccountOrSystemConfiguration()
{
    const auto *account = ::getpwuid(::geteuid());
    if (account == nullptr) test_support::skip("this account has no passwd entry");
    const auto accountHome = QDir::cleanPath(QFile::decodeName(account->pw_dir));
    if (QDir::cleanPath(QDir::homePath()) == accountHome)
        test_support::skip("HOME is the account's home; run this under tests/with-private-xdg.sh");
    const QStringList accountFiles{accountHome + QStringLiteral("/.gitconfig"),
                                   accountHome + QStringLiteral("/.config/git/config")};

    Repository repo;
    const auto log = repo.executables() + QStringLiteral("/environments");
    // Records what each Git call would read, then runs the fixture Git.
    const auto recorder = repo.executable(
        (QStringLiteral(
             "#!/bin/sh\n"
             "{ printf 'HOME=%s\\n' \"$HOME\"\n"
             "  [ -n \"${XDG_CONFIG_HOME+set}\" ] && printf 'XDG_CONFIG_HOME=%s\\n' \"$XDG_CONFIG_HOME\"\n"
             "  [ -n \"${GIT_CONFIG_GLOBAL+set}\" ] && printf 'GIT_CONFIG_GLOBAL=%s\\n' \"$GIT_CONFIG_GLOBAL\"\n"
             "  [ -n \"${GIT_CONFIG_NOSYSTEM+set}\" ] && printf 'GIT_CONFIG_NOSYSTEM=%s\\n' "
             "\"$GIT_CONFIG_NOSYSTEM\"\n"
             "  printf -- '--\\n'; } >> ")
         + test_support::shellQuote(log) + QStringLiteral("\nexec ")
         + test_support::shellQuote(test_support::gitExecutable()) + QStringLiteral(" \"$@\"\n"))
            .toUtf8());

    GitAdapter adapter(recorder);
    CHECK_EQ(run(adapter, repo.request(Operation::Status)).outcome, Outcome::Success);

    int calls = 0;
    QHash<QByteArray, QString> variables;
    for (const auto &line : readFile(log).split('\n')) {
        if (line != "--") {
            const auto equals = line.indexOf('=');
            if (equals > 0) variables.insert(line.left(equals), QFile::decodeName(line.mid(equals + 1)));
            continue;
        }
        SCOPE("Git call " + std::to_string(++calls));
        for (const auto &file : configurationRead(variables)) {
            SCOPE(file.toStdString());
            CHECK(!accountFiles.contains(QDir::cleanPath(file)));
            CHECK_NE(file, QStringLiteral("/etc/gitconfig"));
        }
        variables.clear();
    }
    CHECK_GT(calls, 0);
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("UserConfigIsolationTests");
    suite.run("the adapter's Git reads no configuration from the account's home or the system",
              theAdaptersGitReadsNoAccountOrSystemConfiguration);
    return suite.report();
}
