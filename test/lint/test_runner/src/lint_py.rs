// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

use std::io::{ErrorKind, Write};
use std::process::{Command, Stdio};

use crate::util::{
    check_output, commit_range, get_pathspecs_default_excludes, git, LintError, LintResult,
};

#[derive(Debug, PartialEq)]
struct ChangedFile {
    base_path: Option<String>,
    current_path: String,
}

fn next_path<'a>(
    fields: &mut std::str::Split<'a, char>,
    status: &str,
) -> Result<&'a str, LintError> {
    let path = fields
        .next()
        .ok_or_else(|| format!("Missing path for Git status {status}"))?;
    if path.is_empty() {
        return Err(format!("Empty path for Git status {status}"));
    }
    Ok(path)
}

fn parse_changed_files(output: &[u8]) -> Result<Vec<ChangedFile>, LintError> {
    let output = String::from_utf8(output.to_vec())
        .map_err(|e| format!("Git returned a non-UTF8 path name:\n{e}"))?;
    let mut fields = output.split('\0');
    let mut changed_files = Vec::new();

    while let Some(status) = fields.next() {
        if status.is_empty() {
            break;
        }

        let first_status = status.as_bytes()[0];
        match first_status {
            b'A' | b'M' => {
                let current_path = next_path(&mut fields, status)?;
                changed_files.push(ChangedFile {
                    base_path: (first_status == b'M').then(|| current_path.to_string()),
                    current_path: current_path.to_string(),
                });
            }
            b'C' | b'R' => {
                let base_path = next_path(&mut fields, status)?;
                let current_path = next_path(&mut fields, status)?;
                changed_files.push(ChangedFile {
                    base_path: Some(base_path.to_string()),
                    current_path: current_path.to_string(),
                });
            }
            _ => {
                // Deleted files and changes unrelated to the file contents are not candidates.
                next_path(&mut fields, status)?;
            }
        }
    }

    Ok(changed_files)
}

