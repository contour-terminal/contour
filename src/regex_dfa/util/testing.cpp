// This file is part of the "x0" project, http://github.com/christianparpart/x0>
//   (c) 2009-2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <klex/util/AnsiColor.h>
#include <klex/util/Flags.h>
#include <klex/util/testing.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>

#if defined(_WIN32) || defined(_WIN64)
    #include <Shlwapi.h>
#else
    #include <fnmatch.h>
#endif

using namespace std;

namespace regex_dfa::util::testing
{

auto static constexpr colorsReset = AnsiColor::codes<AnsiColor::Reset>();
auto static constexpr colorsTestCaseHeader = AnsiColor::codes<AnsiColor::Cyan>();
auto static constexpr colorsError = AnsiColor::codes<AnsiColor::Red | AnsiColor::Bold>();
auto static constexpr colorsOk = AnsiColor::codes<AnsiColor::Green>();
auto static constexpr colorsLog = AnsiColor::codes<AnsiColor::Blue | AnsiColor::Bold>();

int main(int argc, const char* argv[])
{
    return UnitTest::instance()->main(argc, argv);
}

bool beginsWith(const string& str, const string_view& prefix)
{
    if (str.length() < prefix.length())
    {
        return false;
    }

    return string_view(&str[0], prefix.length()) == prefix;
}

// ############################################################################

class BailOutException
{
  public:
    BailOutException() {}
};

// ############################################################################

Environment::~Environment()
{
}

void Environment::SetUp()
{
}

void Environment::TearDown()
{
}

// ############################################################################

Test::~Test()
{
}

void Test::SetUp()
{
}

void Test::TearDown()
{
}

void Test::log(const string& message)
{
    UnitTest::instance()->log(message);
}

void Test::reportUnhandledException(const exception& e)
{
    UnitTest::instance()->reportUnhandledException(e);
}

// ############################################################################

TestInfo::TestInfo(const string& testCaseName,
                   const string& testName,
                   bool enabled,
                   unique_ptr<TestFactory>&& testFactory):
    testCaseName_(testCaseName), testName_(testName), enabled_(enabled), testFactory_(move(testFactory))
{
}

// ############################################################################

UnitTest::UnitTest():
    environments_(),
    testCases_(),
    activeTests_(),
    repeats_(1),
    printProgress_(false),
    printSummaryDetails_(true),
    currentTestCase_(nullptr),
    currentCount_(0),
    successCount_(0),
    failCount_(0)
{
}

UnitTest::~UnitTest()
{
}

UnitTest* UnitTest::instance()
{
    static UnitTest unitTest;
    return &unitTest;
}

void UnitTest::randomizeTestOrder()
{
    unsigned int seed = static_cast<unsigned int>(chrono::system_clock::now().time_since_epoch().count());

    shuffle(activeTests_.begin(), activeTests_.end(), default_random_engine(seed));
}

void UnitTest::sortTestsAlphabetically()
{
    sort(activeTests_.begin(), activeTests_.end(), [this](size_t a, size_t b) -> bool {
        TestInfo* left = testCases_[a].get();
        TestInfo* right = testCases_[b].get();

        if (left->testCaseName() < right->testCaseName())
            return true;

        if (left->testCaseName() == right->testCaseName())
            return left->testName() < right->testName();

        return false;
    });
}

bool initializeTTY()
{
#if defined(_WIN32) && defined(ENABLE_VIRTUAL_TERMINAL_PROCESSING)
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE)
        return false;

    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode))
        return false;

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(output, mode))
        return false;
#endif

    return true;
}

