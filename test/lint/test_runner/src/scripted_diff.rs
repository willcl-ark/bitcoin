use std::process::Command;

use crate::LintResult;

pub fn lint_scripted_diff() -> LintResult {
    if Command::new("test/lint/commit-script-check.sh")
        .arg(crate::commit_range())
        .status()
        .expect("command error")
        .success()
    {
        Ok(())
    } else {
        Err("".to_string())
    }
}
