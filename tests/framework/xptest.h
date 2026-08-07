// XPLogin10 - a small, dependency-free test framework.
//
// Deliberately not gtest/Catch2: the test suite has to build on a machine with
// no package manager and no network, next to a credential provider that must
// not acquire third-party dependencies. Roughly 200 lines buys registration,
// assertions with useful failure text, filtering and a CTest-friendly exit code.
#pragma once

#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace xptest {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

class Registry {
public:
    static Registry& Instance() {
        static Registry registry;
        return registry;
    }
    void Add(TestCase test) { tests_.push_back(std::move(test)); }
    const std::vector<TestCase>& Tests() const { return tests_; }

private:
    std::vector<TestCase> tests_;
};

// Thrown by a failing assertion; caught by the runner.
struct AssertionFailure : std::exception {
    std::string message;
    explicit AssertionFailure(std::string m) : message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

// Non-fatal check failures are collected and reported at the end of the test.
struct CurrentTest {
    static std::vector<std::string>& Failures() {
        static std::vector<std::string> failures;
        return failures;
    }
};

// ---------------------------------------------------------------------------
// Value printing
// ---------------------------------------------------------------------------

inline std::string Describe(const std::string& v) { return "\"" + v + "\""; }
inline std::string Describe(const char* v) {
    return v ? std::string("\"") + v + "\"" : std::string("(null)");
}
inline std::string Describe(bool v) { return v ? "true" : "false"; }

inline std::string Describe(const std::wstring& v) {
    // The tests are full of wide strings; render the ASCII subset and escape
    // anything else so a failure message stays readable.
    std::string out = "L\"";
    for (wchar_t c : v) {
        if (c >= 32 && c < 127) {
            out.push_back(static_cast<char>(c));
        } else {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "\\u%04X",
                          static_cast<unsigned>(c) & 0xFFFFu);
            out += buffer;
        }
    }
    out.push_back('"');
    return out;
}

inline std::string Describe(const std::u16string& v) {
    std::string out = "u\"";
    for (char16_t c : v) {
        if (c >= 32 && c < 127) {
            out.push_back(static_cast<char>(c));
        } else {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(c));
            out += buffer;
        }
    }
    out.push_back('"');
    return out;
}

// Byte buffers (asset packs, PNG output) compare as a whole but would print as
// a wall of numbers, so summarise: length plus a short hex prefix, which is
// enough to see whether two buffers diverge at the start or the end.
inline std::string Describe(const std::vector<unsigned char>& bytes) {
    std::string out = std::to_string(bytes.size()) + " bytes";
    if (bytes.empty()) {
        return out;
    }
    out += " [";
    const size_t shown = bytes.size() < 12 ? bytes.size() : 12;
    for (size_t i = 0; i < shown; ++i) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%s%02X", i ? " " : "", bytes[i]);
        out += buffer;
    }
    out += bytes.size() > shown ? " ...]" : "]";
    return out;
}

template <typename T>
inline std::string Describe(const T& value) {
    // Enums (UiState, AuthStatus, ...) have no operator<<, and adding one to
    // production headers just for tests would be the tail wagging the dog.
    if constexpr (std::is_enum_v<T>) {
        return std::to_string(
            static_cast<long long>(static_cast<std::underlying_type_t<T>>(value)));
    } else {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }
}

} // namespace xptest

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

#define XPTEST_CONCAT_IMPL(a, b) a##b
#define XPTEST_CONCAT(a, b) XPTEST_CONCAT_IMPL(a, b)

