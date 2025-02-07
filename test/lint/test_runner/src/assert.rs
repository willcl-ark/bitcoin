use crate::{git, LintResult};

pub fn lint_rpc_assert() -> LintResult {
    let found = git()
        .args([
            "grep",
            "--line-number",
            "--extended-regexp",
            r"\<(A|a)ss(ume|ert)\(",
            "--",
            "src/rpc/",
            "src/wallet/rpc*",
            ":(exclude)src/rpc/server.cpp",
            // src/rpc/server.cpp is excluded from this check since it's mostly meta-code.
        ])
        .status()
        .expect("command error")
        .success();
    if found {
        Err(r#"
CHECK_NONFATAL(condition) or NONFATAL_UNREACHABLE should be used instead of assert for RPC code.

Aborting the whole process is undesirable for RPC code. So nonfatal
checks should be used over assert. See: src/util/check.h
            "#
        .trim()
        .to_string())
    } else {
        Ok(())
    }
}

pub fn lint_boost_assert() -> LintResult {
    let found = git()
        .args([
            "grep",
            "--line-number",
            "--extended-regexp",
            r"BOOST_ASSERT\(",
            "--",
            "*.cpp",
            "*.h",
        ])
        .status()
        .expect("command error")
        .success();
    if found {
        Err(r#"
BOOST_ASSERT must be replaced with Assert, BOOST_REQUIRE, or BOOST_CHECK to avoid an unnecessary
include of the boost/assert.hpp dependency.
            "#
        .trim()
        .to_string())
    } else {
        Ok(())
    }
}