fn changed_python_files(base_revision: &str) -> Result<Vec<ChangedFile>, LintError> {
    let mut command = git();
    command
        .args([
            "diff",
            "--name-status",
            "-z",
            "-M",
            base_revision,
            "--",
            "*.py",
        ])
        .args(get_pathspecs_default_excludes());
    let output = command
        .output()
        .map_err(|e| format!("Error running `git diff`: {e}"))?;
    if !output.status.success() {
        return Err(format!(
            "`git diff` failed:\n{}",
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    parse_changed_files(&output.stdout)
}

fn base_revision() -> Result<String, LintError> {
    let range = commit_range();
    let (base, _) = range
        .split_once("..")
        .ok_or_else(|| format!("Invalid commit range: {range}"))?;
    if base.is_empty() {
        return Err(format!("Commit range has no base revision: {range}"));
    }
    Ok(base.to_string())
}

fn ruff_format_check_base(base_revision: &str, path: &str) -> Result<bool, LintError> {
    let revision_path = format!("{base_revision}:{path}");
    let output = git()
        .arg("show")
        .arg(&revision_path)
        .output()
        .map_err(|e| format!("Error running `git show`: {e}"))?;
    if !output.status.success() {
        return Err(format!(
            "Unable to read {path} from base revision {base_revision}:\n{}",
            String::from_utf8_lossy(&output.stderr)
        ));
    }

    let mut command = Command::new("ruff");
    command
        .args(["format", "--check", "--stdin-filename", path, "-"])
        .stdin(Stdio::piped());
    let mut child = command
        .spawn()
        .map_err(|e| format!("Error running `ruff format --check`: {e}"))?;
    let write_result = child
        .stdin
        .take()
        .ok_or_else(|| "Unable to open Ruff's standard input".to_string())?
        .write_all(&output.stdout);
    let status = child
        .wait()
        .map_err(|e| format!("Error waiting for `ruff format --check`: {e}"))?;
    if let Err(e) = write_result {
        if e.kind() != ErrorKind::BrokenPipe {
            return Err(format!("Error writing base file to Ruff: {e}"));
        }
    }

    match status.code() {
        Some(0) => Ok(true),
        Some(1) => Ok(false),
        Some(code) => Err(format!(
            "`ruff format --check` failed for base file {path} with exit code {code}"
        )),
        None => Err(format!(
            "`ruff format --check` was terminated while checking base file {path}"
        )),
    }
}

pub fn lint_py_format() -> LintResult {
    let bin_name = "ruff";
    match Command::new(bin_name).arg("--version").status() {
        Ok(_) => {}
        Err(e) if e.kind() == ErrorKind::NotFound => {
            println!("`{bin_name}` was not found in $PATH, skipping those checks.");
            return Ok(());
        }
        Err(e) => return Err(format!("Error running `{bin_name}`: {e}")),
    }

    let base_revision = base_revision()?;
    let changed_files = changed_python_files(&base_revision)?;
    if changed_files.is_empty() {
        return Ok(());
    }

    let mut files_to_check = Vec::new();
    for changed_file in changed_files {
        let Some(base_path) = changed_file.base_path else {
            // New files are required to be formatted
            files_to_check.push(changed_file.current_path);
            continue;
        };

        if ruff_format_check_base(&base_revision, &base_path)? {
            files_to_check.push(changed_file.current_path);
        } else {
            println!(
                "Ignoring {} because it is not formatted in the base revision.",
                changed_file.current_path
            );
        }
    }

    if files_to_check.is_empty() {
        return Ok(());
    }

    let status = Command::new(bin_name)
        .args(["format", "--check"])
        .args(&files_to_check)
        .status()
        .map_err(|e| format!("Error running `{bin_name} format --check`: {e}"))?;
    match status.code() {
        Some(0) => Ok(()),
        Some(1) => Err(format!(
            "`{bin_name} format --check` found unformatted files."
        )),
        Some(code) => Err(format!(
            "`{bin_name} format --check` failed with exit code {code}."
        )),
        None => Err(format!("`{bin_name} format --check` was terminated.")),
    }
}

pub fn lint_py_lint() -> LintResult {
    let bin_name = "ruff";
    let files = check_output(
        git()
            .args(["ls-files", "--", "*.py"])
            .args(get_pathspecs_default_excludes()),
    )?;

    let mut cmd = Command::new(bin_name);
    cmd.arg("check").args(files.lines());

    match cmd.status() {
        Ok(status) if status.success() => Ok(()),
        Ok(_) => Err(format!("`{bin_name}` found errors!")),
        Err(e) if e.kind() == ErrorKind::NotFound => {
            println!("`{bin_name}` was not found in $PATH, skipping those checks.");
            Ok(())
        }
        Err(e) => Err(format!("Error running `{bin_name}`: {e}")),
    }
}

pub fn lint_rmtree() -> LintResult {
    let found = git()
        .args([
            "grep",
            "--line-number",
            "rmtree",
            "--",
            "test/functional/",
            ":(exclude)test/functional/test_framework/test_framework.py",
        ])
        .status()
        .expect("command error")
        .success();
    if found {
        Err(r#"
Use of shutil.rmtree() is dangerous and should be avoided. If it
is really required for the test, use self.cleanup_folder(_).
            "#
        .trim()
        .to_string())
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::{parse_changed_files, ChangedFile};

    #[test]
    fn parse_changed_files_handles_file_statuses() {
        let files = parse_changed_files(
            b"A\0new file.py\0M\0modified.py\0R100\0old.py\0renamed.py\0C100\0source.py\0copied.py\0D\0deleted.py\0",
        )
        .unwrap();

        assert_eq!(
            files,
            vec![
                ChangedFile {
                    base_path: None,
                    current_path: "new file.py".to_string(),
                },
                ChangedFile {
                    base_path: Some("modified.py".to_string()),
                    current_path: "modified.py".to_string(),
                },
                ChangedFile {
                    base_path: Some("old.py".to_string()),
                    current_path: "renamed.py".to_string(),
                },
                ChangedFile {
                    base_path: Some("source.py".to_string()),
                    current_path: "copied.py".to_string(),
                },
            ]
        );
    }

    #[test]
    fn parse_changed_files_rejects_incomplete_statuses() {
        assert!(parse_changed_files(b"M\0").is_err());
        assert!(parse_changed_files(b"R100\0old.py\0").is_err());
    }
}
