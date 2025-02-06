use regex::Regex;
use std::process::Command;

const FOLDER_GREP: &str = "src";
const FOLDER_TEST: &str = "src/test/";
const REGEX_ARG: &str =
    r#"\b(?:GetArg|GetArgs|GetBoolArg|GetIntArg|GetPathArg|IsArgSet|get_net)\("(-[^"]+)""#;
const REGEX_DOC: &str = r#"AddArg\("(-[^"=]+)(?:=|")"#;

const SET_DOC_OPTIONAL: [&str; 4] = ["-h", "-?", "-dbcrashratio", "-forcecompactdb"];

pub fn lint_doc() -> Result<(), String> {
    let args_used = find_used_args()?;
    let args_documented = find_documented_args()?;

    let args_need_doc: Vec<_> = args_used
        .iter()
        .filter(|arg| !args_documented.contains(*arg))
        .collect();

    let args_unknown: Vec<_> = args_documented
        .iter()
        .filter(|arg| !args_used.contains(*arg))
        .collect();

    if !args_need_doc.is_empty() {
        println!("Args used        : {}", args_used.len());
        println!("Args documented  : {}", args_documented.len());
        println!("Args undocumented: {}", args_need_doc.len());
        println!("{:?}", args_need_doc);
        println!("Args unknown     : {}", args_unknown.len());
        println!("{:?}", args_unknown);

        return Err(format!(
            "Please document the following arguments: {:?}",
            args_need_doc
        ));
    }

    Ok(())
}

fn find_used_args() -> Result<Vec<String>, String> {
    let git_root = crate::get_git_root();
    let cmd_root_dir = format!("{}/{}", git_root.display(), FOLDER_GREP);

    let output = Command::new("git")
        .args([
            "grep",
            "--perl-regexp",
            REGEX_ARG,
            "--",
            &cmd_root_dir,
            &format!(":(exclude){}", FOLDER_TEST),
        ])
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Err("Git grep command failed".to_string());
    }

    let content = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    let re = Regex::new(REGEX_ARG).unwrap();
    let mut args = Vec::new();

    for cap in re.captures_iter(&content) {
        if let Some(arg) = cap.get(1) {
            args.push(arg.as_str().to_string());
        }
    }

    Ok(args)
}

fn find_documented_args() -> Result<Vec<String>, String> {
    let git_root = crate::get_git_root();
    let cmd_root_dir = format!("{}/{}", git_root.display(), FOLDER_GREP);

    let output = Command::new("git")
        .args(["grep", "--perl-regexp", REGEX_DOC, "--", &cmd_root_dir])
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Err("Git grep command failed".to_string());
    }

    let content = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    let re = Regex::new(REGEX_DOC).unwrap();
    let mut args: Vec<String> = re
        .captures_iter(&content)
        .filter_map(|cap| cap.get(1).map(|m| m.as_str().to_string()))
        .collect();

    // Add optional documented args
    args.extend(SET_DOC_OPTIONAL.iter().map(|s| s.to_string()));

    Ok(args)
}
