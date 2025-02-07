// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

use std::env;
use std::path::PathBuf;
use std::process::{Command, ExitCode};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Instant;

mod assert;
mod check_doc;
mod circular_dependencies;
mod commit_message;
mod dead_code;
mod files;
mod filesystem;
mod ignore_dirs;
mod include_guards;
mod includes;
mod lint_tests;
mod locale_dependence;
mod markdown;
mod py_lint;
mod python;
mod python_utf8;
mod qt_translation;
mod release_notes;
mod scripted_diff;
mod shell;
mod spelling;
mod submodules;
mod subtree;
mod whitespace;

use assert::{lint_boost_assert, lint_rpc_assert};
use check_doc::lint_doc;
use commit_message::lint_commit_msg;
use dead_code::lint_python_dead_code;
use files::lint_files;
use filesystem::lint_std_filesystem;
use include_guards::check_include_guards;
use includes::lint_includes;
use lint_tests::check_test_names;
use locale_dependence::lint_locale_dependence;
use markdown::lint_markdown;
use py_lint::lint_py_lint;
use python::lint_python;
use python_utf8::lint_python_utf8;
use qt_translation::lint_qt_translation;
use release_notes::lint_doc_release_note_snippets;
use scripted_diff::lint_scripted_diff;
use shell::lint_shell;
use shell::lint_shell_locale;
use spelling::lint_spelling;
use submodules::lint_submodules;
use subtree::lint_subtree;
use whitespace::{lint_tabs_whitespace, lint_trailing_whitespace};

/// A possible error returned by any of the linters.
///
/// The error string should explain the failure type and list all violations.
type LintError = String;
type LintResult = Result<(), LintError>;
type LintFn = fn() -> LintResult;

struct Linter {
    pub description: &'static str,
    pub name: &'static str,
    pub lint_fn: LintFn,
}

#[derive(Debug)]
struct LintFailure {
    name: String,
    description: String,
    error: String,
}

#[derive(Debug)]
struct LintStatus {
    running: Vec<String>,
    completed: Vec<String>,
    failures: Vec<LintFailure>,
}

impl LintStatus {
    fn new() -> Self {
        Self {
            running: Vec::new(),
            completed: Vec::new(),
            failures: Vec::new(),
        }
    }
}

fn get_linter_list() -> Vec<&'static Linter> {
    vec![
        &Linter {
            description: "Check locale dependence",
            name: "locale_dependence",
            lint_fn: lint_locale_dependence
        },
        &Linter {
            description: "Check that all command line arguments are documented.",
            name: "doc",
            lint_fn: lint_doc
        },
        &Linter {
            description: "Check that no symbol from bitcoin-build-config.h is used without the header being included",
            name: "includes_build_config",
            lint_fn: lint_includes_build_config
        },
        &Linter {
            description: "Check that markdown links resolve",
            name: "markdown",
            lint_fn: lint_markdown
        },
        &Linter {
            description: "Lint Python code",
            name: "py_lint",
            lint_fn: lint_py_lint,
        },
        &Linter {
            description: "Check that std::filesystem is not used directly",
            name: "std_filesystem",
            lint_fn: lint_std_filesystem
        },
        &Linter {
            description: "Check that fatal assertions are not used in RPC code",
            name: "rpc_assert",
            lint_fn: lint_rpc_assert
        },
        &Linter {
            description: "Check that boost assertions are not used",
            name: "boost_assert",
            lint_fn: lint_boost_assert
        },
        &Linter {
            description: "Check that release note snippets are in the right folder",
            name: "doc_release_note_snippets",
            lint_fn: lint_doc_release_note_snippets
        },
        &Linter {
            description: "Check that subtrees are pure subtrees",
            name: "subtree",
            lint_fn: lint_subtree
        },
        &Linter {
            description: "Check scripted-diffs",
            name: "scripted_diff",
            lint_fn: lint_scripted_diff
        },
        &Linter {
            description: "Check that commit messages have a new line before the body or no body at all.",
            name: "commit_msg",
            lint_fn: lint_commit_msg
        },
        &Linter {
            description: "Check that tabs are not used as whitespace",
            name: "tabs_whitespace",
            lint_fn: lint_tabs_whitespace
        },
        &Linter {
            description: "Check for trailing whitespace",
            name: "trailing_whitespace",
            lint_fn: lint_trailing_whitespace
        },
        &Linter {
            description: "Check for circular dependencies",
            name: "circular_dependencies",
            lint_fn: lint_circular_dependencies
        },
        &Linter {
            description: "Check submodules",
            name: "submodules",
            lint_fn: lint_submodules
        },
        &Linter {
            description: "Check test names",
            name: "tests",
            lint_fn: check_test_names
        },
        &Linter {
            description: "Check include guards",
            name: "include_guards",
            lint_fn: check_include_guards
        },
        &Linter {
            description: "Check includes",
            name: "includes",
            lint_fn: lint_includes
        },
        &Linter {
            description: "Check dead code in python files",
            name: "python_dead_code",
            lint_fn: lint_python_dead_code
        },
        &Linter {
            description: "Check utf8 in python files",
            name: "python_utf8",
            lint_fn: lint_python_utf8
        },
        &Linter {
            description: "Check python files",
            name: "python",
            lint_fn: lint_python
        },
        &Linter {
            description: "Check qt translations",
            name: "qt_translation",
            lint_fn: lint_qt_translation
        },
        &Linter {
            description: "Check shell locale",
            name: "shell_locale",
            lint_fn: lint_shell_locale
        },
        &Linter {
            description: "Check shell script with shellcheck",
            name: "shell",
            lint_fn: lint_shell
        },
        &Linter {
            description: "Check source code spelling",
            name: "spelling",
            lint_fn: lint_spelling
        },
        &Linter {
            description: "Check filenames and permissions",
            name: "files",
            lint_fn: lint_files
        },
    ]
}

