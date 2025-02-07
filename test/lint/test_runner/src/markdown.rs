use std::io::ErrorKind;
use std::process::{Command, Stdio};

use crate::{get_subtrees, LintResult};

pub fn lint_markdown() -> LintResult {
    let bin_name = "mlc";
    let mut md_ignore_paths = get_subtrees();
    md_ignore_paths.push("./doc/README_doxygen.md");
    let md_ignore_path_str = md_ignore_paths.join(",");

    let mut cmd = Command::new(bin_name);
    cmd.args([
        "--offline",
        "--ignore-path",
        md_ignore_path_str.as_str(),
        "--gitignore",
        "--gituntracked",
        "--root-dir",
        ".",
    ])
    .stdout(Stdio::null()); // Suppress overly-verbose output

    match cmd.output() {
        Ok(output) if output.status.success() => Ok(()),
        Ok(output) => {
            let stderr = String::from_utf8_lossy(&output.stderr);
            Err(format!(
                r#"
One or more markdown links are broken.

Note: relative links are preferred as jump-to-file works natively within Emacs, but they are not required.

Markdown link errors found:
{}
                "#,
                stderr
            )
            .trim()
            .to_string())
        }
        Err(e) if e.kind() == ErrorKind::NotFound => {
            println!("`mlc` was not found in $PATH, skipping markdown lint check.");
            Ok(())
        }
        Err(e) => Err(format!("Error running mlc: {}", e)), // Misc errors
    }
}
