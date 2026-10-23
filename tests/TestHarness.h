#pragma once

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace test_support {
inline int assertions = 0;

inline void check(bool condition, const char *expression, const char *file, int line)
{
    ++assertions;
    if (!condition) {
        throw std::runtime_error(std::string(file) + ':' + std::to_string(line) + ": " + expression);
    }
}

class Suite {
public:
    explicit Suite(const char *name) : name_(name) {}
    template<typename Test>
    void run(const std::string &name, Test test)
    {
        ++tests_;
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failures_;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    int report() const
    {
        std::cout << "RESULT " << name_ << " tests=" << tests_ << " assertions=" << assertions
                  << " failures=" << failures_ << '\n';
        return failures_ == 0 ? 0 : 1;
    }
private:
    const char *name_;
    int tests_ = 0;
    int failures_ = 0;
};
}

#define CHECK(...) ::test_support::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__, __LINE__)
