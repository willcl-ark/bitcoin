use crate::{get_pathspecs_exclude_subtrees, git, LintResult};

/// Return the pathspecs for whitespace related excludes
pub fn get_pathspecs_exclude_whitespace() -> Vec<String> {
    let mut list = get_pathspecs_exclude_subtrees();
    list.extend(
        [
            // Permanent excludes
            "*.patch",
            "src/qt/locale",
            "contrib/windeploy/win-codesign.cert",
            "doc/README_windows.txt",
            // Temporary excludes, or existing violations
            "doc/release-notes/release-notes-0.*",
            "contrib/init/bitcoind.openrc",
            "contrib/macdeploy/macdeployqtplus",
            "src/crypto/sha256_sse4.cpp",
            "src/qt/res/src/*.svg",
            "test/functional/test_framework/crypto/ellswift_decode_test_vectors.csv",
            "test/functional/test_framework/crypto/xswiftec_inv_test_vectors.csv",
            "contrib/qos/tc.sh",
            "contrib/verify-commits/gpg.sh",
            "src/univalue/include/univalue_escapes.h",
            "src/univalue/test/object.cpp",
            "test/lint/git-subtree-check.sh",
        ]
        .iter()
        .map(|s| format!(":(exclude){}", s)),
    );
    list
}

pub fn lint_trailing_whitespace() -> LintResult {
    let trailing_space = git()
        .args(["grep", "-I", "--line-number", "\\s$", "--"])
        .args(get_pathspecs_exclude_whitespace())
        .status()
        .expect("command error")
        .success();
    if trailing_space {
        Err(r#"
Trailing whitespace (including Windows line endings [CR LF]) is problematic, because git may warn
about it, or editors may remove it by default, forcing developers in the future to either undo the
changes manually or spend time on review.

Thus, it is best to remove the trailing space now.

Please add any false positives, such as subtrees, Windows-related files, patch files, or externally
sourced files to the exclude list.
            "#
        .trim()
        .to_string())
    } else {
        Ok(())
    }
}

pub fn lint_tabs_whitespace() -> LintResult {
    let tabs = git()
        .args(["grep", "-I", "--line-number", "--perl-regexp", "^\\t", "--"])
        .args(["*.cpp", "*.h", "*.md", "*.py", "*.sh"])
        .args(get_pathspecs_exclude_whitespace())
        .status()
        .expect("command error")
        .success();
    if tabs {
        Err(r#"
Use of tabs in this codebase is problematic, because existing code uses spaces and tabs will cause
display issues and conflict with editor settings.

Please remove the tabs.

Please add any false positives, such as subtrees, or externally sourced files to the exclude list.
            "#
        .trim()
        .to_string())
    } else {
        Ok(())
    }
}
