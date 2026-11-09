#pragma once

// The test harness every suite shares. It has no dependencies beyond the standard library, so
// the engine tests stay Qt-free.
//
//   CHECK(condition)            fails the case at once by throwing; use it in the case body.
//   CHECK_EQ(actual, expected)  and CHECK_NE/LT/LE/GT/GE print both values when they fail.
//   EXPECT(condition)           and EXPECT_EQ record a failure without throwing; use them in Qt
//                               slots, timer callbacks, other threads and syscall wrappers, where
//                               an exception must not unwind. The case fails when it returns, and
//                               a wait can stop early on failurePending().
//   SCOPE(label)                names the row or step a failure belongs to.
//   test_support::skip(reason)  ends the case as skipped. A suite whose every case skipped
//                               reports 77, which CTest's SKIP_RETURN_CODE turns into "skipped".
//
// Cases are selected by name with `--filter <text>` (after test_support::configure) or the
// BRANCH_TRUCE_TEST_FILTER environment variable, and listed with `--list`.

#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace test_support {
inline std::atomic<int> assertions = 0;

// Thrown by CHECK and friends. Suite::run reports it as the case's failure.
class Failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown by skip(). Suite::run reports the case as skipped, not failed.
class Skipped : public std::exception {
public:
    explicit Skipped(std::string reason) : reason_(std::move(reason)) {}
    const char *what() const noexcept override { return reason_.c_str(); }

private:
    std::string reason_;
};

[[noreturn]] inline void skip(const std::string &reason) { throw Skipped(reason); }

namespace detail {
// The SCOPE labels active on this thread, outermost first.
inline std::vector<std::string> &scopes()
{
    thread_local std::vector<std::string> active;
    return active;
}

inline std::string context()
{
    const auto &active = scopes();
    if (active.empty()) return {};
    std::string text = " [in ";
    for (std::size_t i = 0; i < active.size(); ++i) {
        if (i > 0) text += " > ";
        text += active[i];
    }
    return text + ']';
}

inline std::string located(const char *file, int line, const std::string &message)
{
    return std::string(file) + ':' + std::to_string(line) + ": " + message + context();
}

// Failures recorded by EXPECT, from any thread, since the current case started.
struct Pending {
    std::mutex mutex;
    std::vector<std::string> messages;
};
inline Pending &pending()
{
    static Pending shared;
    return shared;
}

inline std::string takePending()
{
    auto &shared = pending();
    const std::lock_guard lock(shared.mutex);
    std::string joined;
    for (const auto &message : shared.messages) joined += (joined.empty() ? "" : "\n    ") + message;
    shared.messages.clear();
    return joined;
}
} // namespace detail

// Labels the failures raised while it lives, for example the row of a table.
class Scope {
public:
    explicit Scope(std::string label) { detail::scopes().push_back(std::move(label)); }
    ~Scope() { detail::scopes().pop_back(); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};

// Quotes text with every byte outside printable ASCII escaped, so binary and
// non-UTF-8 values show exactly which bytes differ.
inline std::string quoted(std::string_view text)
{
    constexpr std::size_t shown = 400;
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size() && i < shown; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        switch (byte) {
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\0': out += "\\0"; break;
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:
            if (byte >= 0x20 && byte < 0x7f) {
                out += static_cast<char>(byte);
            } else {
                constexpr std::string_view digits = "0123456789abcdef";
                out += "\\x";
                out += digits[static_cast<std::size_t>(byte >> 4)];
                out += digits[static_cast<std::size_t>(byte & 0xf)];
            }
        }
    }
    out += '"';
    if (text.size() > shown)
        out += " (" + std::to_string(text.size()) + " bytes, first " + std::to_string(shown) + " shown)";
    return out;
}

// How CHECK_EQ prints a value. Specialize Describe<T> to print a type another way.
template <typename T> struct Describe;

template <typename T> std::string describe(const T &value) { return Describe<T>::of(value); }

