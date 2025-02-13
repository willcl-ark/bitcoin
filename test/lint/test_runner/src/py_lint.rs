use std::{io::ErrorKind, process::Command};

use crate::{check_output, get_pathspecs_exclude_subtrees, git, LintResult};

pub fn lint_py_lint() -> LintResult {
    let bin_name = "ruff";
    let checks = format!(
        "--select={}",
        [
            "B006", // mutable-argument-default
            "B008", // function-call-in-default-argument
            "E101", // indentation contains mixed spaces and tabs
            "E401", // multiple imports on one line
            "E402", // module level import not at top of file
            "E701", // multiple statements on one line (colon)
            "E702", // multiple statements on one line (semicolon)
            "E703", // statement ends with a semicolon
            "E711", // comparison to None should be 'if cond is None:'
            "E714", // test for object identity should be "is not"
            "E721", // do not compare types, use "isinstance()"
            "E722", // do not use bare 'except'
            "E742", // do not define classes named "l", "O", or "I"
            "E743", // do not define functions named "l", "O", or "I"
            "F401", // module imported but unused
            "F402", // import module from line N shadowed by loop variable
            "F403", // 'from foo_module import *' used; unable to detect undefined names
            "F404", // future import(s) name after other statements
            "F405", // foo_function may be undefined, or defined from star imports: bar_module
            "F406", // "from module import *" only allowed at module level
            "F407", // an undefined __future__ feature name was imported
            "F541", // f-string without any placeholders
            "F601", // dictionary key name repeated with different values
            "F602", // dictionary key variable name repeated with different values
            "F621", // too many expressions in an assignment with star-unpacking
            "F631", // assertion test is a tuple, which are always True
            "F632", // use ==/!= to compare str, bytes, and int literals
            "F811", // redefinition of unused name from line N
            "F821", // undefined name 'Foo'
            "F822", // undefined name name in __all__
            "F823", // local variable name … referenced before assignment
            "F841", // local variable 'foo' is assigned to but never used
            "PLE",  // Pylint errors
            "W191", // indentation contains tabs
            "W291", // trailing whitespace
            "W292", // no newline at end of file
            "W293", // blank line contains whitespace
            "W605", // invalid escape sequence "x"
        ]
        .join(",")
    );
    let files = check_output(
        git()
            .args(["ls-files", "--", "*.py"])
            .args(get_pathspecs_exclude_subtrees()),
    )?;

    let mut cmd = Command::new(bin_name);
    cmd.args(["check", &checks])
        .args(files.lines())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped());

    match cmd.output() {
        Ok(output) if output.status.success() => Ok(()),
        Ok(output) => {
            let stdout = String::from_utf8_lossy(&output.stdout);
            let stderr = String::from_utf8_lossy(&output.stderr);
            let mut error_msg = format!("`{}` found errors!", bin_name);
            if !stdout.is_empty() {
                error_msg.push_str(&format!("\n{}", stdout));
            }
            if !stderr.is_empty() {
                error_msg.push_str(&format!("\n{}", stderr));
            }
            Err(error_msg)
        }
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
