use crate::ignore_dirs::get_shared_exclude_args;
use std::io::ErrorKind;
use std::process::Command;

pub fn lint_python() -> Result<(), String> {
    let bin_name = "ruff";
    let checks = format!(
        "--select={}",
        [
            "B006", "B008", "E101", "E401", "E402", "E701", "E702", "E703", "E711", "E714", "E721",
            "E722", "E742", "E743", "F401", "F402", "F403", "F404", "F405", "F406", "F407", "F541",
            "F601", "F602", "F621", "F631", "F632", "F811", "F821", "F822", "F823", "F841", "PLE",
            "W191", "W291", "W292", "W293", "W605"
        ]
        .join(",")
    );

    let files = get_python_files()?;
    run_linter(bin_name, &checks, &files)
}

fn get_python_files() -> Result<Vec<String>, String> {
    let mut cmd = Command::new("git");
    cmd.args(["ls-files", "--", "*.py"]);
    cmd.args(get_shared_exclude_args());

    let output = cmd
        .output()
        .map_err(|e| format!("Failed to list files: {}", e))?;

    if !output.status.success() {
        return Err("Failed to get list of Python files".to_string());
    }

    Ok(String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(String::from)
        .collect())
}

fn run_linter(bin_name: &str, checks: &str, files: &[String]) -> Result<(), String> {
    let mut cmd = Command::new(bin_name);
    cmd.args(["check", checks]);
    cmd.args(files);

    match cmd.output() {
        Ok(output) if output.status.success() => Ok(()),
        Ok(output) => Err(format!(
            "`{}` found errors: {}",
            bin_name,
            String::from_utf8_lossy(&output.stderr)
        )),
        Err(e) if e.kind() == ErrorKind::NotFound => {
            println!(
                "`{}` was not found in $PATH, skipping those checks.",
                bin_name
            );
            Ok(())
        }
        Err(e) => Err(format!("Error running `{}`: {}", bin_name, e)),
    }
}
