use std::process::Command;

pub fn lint_qt_translation() -> Result<(), String> {
    let output = Command::new("git")
        .args(["grep", "-e", r#"tr("\s"#, "--", "src/qt"])
        .output()
        .map_err(|e| format!("Failed to execute git command: {}", e))?;

    if !output.status.success() && output.status.code() != Some(1) {
        return Err("Git grep command failed".to_string());
    }

    let found_strings = String::from_utf8_lossy(&output.stdout).trim().to_string();

    if !found_strings.is_empty() {
        println!("Avoid leading whitespaces in:");
        println!("{}", found_strings);
        return Err("Found translations with leading whitespace".to_string());
    }

    Ok(())
}
