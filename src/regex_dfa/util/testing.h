// This file is part of the "x0" project, http://github.com/christianparpart/x0>
//   (c) 2009-2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#pragma once

#include <fmt/format.h>

#include <string>
#include <memory>
#include <vector>

namespace regex_dfa::util::testing {

#define TEST_ENV_SETUP(Name)                                                  \
  class _CALLBACK_NAME(Name) : public ::regex_dfa::util::testing::Callback {       \
   public:                                                                    \
    void invoke() override;                                                   \
   private:                                                                   \
    static ::regex_dfa::util::testing::Callback* const ref_ [[maybe_unused]];      \
  };                                                                          \
                                                                              \
  ::regex_dfa::util::testing::Callback* const                                      \
  _CALLBACK_NAME(Name)::ref_ =                                                \
      ::regex_dfa::util::testing::UnitTest::instance()->addInitializer(            \
          std::make_unique<_CALLBACK_NAME>(Name));                            \
                                                                              \
  void _CALLBACK_NAME(Name)::invoke()

#define _CALLBACK_NAME(Name) Callback_##Name

#define TEST_ENV_TEARDOWN(Name) // TODO

#define TEST_ENV_F(EnvName)                                                   \
  ::regex_dfa::util::testing::UnitTest::instance()->addEnvironment(                \
      std::unique_ptr<::regex_dfa::util::testing::Environment>(EnvName));

// ############################################################################

#define TEST(testCase, testName) _CREATE_TEST(testCase, testName, ::regex_dfa::util::testing::Test)
#define TEST_F(testFixture, testName) _CREATE_TEST(testFixture, testName, testFixture)

#define EXPECT_EQ(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, false, expected, actual, ==)

#define EXPECT_NE(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, false, expected, actual, !=)

#define EXPECT_GE(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, false, expected, actual, >=)

#define EXPECT_LE(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, false, expected, actual, <=)

#define EXPECT_GT(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, false, expected, actual, >)

#define EXPECT_LT(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, false, expected, actual, <)

#define EXPECT_TRUE(actual) \
  _EXPECT_BOOLEAN(__FILE__, __LINE__, false, true, actual)

#define EXPECT_FALSE(actual) \
  _EXPECT_BOOLEAN(__FILE__, __LINE__, false, false, actual)

#define EXPECT_NEAR(expected, actual, diff)       // TODO

#define REPORT_ERROR(message)                                                 \
	do {                                                                      \
		::regex_dfa::util::testing::UnitTest::instance()->reportMessage(           \
			__FILE__, __LINE__, false, (message));                            \
	} while (0)

#define EXPECT_ERROR_CODE_SUCCESS(errorCode)                                  \
  if (errorCode) {                                                            \
    ::regex_dfa::util::testing::UnitTest::instance()->reportError(                 \
        __FILE__, __LINE__, false, #errorCode, errorCode);                    \
  }

#define EXPECT_ERROR_CODE(expected, actual)                                   \
  do {                                                                        \
    std::error_code actual_ {(actual)};                                       \
    if (actual_ != (expected)) {                                              \
      ::regex_dfa::util::testing::UnitTest::instance()->reportError(               \
          __FILE__, __LINE__, false,                                          \
          #expected, (expected),                                              \
          #actual, actual_);                                                  \
    }                                                                         \
  } while (0)

#define EXPECT_THROW(program, ExceptionType)                                  \
  do {                                                                        \
    try {                                                                     \
      program;                                                                \
      ::regex_dfa::util::testing::UnitTest::instance()->reportEH(                  \
          __FILE__, __LINE__, false, #program, #ExceptionType,                \
          "<no exception thrown>");                                           \
    } catch (const ExceptionType&) {                                          \
      break; \
    } catch (...) { \
      ::regex_dfa::util::testing::UnitTest::instance()->reportEH(                  \
          __FILE__, __LINE__, false, #program, #ExceptionType, "<foreign>");  \
    }                                                                         \
  } while (0)

#define EXPECT_ANY_THROW(program)                                             \
  do {                                                                        \
    try {                                                                     \
      program;                                                                \
      ::regex_dfa::util::testing::UnitTest::instance()->reportEH(                  \
          __FILE__, __LINE__, false, #program, "<any exception>",             \
          "<no exception thrown>");                                           \
    } catch (...) {                                                           \
    }                                                                         \
  } while (0)

// ############################################################################

#define ASSERT_EQ(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, true, expected, actual, ==)

#define ASSERT_NE(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, true, expected, actual, !=)

#define ASSERT_GE(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, true, expected, actual, >=)

#define ASSERT_LE(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, true, expected, actual, <=)

#define ASSERT_GT(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, true, expected, actual, >)

#define ASSERT_LT(expected, actual) \
  _EXPECT_BINARY(__FILE__, __LINE__, true, expected, actual, <)

#define ASSERT_TRUE(actual) \
  _EXPECT_BOOLEAN(__FILE__, __LINE__, true, true, actual)

#define ASSERT_FALSE(actual) \
  _EXPECT_BOOLEAN(__FILE__, __LINE__, true, false, actual)

#define ASSERT_NEAR(expected, actual, diff)       // TODO

#define ASSERT_ERROR_CODE_SUCCESS(errorCode)                                  \
  if (errorCode) {                                                            \
    ::regex_dfa::util::testing::UnitTest::instance()->reportError(                 \
        __FILE__, __LINE__, true, #errorCode, errorCode);                     \
  }

#define ASSERT_ERROR_CODE(expected, actual)                                   \
  do {                                                                        \
    std::error_code actual_ {(actual)};                                       \
    if (actual_ != (expected)) {                                              \
      ::regex_dfa::util::testing::UnitTest::instance()->reportError(               \
          __FILE__, __LINE__, true,                                           \
          #expected, (expected),                                              \
          #actual, actual_);                                                  \
    }                                                                         \
  } while (0)

#define ASSERT_THROW(program, ExceptionType)                                  \
  do {                                                                        \
    try {                                                                     \
      program;                                                                \
      ::regex_dfa::util::testing::UnitTest::instance()->reportEH(                  \
          __FILE__, __LINE__, true, #program, #ExceptionType,                 \
          "<no exception thrown>");                                           \
    } catch (const ExceptionType&) {                                          \
      break; \
    } catch (...) { \
      ::regex_dfa::util::testing::UnitTest::instance()->reportEH(                  \
          __FILE__, __LINE__, true, #program, #ExceptionType, "<foreign>");   \
    }                                                                         \
  } while (0)

#define ASSERT_ANY_THROW(program)                                             \
  do {                                                                        \
    try {                                                                     \
      program;                                                                \
      ::regex_dfa::util::testing::UnitTest::instance()->reportEH(                  \
          __FILE__, __LINE__, true, #program, "<any exception>",              \
          "<no exception thrown>");                                           \
    } catch (...) {                                                           \
    }                                                                         \
  } while (0)

// ############################################################################

#define _EXPECT_BOOLEAN(fileName, lineNo, fatal, expected, actual)            \
  do {                                                                        \
    bool actualEvaluated = !! (actual);                                       \
    bool failed = (expected && !actualEvaluated)                              \
               || (!expected && actualEvaluated);                             \
    if (failed) {                                                             \
      ::regex_dfa::util::testing::UnitTest::instance()->reportBinary(              \
          __FILE__, __LINE__, fatal, #expected, #actual,                      \
          ::fmt::format("{}", (actualEvaluated)), "");                        \
    } \
  } while (0)

#define _EXPECT_BINARY(fileName, lineNo, fatal, expected, actual, op)         \
  do {                                                                        \
    auto actual_ = (actual);                                                  \
    if (!((expected) op (actual_))) {                                         \
    ::regex_dfa::util::testing::UnitTest::instance()->reportBinary(                \
        __FILE__, __LINE__, fatal, #expected, #actual,                        \
        ::fmt::format("{}", actual_), #op);                                   \
    }                                                                         \
  } while (0)

#define _TEST_CLASS_NAME(testCaseName, testName) \
  Test_##testCaseName##testName

#define _CREATE_TEST(testCaseName, testName, ParentClass)                     \
class _TEST_CLASS_NAME(testCaseName, testName) : public ParentClass {         \
 public:                                                                      \
  _TEST_CLASS_NAME(testCaseName, testName)() {}                               \
                                                                              \
 private:                                                                     \
  virtual void TestBody();                                                    \
                                                                              \
  static ::regex_dfa::util::testing::TestInfo* const test_info_;                   \
};                                                                            \
                                                                              \
::regex_dfa::util::testing::TestInfo* const                                        \
_TEST_CLASS_NAME(testCaseName, testName)::test_info_ =                        \
    ::regex_dfa::util::testing::UnitTest::instance()->addTest(                     \
        #testCaseName, #testName,                                             \
        std::make_unique<                                                     \
            ::regex_dfa::util::testing::TestFactoryTemplate<                       \
                _TEST_CLASS_NAME(testCaseName, testName)>>());                \
                                                                              \
void _TEST_CLASS_NAME(testCaseName, testName)::TestBody()

// ############################################################################

int main(int argc, const char* argv[]);

// ############################################################################

class Callback {
 public:
  virtual ~Callback() {}

  virtual void invoke() = 0;
};

/**
 * Environment hooks.
 */
class Environment {
 public:
  virtual ~Environment();

  virtual void SetUp();
  virtual void TearDown();
};

/**
 * interface to a single test.
 */
class Test {
 public:
  virtual ~Test();

  virtual void SetUp();
  virtual void TestBody() = 0;
  virtual void TearDown();

  void log(const std::string& message);

  template<typename... Args>
  void logf(const char* fmt, Args... args);

  void reportUnhandledException(const std::exception& e);
};

/**
 * API to create one kind of a test.
 */
class TestFactory {
  TestFactory(const TestFactory&) = delete;
  TestFactory& operator=(const TestFactory&) = delete;

 public:
  TestFactory() {}
  virtual ~TestFactory() {}
  virtual std::unique_ptr<Test> createTest() = 0;
};

template<typename TheTestClass>
class TestFactoryTemplate : public TestFactory {
 public:
  std::unique_ptr<Test> createTest() override {
    return std::make_unique<TheTestClass>();
  }
};

/**
 * TestInfo describes a single test.
 */
class TestInfo {
  TestInfo(const TestInfo&) = delete;
  TestInfo& operator=(const TestInfo&) = delete;

 public:
  TestInfo(const std::string& testCaseName, 
           const std::string& testName,
           bool enabled,
           std::unique_ptr<TestFactory>&& testFactory);

  const std::string& testCaseName() const { return testCaseName_; }
  const std::string& testName() const { return testName_; }
  bool isEnabled() const { return enabled_; }

  std::unique_ptr<Test> createTest() { return testFactory_->createTest(); }

 private:
  std::string testCaseName_;
  std::string testName_;
  bool enabled_;
  std::unique_ptr<TestFactory> testFactory_;
};

class UnitTest {
 public:
  UnitTest();
  ~UnitTest();

  static UnitTest* instance();

  int main(int argc, const char* argv[]);

  void randomizeTestOrder();
  void sortTestsAlphabetically();
  void printTestList();
  void filterTests(const std::string& filter, const std::string& exclude);
  void run();

  void addEnvironment(std::unique_ptr<Environment>&& env);

  Callback* addInitializer(std::unique_ptr<Callback>&& cb);

  TestInfo* addTest(const char* testCaseName,
                    const char* testName,
                    std::unique_ptr<TestFactory>&& testFactory);

  void reportError(const char* fileName,
                   int lineNo,
                   bool fatal,
                   const char* actual,
                   const std::error_code& ec);

  void reportError(const char* fileName,
                   int lineNo,
                   bool fatal,
                   const char* expected,
                   const std::error_code& expectedEvaluated,
                   const char* actual,
                   const std::error_code& actualEvaluated);

  void reportBinary(const char* fileName,
                    int lineNo,
                    bool fatal,
                    const char* expected,
                    const char* actual,
                    const std::string& actualEvaluated,
                    const char* op);

  void reportUnhandledException(const std::exception& e);

  void reportEH(const char* fileName,
                int lineNo,
                bool fatal,
                const char* program,
                const char* expected,
                const char* actual);

  void reportMessage(const std::string& message, bool fatal);
  void reportMessage(const char* fileName, int lineNo, bool fatal, const std::string& message);

  void log(const std::string& message);

  template<typename ... Args>
  void logf(const char* format, Args... args) {
    log(fmt::format(format, args...));
  }

 private:
  void runAllTestsOnce();
  void printSummary();
  size_t enabledCount() const;
  size_t disabledCount() const;

 private:
  std::vector<std::unique_ptr<Environment>> environments_;
  std::vector<std::unique_ptr<Callback>> initializers_;
  std::vector<std::unique_ptr<TestInfo>> testCases_;

  //! ordered list of tests as offsets into testCases_
  std::vector<size_t> activeTests_;

  int repeats_;
  bool verbose_;
  bool printProgress_;
  bool printSummaryDetails_;

  TestInfo* currentTestCase_;
  size_t currentCount_;
  size_t successCount_;
  int failCount_;
  std::vector<std::string> failures_;
};

template<typename... Args>
inline void Test::logf(const char* fmt, Args... args) {
  UnitTest::instance()->logf(fmt, args...);
}

} // namespace regex_dfa::util::testing
