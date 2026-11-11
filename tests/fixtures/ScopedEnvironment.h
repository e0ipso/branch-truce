#pragma once

// Applies a fixture's environment to this process while it lives, for code in this process that
// still reads its environment: the Git that session tasks run and the default Application config.
// Each listed variable the fixture leaves out is unset meanwhile, and every one is restored after.

#include <QByteArray>
#include <QMap>
#include <QProcessEnvironment>
#include <QString>

#include <optional>

namespace test_support {

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const QProcessEnvironment &environment)
    {
        for (const auto name : {"HOME", "PATH", "LC_ALL", "TMPDIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                "XDG_DATA_HOME", "XDG_RUNTIME_DIR", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL",
                                "GIT_CONFIG_SYSTEM", "GIT_TERMINAL_PROMPT"}) {
            previous_.insert(name,
                             qEnvironmentVariableIsSet(name) ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt);
            const auto key = QString::fromLatin1(name);
            if (environment.contains(key))
                qputenv(name, environment.value(key).toUtf8());
            else
                qunsetenv(name);
        }
    }
    ~ScopedEnvironment()
    {
        for (auto entry = previous_.cbegin(); entry != previous_.cend(); ++entry) {
            if (entry.value())
                qputenv(entry.key().constData(), *entry.value());
            else
                qunsetenv(entry.key().constData());
        }
    }
    ScopedEnvironment(const ScopedEnvironment &) = delete;
    ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

private:
    QMap<QByteArray, std::optional<QByteArray>> previous_;
};

} // namespace test_support
