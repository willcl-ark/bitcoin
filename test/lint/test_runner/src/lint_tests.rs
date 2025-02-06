use regex::Regex;
use std::process::Command;

pub fn check_test_names() -> Result<(), String> {
    let test_suite_list = grep_boost_fixture_test_suite()?;
    let mut failed = false;

    if check_matching_test_names(&test_suite_list) > 0 {
        failed = true;
    }

    if check_unique_test_names(&test_suite_list) > 0 {
        failed = true;
    }

    if failed {
        Err("Test suite naming convention check failed".to_string())
    } else {
        Ok(())
    }
}

fn grep_boost_fixture_test_suite() -> Result<Vec<String>, String> {
    let output = Command::new("git")
        .args([
            "grep",
            "-E",
            "^BOOST_FIXTURE_TEST_SUITE\\(",
            "--",
            "src/test/**.cpp",
            "src/wallet/test/**.cpp",
        ])
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Err("Git grep command failed".to_string());
    }

    Ok(String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?
        .lines()
        .map(String::from)
        .collect())
}

fn check_matching_test_names(test_suite_list: &[String]) -> i32 {
    let filename_re = Regex::new(r"/([^/]+)\.cpp:").unwrap();
    let suite_name_re = Regex::new(r"BOOST_FIXTURE_TEST_SUITE\(([^,]+),").unwrap();

    let not_matching: Vec<_> = test_suite_list
        .iter()
        .filter(|line| {
            let filename = filename_re
                .captures(line)
                .and_then(|cap| cap.get(1))
                .map(|m| m.as_str());
            let suite_name = suite_name_re
                .captures(line)
                .and_then(|cap| cap.get(1))
                .map(|m| m.as_str());

            match (filename, suite_name) {
                (Some(f), Some(s)) => f != s,
                _ => true,
            }
        })
        .collect();

    if !not_matching.is_empty() {
        let error_msg = format!(
            r#"The test suite in file src/test/foo_tests.cpp should be named
"foo_tests". Please make sure the following test suites follow
that convention:

{}
"#,
            not_matching
                .iter()
                .map(|s| s.as_str())
                .collect::<Vec<_>>()
                .join("\n")
        );
        println!("{}", error_msg);
        1
    } else {
        0
    }
}

fn check_unique_test_names(test_suite_list: &[String]) -> i32 {
    let re = Regex::new(r"\((.*?),").unwrap();
    let mut seen = std::collections::HashSet::new();
    let mut dupes = std::collections::HashSet::new();

    for line in test_suite_list {
        if let Some(cap) = re.captures(line) {
            if let Some(name) = cap.get(1) {
                let name = name.as_str().to_string();
                if !seen.insert(name.clone()) {
                    dupes.insert(name);
                }
            }
        }
    }

    let dupes: Vec<_> = dupes.into_iter().collect();
    if !dupes.is_empty() {
        let error_msg = format!(
            r#"Test suite names must be unique. The following test suite names
appear to be used more than once:

{}
"#,
            dupes.join("\n")
        );
        println!("{}", error_msg);
        1
    } else {
        0
    }
}