fn print_help_and_exit() {
    print!(
        r#"
Usage: test_runner [--lint=LINTER_TO_RUN]
Runs all linters in the lint test suite, printing any errors
they detect.

If you wish to only run some particular lint tests, pass
'--lint=' with the name of the lint test you wish to run.
You can set as many '--lint=' values as you wish, e.g.:
test_runner --lint=doc --lint=subtree

The individual linters available to run are:
"#
    );
    for linter in get_linter_list() {
        println!("{}: \"{}\"", linter.name, linter.description)
    }

    std::process::exit(1);
}

fn parse_lint_args(args: &[String]) -> Vec<&'static Linter> {
    let linter_list = get_linter_list();
    let mut lint_values = Vec::new();

    for arg in args {
        #[allow(clippy::if_same_then_else)]
        if arg.starts_with("--lint=") {
            let lint_arg_value = arg
                .trim_start_matches("--lint=")
                .trim_matches('"')
                .trim_matches('\'');

            let try_find_linter = linter_list
                .iter()
                .find(|linter| linter.name == lint_arg_value);
            match try_find_linter {
                Some(linter) => {
                    lint_values.push(*linter);
                }
                None => {
                    println!("No linter {lint_arg_value} found!");
                    print_help_and_exit();
                }
            }
        } else if arg.eq("--help") || arg.eq("-h") {
            print_help_and_exit();
        } else {
            print_help_and_exit();
        }
    }

    lint_values
}

/// Return the git command
///
/// Lint functions should use this command, so that only files tracked by git are considered and
/// temporary and untracked files are ignored. For example, instead of 'grep', 'git grep' should be
/// used.
fn git() -> Command {
    let mut git = Command::new("git");
    git.arg("--no-pager");
    git
}

/// Return stdout on success and a LintError on failure, when invalid UTF8 was detected or the
/// command did not succeed.
fn check_output(cmd: &mut std::process::Command) -> Result<String, LintError> {
    let out = cmd.output().expect("command error");
    if !out.status.success() {
        return Err(String::from_utf8_lossy(&out.stderr).to_string());
    }
    Ok(String::from_utf8(out.stdout)
        .map_err(|e| {
            format!("All path names, source code, messages, and output must be valid UTF8!\n{e}")
        })?
        .trim()
        .to_string())
}

/// Return the git root as utf8, or panic
fn get_git_root() -> PathBuf {
    PathBuf::from(check_output(git().args(["rev-parse", "--show-toplevel"])).unwrap())
}

