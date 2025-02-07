use crate::LintResult;

pub fn lint_std_filesystem() -> LintResult {
    let found = crate::git()
        .args([
            "grep",
            "--line-number",
            "std::filesystem",
            "--",
            "./src/",
            ":(exclude)src/util/fs.h",
        ])
        .status()
        .expect("command error")
        .success();
    if found {
        Err(r#"
Direct use of std::filesystem may be dangerous and buggy. Please include <util/fs.h> and use the
fs:: namespace, which has unsafe filesystem functions marked as deleted.
            "#
        .trim()
        .to_string())
    } else {
        Ok(())
    }
}