template <typename T> struct Describe {
    static std::string of(const T &value)
    {
        if constexpr (std::is_same_v<T, bool>) {
            return value ? "true" : "false";
        } else if constexpr (std::is_same_v<T, char>) {
            return quoted(std::string_view(&value, 1));
        } else if constexpr (std::is_enum_v<T>) {
            return "enum " + std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<T>>(value)));
        } else if constexpr (std::is_arithmetic_v<T>) {
            std::ostringstream out;
            out << +value;
            return out.str();
        } else if constexpr (std::is_pointer_v<T> && std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>) {
            return value == nullptr ? std::string("nullptr") : quoted(std::string_view(value));
        } else if constexpr (std::is_convertible_v<const T &, std::string_view>) {
            return quoted(std::string_view(value));
        } else if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
            if (value == nullptr) return "nullptr";
            std::ostringstream out;
            out << static_cast<const void *>(value);
            return out.str();
        } else if constexpr (requires { value.toStdString(); }) {
            // QByteArray and QString, without including Qt here.
            return quoted(value.toStdString());
        } else if constexpr (requires {
                                 value.has_value();
                                 *value;
                             }) {
            return value.has_value() ? describe(*value) : std::string("empty");
        } else if constexpr (requires(std::ostream &out) { out << value; }) {
            std::ostringstream out;
            out << value;
            return out.str();
        } else if constexpr (requires {
                                 std::begin(value);
                                 std::end(value);
                             }) {
            constexpr std::size_t shown = 20;
            std::string out = "[";
            std::size_t count = 0;
            for (const auto &element : value) {
                if (count == shown) {
                    out += ", ...";
                    break;
                }
                out += (count++ == 0 ? "" : ", ") + describe(element);
            }
            return out + ']';
        } else {
            return "<value without a printer>";
        }
    }
};

template <typename T, std::size_t N> struct Describe<T[N]> {
    static std::string of(const T (&value)[N])
    {
        if constexpr (std::is_same_v<std::remove_cv_t<T>, char>) {
            return quoted(std::string_view(value));
        } else {
            return describe(std::vector<T>(std::begin(value), std::end(value)));
        }
    }
};

namespace detail {
template <typename T>
concept StrictInteger =
    std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, char8_t>
    && !std::same_as<T, char16_t> && !std::same_as<T, char32_t> && !std::same_as<T, wchar_t>;

enum class Relation { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

inline const char *symbol(Relation relation)
{
    switch (relation) {
    case Relation::Equal: return "==";
    case Relation::NotEqual: return "!=";
    case Relation::Less: return "<";
    case Relation::LessEqual: return "<=";
    case Relation::Greater: return ">";
    case Relation::GreaterEqual: return ">=";
    }
    return "?";
}

// Compares mixed signed and unsigned integers by value, without sign-conversion warnings.
template <typename A, typename B> bool holds(Relation relation, const A &a, const B &b)
{
    if constexpr (StrictInteger<A> && StrictInteger<B>) {
        switch (relation) {
        case Relation::Equal: return std::cmp_equal(a, b);
        case Relation::NotEqual: return std::cmp_not_equal(a, b);
        case Relation::Less: return std::cmp_less(a, b);
        case Relation::LessEqual: return std::cmp_less_equal(a, b);
        case Relation::Greater: return std::cmp_greater(a, b);
        case Relation::GreaterEqual: return std::cmp_greater_equal(a, b);
        }
        return false;
    } else {
        switch (relation) {
        case Relation::Equal: return static_cast<bool>(a == b);
        case Relation::NotEqual: return static_cast<bool>(!(a == b));
        case Relation::Less: return static_cast<bool>(a < b);
        case Relation::LessEqual: return static_cast<bool>(!(b < a));
        case Relation::Greater: return static_cast<bool>(b < a);
        case Relation::GreaterEqual: return static_cast<bool>(!(a < b));
        }
        return false;
    }
}

// Empty when the relation holds, otherwise the message that names both values.
template <typename A, typename B>
std::optional<std::string> compare(Relation relation, const A &a, const B &b, const char *left, const char *right)
{
    ++assertions;
    if (holds(relation, a, b)) return std::nullopt;
    return std::string(left) + ' ' + symbol(relation) + ' ' + right + "\n      actual: " + describe(a)
        + "\n    expected: " + symbol(relation) + ' ' + describe(b);
}
} // namespace detail

inline void check(bool condition, const char *expression, const char *file, int line)
{
    ++assertions;
    if (!condition) throw Failure(detail::located(file, line, expression));
}

// Records a failure without throwing and returns whether the condition held.
inline bool expect(bool condition, const char *expression, const char *file, int line)
{
    ++assertions;
    if (condition) return true;
    auto &shared = detail::pending();
    const std::lock_guard lock(shared.mutex);
    shared.messages.push_back(detail::located(file, line, expression));
    return false;
}

template <typename A, typename B>
void checkRelation(detail::Relation relation, const A &a, const B &b, const char *left, const char *right,
                   const char *file, int line)
{
    if (const auto message = detail::compare(relation, a, b, left, right))
        throw Failure(detail::located(file, line, *message));
}

template <typename A, typename B>
bool expectRelation(detail::Relation relation, const A &a, const B &b, const char *left, const char *right,
                    const char *file, int line)
{
    const auto message = detail::compare(relation, a, b, left, right);
    if (!message) return true;
    auto &shared = detail::pending();
    const std::lock_guard lock(shared.mutex);
    shared.messages.push_back(detail::located(file, line, *message));
    return false;
}

// True once an EXPECT has failed in the current case. Waits use it to stop early.
inline bool failurePending()
{
    auto &shared = detail::pending();
    const std::lock_guard lock(shared.mutex);
    return !shared.messages.empty();
}

// Throws the failures EXPECT recorded, so the case stops where a wait returns.
inline void throwPending()
{
    if (auto messages = detail::takePending(); !messages.empty()) throw Failure(messages);
}

// Which cases run. configure() reads --filter and --list; without it the filter comes from
// BRANCH_TRUCE_TEST_FILTER, so every test executable can be narrowed without code changes.
struct Options {
    std::string filter;
    bool list = false;
};

inline Options &options()
{
    static Options current = [] {
        Options initial;
        if (const char *filter = std::getenv("BRANCH_TRUCE_TEST_FILTER")) initial.filter = filter;
        return initial;
    }();
    return current;
}

// Takes the harness options out of argv and returns the remaining arguments, program name first.
inline std::vector<std::string> configure(int argc, char **argv)
{
    std::vector<std::string> remaining;
    for (int i = 0; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--filter" && i + 1 < argc) {
            options().filter = argv[++i];
        } else if (argument.starts_with("--filter=")) {
            options().filter = std::string(argument.substr(9));
        } else if (argument == "--list") {
            options().list = true;
        } else {
            remaining.emplace_back(argument);
        }
    }
    return remaining;
}

