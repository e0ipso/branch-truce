// Whether the Git that GitAdapter starts reads only the configuration its caller selected. The
// adapter takes HOME, XDG_CONFIG_HOME and the GIT_CONFIG_* selection from the GitProgram it is
// given, so a Git suite built on the fixture's environment never reads the invoking account's
// ~/.gitconfig, ~/.config/git/config or the system's /etc/gitconfig, whatever this process's own
// environment holds.
#include "TestHarness.h"
#include "git/GitFixtures.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QProcessEnvironment>

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

    GitAdapter adapter(repo.program(recorder));
    CHECK_EQ(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome, Outcome::Success);

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
void theEnvironmentSelectionKeepsAbsoluteConfigurationPathsOnly()
{
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("HOME"), QStringLiteral("/home/chosen"));
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), QStringLiteral("/home/chosen/xdg"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/home/chosen/global"));
    environment.insert(QStringLiteral("GIT_CONFIG_SYSTEM"), QStringLiteral("/etc/chosen"));
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    const auto chosen = GitEnvironment::from(environment);
    CHECK_EQ(chosen.home, QStringLiteral("/home/chosen"));
    CHECK_EQ(chosen.configHome, QStringLiteral("/home/chosen/xdg"));
    CHECK_EQ(chosen.globalConfig, QStringLiteral("/home/chosen/global"));
    CHECK_EQ(chosen.systemConfig, QStringLiteral("/etc/chosen"));
    CHECK_EQ(chosen.noSystem, QStringLiteral("1"));

    QProcessEnvironment relative;
    relative.insert(QStringLiteral("HOME"), QStringLiteral("relative/home"));
    relative.insert(QStringLiteral("XDG_CONFIG_HOME"), QStringLiteral("relative"));
    relative.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral(":/resource"));
    const auto dropped = GitEnvironment::from(relative);
    CHECK(dropped.configHome.isEmpty() && dropped.globalConfig.isEmpty() && dropped.systemConfig.isEmpty());
    CHECK(dropped.noSystem.isEmpty());
    // HOME falls back to the account's home directory.
    if (const auto *account = ::getpwuid(::geteuid()); account != nullptr)
        CHECK_EQ(dropped.home, QFile::decodeName(account->pw_dir));
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("UserConfigIsolationTests");
    suite.run("the adapter's Git reads no configuration from the account's home or the system",
              theAdaptersGitReadsNoAccountOrSystemConfiguration);
    suite.run("the environment selection keeps absolute configuration paths only",
              theEnvironmentSelectionKeepsAbsoluteConfigurationPathsOnly);
    return suite.report();
}
