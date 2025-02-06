use crate::LintResult;
use std::process::Command;

use crate::ignore_dirs::SHARED_EXCLUDED_SUBTREES;

const EXCLUDED_DIRS: &[&str] = &[
    "contrib/seeds/*.txt",
    "depends/",
    "doc/release-notes/",
    "src/qt/locale/",
    "src/qt/*.qrc",
    "contrib/guix/patches",
];

fn check_codespell_install() -> Result<(), String> {
    match Command::new("codespell").arg("--version").output() {
        Ok(_) => Ok(()),
        Err(_) => {
            println!("Skipping spell check linting since codespell is not installed.");
            Ok(())
        }
    }
}

fn get_excluded_paths() -> Vec<String> {
    EXCLUDED_DIRS
        .iter()
        .chain(SHARED_EXCLUDED_SUBTREES.iter())
        .map(|&s| s.to_string())
        .collect()
}

fn get_files() -> Result<Vec<String>, String> {
    let mut cmd = Command::new("git");
    cmd.args(["ls-files"]);

    for path in get_excluded_paths() {
        cmd.arg(format!(":(exclude){}", path));
    }

    let output = cmd
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

pub fn lint_spelling() -> LintResult {
    check_codespell_install()?;

    let files = get_files()?;
    if files.is_empty() {
        return Ok(());
    }

    let mut cmd = Command::new("codespell");
    cmd.args([
        "--check-filenames",
        "--disable-colors",
        "--quiet-level=7",
        "--ignore-words=test/lint/spelling.ignore-words.txt",
    ]);
    cmd.args(&files);

    let output = cmd
        .output()
        .map_err(|e| format!("Failed to execute codespell: {}", e))?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        println!("{}", stdout);
        println!("{}", stderr);
        println!("^ Warning: codespell identified likely spelling errors. Any false positives? Add them to the list of ignored words in test/lint/spelling.ignore-words.txt");
    }

    Ok(())
}
