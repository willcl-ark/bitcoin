use crate::ignore_dirs::get_exclude_args;
use std::process::Command;

const DISABLED_WARNINGS: &[&str] = &["SC2162"]; // read without -r will mangle backslashes
const SHELL_EXCLUDED_DIRS: &[&str] = &["src/secp256k1/", "src/minisketch/"];

fn get_shell_files() -> Result<Vec<String>, String> {
    let output = Command::new("git")
        .args(["ls-files", "--", "*.sh"])
        .args(get_exclude_args(SHELL_EXCLUDED_DIRS))
        .output()
        .map_err(|e| format!("Failed to execute git command: {}", e))?;

    if !output.status.success() {
        return Err("Git ls-files command failed".to_string());
    }

    Ok(String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?
        .lines()
        .map(String::from)
        .collect())
}

fn check_shellcheck_install() -> Result<(), String> {
    match Command::new("shellcheck").arg("--version").output() {
        Ok(_) => Ok(()),
        Err(_) => {
            println!("Skipping shell linting since shellcheck is not installed.");
            Ok(())
        }
    }
}

pub fn lint_shell() -> Result<(), String> {
    check_shellcheck_install()?;

    let exclude = format!("--exclude={}", DISABLED_WARNINGS.join(","));
    let files = get_shell_files()?;

    if files.is_empty() {
        return Ok(());
    }

    let mut cmd = Command::new("shellcheck");
    cmd.args([
        "--external-sources",
        "--check-sourced",
        "--source-path=SCRIPTDIR",
    ]);
    cmd.arg(&exclude);
    cmd.args(&files);

    let output = cmd
        .output()
        .map_err(|e| format!("Failed to execute shellcheck: {}", e))?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        let mut error_msg = String::new();

        if !stdout.is_empty() {
            error_msg.push_str(&stdout);
        }
        if !stderr.is_empty() {
            if !error_msg.is_empty() {
                error_msg.push('\n');
            }
            error_msg.push_str(&stderr);
        }

        Err(error_msg)
    } else {
        Ok(())
    }
}

pub fn lint_shell_locale() -> Result<(), String> {
    let files = get_shell_files()?;
    let mut exit_code = 0;

    for file in files {
        if !file.contains("src/secp256k1/") && !file.contains("src/minisketch/") {
            let content = std::fs::read_to_string(&file)
                .map_err(|e| format!("Failed to read {}: {}", file, e))?;

            let non_comment_line = content.lines().find(|line| {
                let trimmed = line.trim();
                !trimmed.is_empty() && !trimmed.starts_with('#')
            });

            if let Some(first_line) = non_comment_line {
                if !matches!(first_line.trim(), "export LC_ALL=C" | "export LC_ALL=C.UTF-8")
                    && !content.contains("# This script is intentionally locale dependent by not setting \"export LC_ALL=C\"") {
                    println!(
                        "Missing \"export LC_ALL=C\" (to avoid locale dependence) as first non-comment non-empty line in {}",
                        file
                    );
                    exit_code = 1;
                }
            }
        }
    }

    if exit_code != 0 {
        Err("Please fix the above listed shell script locale issues.".to_string())
    } else {
        Ok(())
    }
}