#define TEST(suite_name, test_name)                                              \
    static void XPTEST_CONCAT(xptest_body_, __LINE__)();                         \
    namespace {                                                                  \
    struct XPTEST_CONCAT(XpTestRegistrar_, __LINE__) {                           \
        XPTEST_CONCAT(XpTestRegistrar_, __LINE__)() {                            \
            ::xptest::Registry::Instance().Add(                                  \
                {#suite_name, #test_name, &XPTEST_CONCAT(xptest_body_, __LINE__)}); \
        }                                                                        \
    } XPTEST_CONCAT(xptest_registrar_, __LINE__);                                \
    }                                                                            \
    static void XPTEST_CONCAT(xptest_body_, __LINE__)()

// ---------------------------------------------------------------------------
// Assertions. CHECK_* records and continues, REQUIRE_* aborts the test.
// ---------------------------------------------------------------------------

#define XPTEST_FAIL_MESSAGE(text)                                                \
    (std::string(__FILE__) + ":" + std::to_string(__LINE__) + "\n      " + (text))

#define XPTEST_REPORT(fatal, text)                                               \
    do {                                                                         \
        const std::string xptest_msg = XPTEST_FAIL_MESSAGE(text);                \
        if (fatal) {                                                             \
            throw ::xptest::AssertionFailure(xptest_msg);                        \
        }                                                                        \
        ::xptest::CurrentTest::Failures().push_back(xptest_msg);                 \
    } while (0)

#define XPTEST_BINARY(fatal, actual, op, expected)                               \
    do {                                                                         \
        const auto& xptest_a = (actual);                                         \
        const auto& xptest_b = (expected);                                       \
        if (!(xptest_a op xptest_b)) {                                           \
            XPTEST_REPORT(fatal,                                                 \
                          std::string(#actual " " #op " " #expected) +           \
                              "\n        actual:   " +                           \
                              ::xptest::Describe(xptest_a) +                     \
                              "\n        expected: " +                           \
                              ::xptest::Describe(xptest_b));                     \
        }                                                                        \
    } while (0)

#define XPTEST_TRUTH(fatal, condition, wanted)                                   \
    do {                                                                         \
        if (static_cast<bool>(condition) != (wanted)) {                          \
            XPTEST_REPORT(fatal, std::string(#condition) + " was " +             \
                                     (wanted ? "false" : "true"));               \
        }                                                                        \
    } while (0)

#define CHECK(condition)        XPTEST_TRUTH(false, condition, true)
#define CHECK_FALSE(condition)  XPTEST_TRUTH(false, condition, false)
#define REQUIRE(condition)      XPTEST_TRUTH(true, condition, true)
#define REQUIRE_FALSE(condition) XPTEST_TRUTH(true, condition, false)

#define CHECK_EQ(a, b)   XPTEST_BINARY(false, a, ==, b)
#define CHECK_NE(a, b)   XPTEST_BINARY(false, a, !=, b)
#define CHECK_LT(a, b)   XPTEST_BINARY(false, a, <, b)
#define CHECK_LE(a, b)   XPTEST_BINARY(false, a, <=, b)
#define CHECK_GT(a, b)   XPTEST_BINARY(false, a, >, b)
#define CHECK_GE(a, b)   XPTEST_BINARY(false, a, >=, b)

#define REQUIRE_EQ(a, b) XPTEST_BINARY(true, a, ==, b)
#define REQUIRE_NE(a, b) XPTEST_BINARY(true, a, !=, b)
#define REQUIRE_LT(a, b) XPTEST_BINARY(true, a, <, b)
#define REQUIRE_LE(a, b) XPTEST_BINARY(true, a, <=, b)
#define REQUIRE_GT(a, b) XPTEST_BINARY(true, a, >, b)
#define REQUIRE_GE(a, b) XPTEST_BINARY(true, a, >=, b)

#define CHECK_NEAR(a, b, tolerance)                                              \
    do {                                                                         \
        const double xptest_diff =                                               \
            static_cast<double>(a) - static_cast<double>(b);                     \
        const double xptest_abs = xptest_diff < 0 ? -xptest_diff : xptest_diff;  \
        if (xptest_abs > static_cast<double>(tolerance)) {                       \
            XPTEST_REPORT(false, std::string(#a " ~= " #b) +                     \
                                     "\n        actual:   " +                    \
                                     ::xptest::Describe(a) +                     \
                                     "\n        expected: " +                    \
                                     ::xptest::Describe(b) +                     \
                                     "\n        tolerance: " +                   \
                                     ::xptest::Describe(tolerance));             \
        }                                                                        \
    } while (0)

#define FAIL(text) XPTEST_REPORT(true, std::string(text))
