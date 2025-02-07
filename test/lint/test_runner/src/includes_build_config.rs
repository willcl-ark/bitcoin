use std::process::Command;

use crate::{check_output, get_pathspecs_exclude_subtrees, git, LintResult};

pub fn lint_includes_build_config() -> LintResult {
    let config_path = "./cmake/bitcoin-build-config.h.in";
    let defines_regex = format!(
        r"^\s*(?!//).*({})",
        check_output(Command::new("grep").args(["define", "--", config_path]))
            .expect("grep failed")
            .lines()
            .map(|line| {
                line.split_whitespace()
                    .nth(1)
                    .unwrap_or_else(|| panic!("Could not extract name in line: {line}"))
            })
            .collect::<Vec<_>>()
            .join("|")
    );
    let print_affected_files = |mode: bool| {
        // * mode==true: Print files which use the define, but lack the include
        // * mode==false: Print files which lack the define, but use the include
        let defines_files = check_output(
            git()
                .args([
                    "grep",
                    "--perl-regexp",
                    if mode {
                        "--files-with-matches"
                    } else {
                        "--files-without-match"
                    },
                    &defines_regex,
                    "--",
                    "*.cpp",
                    "*.h",
                ])
                .args(get_pathspecs_exclude_subtrees())
                .args([
                    // These are exceptions which don't use bitcoin-build-config.h, rather CMakeLists.txt adds
                    // these cppflags manually.
                    ":(exclude)src/crypto/sha256_arm_shani.cpp",
                    ":(exclude)src/crypto/sha256_avx2.cpp",
                    ":(exclude)src/crypto/sha256_sse41.cpp",
                    ":(exclude)src/crypto/sha256_x86_shani.cpp",
                ]),
        )
        .expect("grep failed");
        git()
            .args([
                "grep",
                if mode {
                    "--files-without-match"
                } else {
                    "--files-with-matches"
                },
                if mode {
                    "^#include <bitcoin-build-config.h> // IWYU pragma: keep$"
                } else {
                    "#include <bitcoin-build-config.h>" // Catch redundant includes with and without the IWYU pragma
                },
                "--",
            ])
            .args(defines_files.lines())
            .status()
            .expect("command error")
            .success()
    };
    let missing = print_affected_files(true);
    if missing {
        return Err(format!(
            r#"
One or more files use a symbol declared in the bitcoin-build-config.h header. However, they are not
including the header. This is problematic, because the header may or may not be indirectly
included. If the indirect include were to be intentionally or accidentally removed, the build could
still succeed, but silently be buggy. For example, a slower fallback algorithm could be picked,
even though bitcoin-build-config.h indicates that a faster feature is available and should be used.

If you are unsure which symbol is used, you can find it with this command:
git grep --perl-regexp '{}' -- file_name

Make sure to include it with the IWYU pragma. Otherwise, IWYU may falsely instruct to remove the
include again.

#include <bitcoin-build-config.h> // IWYU pragma: keep
            "#,
            defines_regex
        )
        .trim()
        .to_string());
    }
    let redundant = print_affected_files(false);
    if redundant {
        return Err(r#"
None of the files use a symbol declared in the bitcoin-build-config.h header. However, they are including
the header. Consider removing the unused include.
            "#
        .to_string());
    }
    Ok(())
}
