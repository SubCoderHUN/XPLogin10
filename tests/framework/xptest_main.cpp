// XPLogin10 - test runner entry point.
//
// Usage: xplogin_tests [--filter <substring>] [--list] [--verbose]
#include "xptest.h"

#include <cstring>
#include <string>

namespace {

bool Matches(const xptest::TestCase& test, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    const std::string full = test.suite + "." + test.name;
    return full.find(filter) != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    std::string filter;
    bool listOnly = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (std::strcmp(argv[i], "--list") == 0) {
            listOnly = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: %s [--filter <substring>] [--list] [--verbose]\n", argv[0]);
            return 0;
        }
    }

    const auto& tests = xptest::Registry::Instance().Tests();

    if (listOnly) {
        for (const auto& test : tests) {
            std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
        }
        return 0;
    }

    int passed = 0;
    int failed = 0;
    std::string lastSuite;

    for (const auto& test : tests) {
        if (!Matches(test, filter)) {
            continue;
        }
        if (test.suite != lastSuite) {
            std::printf("\n[ %s ]\n", test.suite.c_str());
            lastSuite = test.suite;
        }

        xptest::CurrentTest::Failures().clear();
        std::string fatalMessage;
        bool crashed = false;

        try {
            test.body();
        } catch (const xptest::AssertionFailure& failure) {
            fatalMessage = failure.message;
        } catch (const std::exception& error) {
            crashed = true;
            fatalMessage =
                std::string("unexpected exception: ") + error.what();
        } catch (...) {
            crashed = true;
            fatalMessage = "unexpected non-standard exception";
        }

        const auto& softFailures = xptest::CurrentTest::Failures();
        const bool ok = fatalMessage.empty() && softFailures.empty();

        if (ok) {
            ++passed;
            if (verbose) {
                std::printf("  PASS  %s\n", test.name.c_str());
            }
        } else {
            ++failed;
            std::printf("  FAIL  %s\n", test.name.c_str());
            for (const auto& failure : softFailures) {
                std::printf("    at %s\n", failure.c_str());
            }
            if (!fatalMessage.empty()) {
                std::printf("    %s%s\n", crashed ? "" : "at ", fatalMessage.c_str());
            }
        }
    }

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed, %d total\n", passed, failed,
                passed + failed);
    if (!filter.empty()) {
        std::printf("(filter: \"%s\")\n", filter.c_str());
    }
    return failed == 0 ? 0 : 1;
}