int UnitTest::main(int argc, const char* argv[])
{
    initializeTTY();
    // TODO: add CLI parameters (preferably gtest compatible)
    //
    // --no-color | --color   explicitely enable/disable color output
    // --filter=REGEX         filter tests by regular expression
    // --exclude=REGEX        excludes tests by regular expressions
    // --randomize            randomize test order
    // --repeats=NUMBER        repeats tests given number of times
    // --list[-tests]         Just list the tests and exit.

    Flags flags;
    flags.defineBool("help", 'h', "Prints this help and terminates.")
        .defineBool("verbose", 'v', "Prints to console in debug log level.")
        .defineString("filter", 'f', "GLOB", "Filters tests by given glob.", "*")
        .defineString("exclude", 'e', "GLOB", "Excludes tests by given glob.", "")
        .defineBool("list", 'l', "Prints all tests and exits.")
        .defineBool("randomize", 'R', "Randomizes test order.")
        .defineBool("sort", 's', "Sorts tests alphabetically ascending.")
        .defineBool("no-progress", 0, "Avoids printing progress.")
        .defineNumber("repeat", 'r', "COUNT", "Repeat tests given number of times.", 1);

    try
    {
        flags.parse(argc, argv);
    }
    catch (const exception& ex)
    {
        fprintf(stderr, "Failed to parse flags. %s\n", ex.what());
        return EXIT_FAILURE;
    }

    if (flags.getBool("help"))
    {
        printf("%s\n", flags.helpText().c_str());
        return EXIT_SUCCESS;
    }

    verbose_ = flags.getBool("verbose");

    string filter = flags.getString("filter");
    string exclude = flags.getString("exclude");
    repeats_ = flags.getNumber("repeat");
    printProgress_ = !flags.getBool("no-progress");

    if (flags.getBool("randomize"))
        randomizeTestOrder();
    else if (flags.getBool("sort"))
        sortTestsAlphabetically();

    filterTests(filter, exclude);

    if (flags.getBool("list"))
    {
        printTestList();
        return EXIT_SUCCESS;
    }

    run();

    return failCount_ == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

void UnitTest::filterTests(const string& filter, const string& exclude)
{
    // if (filter != "*") { ... }

    vector<size_t> filtered;
    for (size_t i = 0, e = activeTests_.size(); i != e; ++i)
    {
        TestInfo* testInfo = testCases_[activeTests_[i]].get();
        string matchName = fmt::format("{}.{}", testInfo->testCaseName(), testInfo->testName());

#if defined(_WIN32) || defined(_WIN64)
        if (!exclude.empty() && PathMatchSpec(matchName.c_str(), exclude.c_str()) == TRUE)
            continue; // exclude this one

        if (PathMatchSpec(matchName.c_str(), filter.c_str()) == TRUE)
            filtered.push_back(activeTests_[i]);
#else
        const int flags = 0;

        if (!exclude.empty() && fnmatch(exclude.c_str(), matchName.c_str(), flags) == 0)
            continue; // exclude this one

        if (fnmatch(filter.c_str(), matchName.c_str(), flags) == 0)
        {
            filtered.push_back(activeTests_[i]);
        }
#endif
    }
    activeTests_ = move(filtered);
}

void UnitTest::run()
{
    for (auto& env: environments_)
    {
        env->SetUp();
    }

    for (auto& init: initializers_)
    {
        init->invoke();
    }

    for (int i = 0; i < repeats_; i++)
    {
        runAllTestsOnce();
    }

    for (auto& env: environments_)
    {
        env->TearDown();
    }

    printSummary();
}

void UnitTest::printTestList()
{
    for (size_t i = 0, e = activeTests_.size(); i != e; ++i)
    {
        TestInfo* testCase = testCases_[activeTests_[i]].get();
        printf("%4zu. %s.%s\n", i + 1, testCase->testCaseName().c_str(), testCase->testName().c_str());
    }
}

void UnitTest::printSummary()
{
    // print summary
    fmt::print("{}Finished running {} tests ({} repeats). {} success, {} failed, {} disabled.{}\n",
               failCount_ ? colorsError.data() : colorsOk.data(),
               repeats_ * activeTests_.size(),
               repeats_,
               successCount_,
               failCount_,
               disabledCount(),
               colorsReset.data());

    if (printSummaryDetails_ && !failures_.empty())
    {
        printf("================================\n");
        printf(" Summary:\n");
        printf("================================\n");

        for (size_t i = 0, e = failures_.size(); i != e; ++i)
        {
            const auto& failure = failures_[i];
            fmt::print("{}{}{}\n", colorsError.data(), failure, colorsReset.data());
        }
    }
}

size_t UnitTest::enabledCount() const
{
    size_t count = 0;

    for (size_t i = 0, e = activeTests_.size(); i != e; ++i)
    {
        if (testCases_[activeTests_[i]]->isEnabled())
        {
            count++;
        }
    }

    return count;
}

size_t UnitTest::disabledCount() const
{
    size_t count = 0;

    for (size_t i = 0, e = activeTests_.size(); i != e; ++i)
    {
        if (!testCases_[activeTests_[i]]->isEnabled())
        {
            count++;
        }
    }

    return count;
}

void UnitTest::runAllTestsOnce()
{
    const size_t totalCount = repeats_ * enabledCount();

    for (size_t i = 0, e = activeTests_.size(); i != e; ++i)
    {
        TestInfo* testCase = testCases_[activeTests_[i]].get();
        unique_ptr<Test> test = testCase->createTest();

        if (!testCase->isEnabled())
            continue;

        currentTestCase_ = testCase;
        currentCount_++;
        size_t percentage = currentCount_ * 100 / totalCount;

        if (printProgress_)
        {
            fmt::print("{}{:>3} Running test: {}.{}{}\n",
                       colorsTestCaseHeader.data(),
                       percentage,
                       testCase->testCaseName(),
                       testCase->testName(),
                       colorsReset.data());
        }

        int failed = 0;

        try
        {
            test->SetUp();
        }
        catch (const BailOutException&)
        {
            // SHOULD NOT HAPPEND: complain about it
            failed++;
        }
        catch (...)
        {
            // TODO: report failure upon set-up phase, hence skipping actual test
            failed++;
        }

        if (!failed)
        {
            try
            {
                test->TestBody();
            }
            catch (const BailOutException&)
            {
                // no-op
                failed++;
            }
            catch (const exception& ex)
            {
                reportUnhandledException(ex);
                failed++;
            }
            catch (...)
            {
                reportMessage("Unhandled exception caught in test.", false);
                failed++;
            }

            try
            {
                test->TearDown();
            }
            catch (const BailOutException&)
            {
                // SHOULD NOT HAPPEND: complain about it
                failed++;
            }
            catch (...)
            {
                // TODO: report failure in tear-down
                failed++;
            }

            if (!failed)
            {
                successCount_++;
            }
        }
    }
}

void UnitTest::reportError(
    const char* fileName, int lineNo, bool fatal, const char* actual, const error_code& ec)
{
    string message = fmt::format("{}:{}: Failure\n"
                                 "  Value of: {}\n"
                                 "  Expected: success\n"
                                 "    Actual: ({}) {}\n",
                                 fileName,
                                 lineNo,
                                 actual,
                                 ec.category().name(),
                                 ec.message());

    reportMessage(message, fatal);
}

void UnitTest::reportError(const char* fileName,
                           int lineNo,
                           bool fatal,
                           const char* expected,
                           const error_code& expectedEvaluated,
                           const char* actual,
                           const error_code& actualEvaluated)
{
    string message = fmt::format("{}:{}: Failure\n"
                                 "  Value of: {}\n"
                                 "  Expected: ({}) {}\n"
                                 "    Actual: ({}) {}\n",
                                 fileName,
                                 lineNo,
                                 actual,
                                 expectedEvaluated.category().name(),
                                 expectedEvaluated.message(),
                                 actualEvaluated.category().name(),
                                 actualEvaluated.message());

    reportMessage(message, fatal);
}

void UnitTest::reportBinary(const char* fileName,
                            int lineNo,
                            bool fatal,
                            const char* expected,
                            const char* actual,
                            const string& actualEvaluated,
                            const char* op)
{
    string message = fmt::format("{}:{}: Failure\n"
                                 "  Value of: {}\n"
                                 "  Expected: {} {}\n"
                                 "    Actual: {}\n",
                                 fileName,
                                 lineNo,
                                 actual,
                                 expected,
                                 op,
                                 actualEvaluated);

    reportMessage(message, fatal);
}

void UnitTest::reportUnhandledException(const exception& e)
{
    string message = fmt::format("Unhandled Exception\n"
                                 "  Type: {}\n"
                                 "  What: {}\n",
                                 typeid(e).name(),
                                 e.what());
    reportMessage(message, false);
}

void UnitTest::reportEH(const char* fileName,
                        int lineNo,
                        bool fatal,
                        const char* program,
                        const char* expected,
                        const char* actual)
{
    string message = fmt::format("{}:{}: {}\n"
                                 "  Value of: {}\n"
                                 "  Expected: {}\n"
                                 "    Actual: {}\n",
                                 fileName,
                                 lineNo,
                                 actual ? "Unexpected exception caught" : "No exception caught",
                                 program,
                                 expected,
                                 actual);

    reportMessage(message, fatal);
}

void UnitTest::reportMessage(const char* fileName, int lineNo, bool fatal, const string& msg)
{
    string message = fmt::format("{}:{}: {}\n", fileName, lineNo, msg);
    reportMessage(message, fatal);
}

void UnitTest::reportMessage(const string& message, bool fatal)
{
    fmt::print("{}{}{}\n", colorsError.data(), message, colorsReset.data());

    failCount_++;
    failures_.emplace_back(message);

    if (fatal)
    {
        throw BailOutException();
    }
}

void UnitTest::addEnvironment(unique_ptr<Environment>&& env)
{
    environments_.emplace_back(move(env));
}

Callback* UnitTest::addInitializer(unique_ptr<Callback>&& cb)
{
    initializers_.emplace_back(move(cb));
    return initializers_.back().get();
}

TestInfo* UnitTest::addTest(const char* testCaseName,
                            const char* testName,
                            unique_ptr<TestFactory>&& testFactory)
{
    testCases_.emplace_back(
        make_unique<TestInfo>(testCaseName,
                              testName,
                              !beginsWith(testCaseName, "DISABLED_") && !beginsWith(testName, "DISABLED_"),
                              move(testFactory)));

    activeTests_.emplace_back(activeTests_.size());

    return testCases_.back().get();
}

void UnitTest::log(const string& message)
{
    if (verbose_)
    {
        size_t bol = 0;
        size_t eol = 0;
        do
        {
            eol = message.find('\n', bol);
            string line = message.substr(bol, eol - bol);
            if (eol + 1 < message.size() || (!line.empty() && line != "\n"))
            {
                fmt::print("{}{}.{}:{} {}\n",
                           colorsLog.data(),
                           currentTestCase_->testCaseName(),
                           currentTestCase_->testName(),
                           colorsReset.data(),
                           line);
            }
            bol = eol + 1;
        } while (eol != string::npos);
    }
}

} // namespace regex_dfa::util::testing