/// Return the commit range, or panic
fn commit_range() -> String {
    // Use the env var, if set. E.g. COMMIT_RANGE='HEAD~n..HEAD' for the last 'n' commits.
    env::var("COMMIT_RANGE").unwrap_or_else(|_| {
        // Otherwise, assume that a merge commit exists. This merge commit is assumed
        // to be the base, after which linting will be done. If the merge commit is
        // HEAD, the range will be empty.
        format!(
            "{}..HEAD",
            check_output(git().args(["rev-list", "--max-count=1", "--merges", "HEAD"]))
                .expect("check_output failed")
        )
    })
}

/// Return all subtree paths
fn get_subtrees() -> Vec<&'static str> {
    vec![
        "src/crc32c",
        "src/crypto/ctaes",
        "src/leveldb",
        "src/minisketch",
        "src/secp256k1",
    ]
}

/// Return the pathspecs to exclude all subtrees
fn get_pathspecs_exclude_subtrees() -> Vec<String> {
    get_subtrees()
        .iter()
        .map(|s| format!(":(exclude){}", s))
        .collect()
}

fn lint_includes_build_config() -> LintResult {
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

fn lint_circular_dependencies() -> LintResult {
    match circular_dependencies::check_circular_dependencies() {
        Ok(()) => Ok(()),
        Err(e) => Err(e),
    }
}

fn main() -> ExitCode {
    let linters_to_run: Vec<&Linter> = if env::args().count() > 1 {
        let args: Vec<String> = env::args().skip(1).collect();
        parse_lint_args(&args)
    } else {
        get_linter_list()
    };

    let git_root = get_git_root();
    let commit_range = commit_range();
    let commit_log = check_output(git().args(["log", "--no-merges", "--oneline", &commit_range]))
        .expect("check_output failed");
    println!("Checking commit range ({commit_range}):\n{commit_log}\n");

    let status = Arc::new(Mutex::new(LintStatus::new()));
    let git_root = Arc::new(git_root);
    let total_linters = linters_to_run.len();

    let start_time = Instant::now();
    println!("Starting {} linters in parallel...\n", total_linters);

    // Spawn threads for each linter
    let handles: Vec<_> = linters_to_run
        .iter()
        .map(|linter| {
            let git_root = Arc::clone(&git_root);
            let status = Arc::clone(&status);
            let name = linter.name.to_string();
            let description = linter.description.to_string();
            let lint_fn = linter.lint_fn;
            let total = total_linters;

            thread::spawn(move || {
                // Mark linter as running
                let linter_name = name.clone();
                {
                    let mut status = status.lock().unwrap();
                    status.running.push(linter_name.clone());
                    let running = status.running.len();
                    let completed = status.completed.len();
                    println!(
                        "Starting: {} ({}/{} running, {} completed)",
                        linter_name, running, total, completed
                    );
                }

                env::set_current_dir(&*git_root).unwrap();
                let result = (lint_fn)();

                // Update status after completion
                let mut status = status.lock().unwrap();
                status.running.retain(|n| n != &linter_name);
                status.completed.push(linter_name.clone());

                if let Err(error) = result {
                    status.failures.push(LintFailure {
                        name,
                        description,
                        error,
                    });
                }

                let running = status.running.len();
                let completed = status.completed.len();
                println!(
                    "Completed: {} ({}/{} running, {} completed)",
                    linter_name, running, total, completed
                );
            })
        })
        .collect();

    // Wait for all threads to complete
    for handle in handles {
        handle.join().unwrap();
    }

    let elapsed = start_time.elapsed();
    let status = status.lock().unwrap();

    // Print all failures
    if !status.failures.is_empty() {
        println!("\nLint failures:");
        for failure in &status.failures {
            println!(
                "^^^\n{}\n^---- ⚠️ Failure generated from lint check '{}' ({})!\n",
                failure.error, failure.name, failure.description,
            );
        }
        println!(
            "\nCompleted in {:.2?} with {} failures",
            elapsed,
            status.failures.len()
        );
        ExitCode::FAILURE
    } else {
        println!(
            "\nAll {} linters passed successfully in {:.2?}!",
            total_linters, elapsed
        );
        ExitCode::SUCCESS
    }
}
