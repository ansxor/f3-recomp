---
name: gtest-rapidcheck-tests
description: "Unit tests use GoogleTest; property tests use RapidCheck (rc::gtest)"
condition: "(?i)\\b(unit[- ]?tests?|property[- ]?(based )?tests?|test (harness|framework|runner)|add(ing)? (a )?tests?|write (a )?tests?|Catch2|doctest|boost\\.?test|assert\\(|_check\\.cpp|add_test\\()"
question: "Does this output add, propose or restructure C++ tests without GoogleTest (TEST/TEST_F) for unit tests and RapidCheck (RC_GTEST_PROP) for property tests, or register them other than through gtest_discover_tests in runtime/tests/?"
scope: ["text", "thinking", "tool"]
---

All C++ unit tests in this repo use **GoogleTest**, and all property tests use **RapidCheck** through its GoogleTest integration (`RC_GTEST_PROP`, `rapidcheck/gtest.h`).

- New tests go in `runtime/tests/<area>.cpp`, are built as `f3rt-test-<area>` and are registered with `gtest_discover_tests(... TEST_PREFIX "runtime-<area>.")` in `CMakeLists.txt`. Use `f3rt-test-support` for shared fixtures.
- NEVER add hand-rolled `main()`/`assert` check executables, plain `add_test` checkers, Catch2, doctest or Boost.Test.
- Properties must exercise production code (invariants, round-trips, boundaries). They must not re-implement or echo the code under test.
- Run tests with `ctest --test-dir <build> -R '^runtime-'`.
