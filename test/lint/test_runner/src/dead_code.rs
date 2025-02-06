use std::process::Command;

pub fn lint_python_dead_code() -> Result<(), String> {
    check_vulture_install()?;

    let files = get_python_files()?;
    let vulture_args = vec!["--min-confidence=100"];

    let output = Command::new("vulture")
        .args(vulture_args)
        .args(files.lines())
        .output()
        .map_err(|e| format!("Failed to execute vulture: {}", e))?;

    if !output.status.success() {
        let error_msg = String::from_utf8_lossy(&output.stdout);
        println!("{}", error_msg);
        Err("Python dead code detection found some issues".to_string())
    } else {
        Ok(())
    }
}

fn check_vulture_install() -> Result<(), String> {
    match Command::new("vulture").arg("--version").output() {
        Ok(_) => Ok(()),
        Err(_) => {
            println!("Skipping Python dead code linting since vulture is not installed. Install by running \"pip3 install vulture\"");
            Ok(())
        }
    }
}

fn get_python_files() -> Result<String, String> {
    let output = Command::new("git")
        .args(["ls-files", "--", "*.py"])
        .output()
        .map_err(|e| format!("Failed to execute git command: {}", e))?;

    String::from_utf8(output.stdout).map_err(|e| format!("Invalid UTF-8 in git output: {}", e))
}
