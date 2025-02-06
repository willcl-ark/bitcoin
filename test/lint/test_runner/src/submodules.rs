use std::process::Command;

pub fn lint_submodules() -> Result<(), String> {
    let output = Command::new("git")
        .args(["submodule", "status", "--recursive"])
        .output()
        .map_err(|e| format!("Failed to execute git command: {}", e))?;

    if !output.status.success() {
        return Err("Git submodule status command failed".to_string());
    }

    let submodules = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?
        .trim()
        .to_string();

    if !submodules.is_empty() {
        Err(format!(
            "These submodules were found, delete them:\n{}",
            submodules
        ))
    } else {
        Ok(())
    }
}