class Suite {
public:
    explicit Suite(const char *name) : name_(name) {}
    template <typename Test> void run(const std::string &name, Test test)
    {
        const auto &selected = options();
        if (!selected.filter.empty() && name.find(selected.filter) == std::string::npos) return;
        if (selected.list) {
            std::cout << name << '\n';
            return;
        }
        ++tests_;
        detail::takePending();
        std::string failure;
        try {
            test();
        } catch (const Skipped &skipped) {
            failure = detail::takePending();
            if (failure.empty()) {
                ++skipped_;
                std::cout << "SKIP " << name << ": " << skipped.what() << '\n';
                return;
            }
        } catch (const std::exception &error) {
            failure = error.what();
        } catch (...) {
            failure = "unknown exception";
        }
        if (const auto deferred = detail::takePending(); !deferred.empty())
            failure += (failure.empty() ? "" : "\n    ") + deferred;
        if (failure.empty()) {
            std::cout << "PASS " << name << '\n';
            return;
        }
        ++failures_;
        std::cerr << "FAIL " << name << ": " << failure << '\n';
    }
    // 0 when every case that ran passed, 77 when every one skipped, 1 otherwise. A filter that
    // selects nothing is a failure, so a mistyped name cannot pass.
    int report() const
    {
        if (options().list) return 0;
        const auto stray = detail::takePending();
        if (!stray.empty()) std::cerr << "FAIL outside any case: " << stray << '\n';
        std::cout << "RESULT " << name_ << " tests=" << tests_ << " assertions=" << assertions.load()
                  << " failures=" << failures_ << " skipped=" << skipped_ << '\n';
        if (failures_ > 0 || !stray.empty()) return 1;
        if (tests_ == 0 && !options().filter.empty()) {
            std::cerr << "no case in " << name_ << " matches the filter \"" << options().filter << "\"\n";
            return 1;
        }
        return tests_ > 0 && skipped_ == tests_ ? 77 : 0;
    }

private:
    const char *name_;
    int tests_ = 0;
    int failures_ = 0;
    int skipped_ = 0;
};
} // namespace test_support

#define TEST_SUPPORT_JOIN_(a, b) a##b
#define TEST_SUPPORT_JOIN(a, b) TEST_SUPPORT_JOIN_(a, b)

#define CHECK(...) ::test_support::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__, __LINE__)
#define EXPECT(...) ::test_support::expect(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__, __LINE__)
#define SCOPE(label) const ::test_support::Scope TEST_SUPPORT_JOIN(test_support_scope_, __LINE__)(label)

#define TEST_SUPPORT_RELATION(kind, relation, a, b)                                                                    \
    ::test_support::kind##Relation(::test_support::detail::Relation::relation, (a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_EQ(a, b) TEST_SUPPORT_RELATION(check, Equal, a, b)
#define CHECK_NE(a, b) TEST_SUPPORT_RELATION(check, NotEqual, a, b)
#define CHECK_LT(a, b) TEST_SUPPORT_RELATION(check, Less, a, b)
#define CHECK_LE(a, b) TEST_SUPPORT_RELATION(check, LessEqual, a, b)
#define CHECK_GT(a, b) TEST_SUPPORT_RELATION(check, Greater, a, b)
#define CHECK_GE(a, b) TEST_SUPPORT_RELATION(check, GreaterEqual, a, b)
#define EXPECT_EQ(a, b) TEST_SUPPORT_RELATION(expect, Equal, a, b)
#define EXPECT_NE(a, b) TEST_SUPPORT_RELATION(expect, NotEqual, a, b)
