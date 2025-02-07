use crate::ignore_dirs::get_all_exclude_args;
use crate::{git, LintResult};

pub fn lint_trailing_whitespace() -> LintResult {
    let trailing_space = git()
        .args(["grep", "-I", "--line-number", "\\s$", "--"])
        .args(get_all_exclude_args())
        .status()
        .expect("command error")
        .success();
    if trailing_space {
        Err(r#"Trailing whitespace (including Windows line endings [CR LF]) is problematic. Please remove the trailing space."#.to_string())
    } else {
        Ok(())
    }
}

pub fn lint_tabs_whitespace() -> LintResult {
    let tabs = git()
        .args(["grep", "-I", "--line-number", "--perl-regexp", "^\\t", "--"])
        .args(["*.cpp", "*.h", "*.md", "*.py", "*.sh"])
        .args(get_all_exclude_args())
        .status()
        .expect("command error")
        .success();
    if tabs {
        Err(r#"Use of tabs in this codebase is problematic. Please remove the tabs."#.to_string())
    } else {
        Ok(())
    }
}
