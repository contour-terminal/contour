add_library(regex_dfa STATIC
    Alphabet.cpp
    Compiler.cpp
    DFA.cpp
    DFABuilder.cpp
    DFAMinimizer.cpp
    DotWriter.cpp
    MultiDFA.cpp
    NFA.cpp
    NFABuilder.cpp
    RegExpr.cpp
    RegExprParser.cpp
    RuleParser.cpp
    State.cpp
    Symbols.cpp
    Report.cpp
    SourceLocation.cpp
)

target_include_directories(regex_dfa PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(regex_dfa PUBLIC fmt::fmt-header-only)

# ----------------------------------------------------------------------------
option(REGEX_DFA_TESTING "Enables building of unittests for regex_dfa library [default: ON]" ON)
if(REGEX_DFA_TESTING)
    enable_testing()
    add_executable(regex_dfa_test
        regex_dfa_test.cpp
        DFABuilder_test.cpp
        DotWriter_test.cpp
        Lexer_test.cpp
        NFA_test.cpp
        RegExprParser_test.cpp
        RuleParser_test.cpp
        State_test.cpp
        Symbols_test.cpp
        util/iterator_test.cpp
    )

    target_link_libraries(regex_dfa_test PUBLIC regex_dfa)
    target_link_libraries(regex_dfa_test PUBLIC Catch2::Catch2)
    target_link_libraries(regex_dfa_test PUBLIC fmt::fmt-header-only)
endif(REGEX_DFA_TESTING)
