use crate::{check_output, commit_range, git, LintResult};

pub fn lint_commit_msg() -> LintResult {
    let mut good = true;
    let commit_hashes = check_output(git().args([
        "-c",
        "log.showSignature=false",
        "log",
        &commit_range(),
        "--format=%H",
    ]))?;
    for hash in commit_hashes.lines() {
        let commit_info = check_output(git().args([
            "-c",
            "log.showSignature=false",
            "log",
            "--format=%B",
            "-n",
            "1",
            hash,
        ]))?;
        if let Some(line) = commit_info.lines().nth(1) {
            if !line.is_empty() {
                println!(
                        "The subject line of commit hash {} is followed by a non-empty line. Subject lines should always be followed by a blank line.",
                        hash
                    );
                good = false;
            }
        }
    }
    if good {
        Ok(())
    } else {
        Err("".to_string())
    }
}
