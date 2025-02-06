use std::collections::HashSet;
use std::process::Command;

use crate::ignore_dirs::get_exclude_args;

const EXPECTED_BOOST_INCLUDES: &[&str] = &[
    "boost/multi_index/detail/hash_index_iterator.hpp",
    "boost/multi_index/hashed_index.hpp",
    "boost/multi_index/identity.hpp",
    "boost/multi_index/indexed_by.hpp",
    "boost/multi_index/ordered_index.hpp",
    "boost/multi_index/sequenced_index.hpp",
    "boost/multi_index/tag.hpp",
    "boost/multi_index_container.hpp",
    "boost/operators.hpp",
    "boost/signals2/connection.hpp",
    "boost/signals2/optional_last_value.hpp",
    "boost/signals2/signal.hpp",
    "boost/test/included/unit_test.hpp",
    "boost/test/unit_test.hpp",
    "boost/tuple/tuple.hpp",
];

const EXCLUDED_DIRS: &[&str] = &["contrib/devtools/bitcoin-tidy/"];

fn check_duplicate_includes() -> Result<(), String> {
    let files = crate::check_output(
        Command::new("git")
            .args(["ls-files", "--", "*.cpp", "*.h"])
            .args(get_exclude_args(EXCLUDED_DIRS)),
    )?;

    let mut failed_tests = 0;
    for filename in files.lines() {
        let content = std::fs::read_to_string(filename)
            .map_err(|e| format!("Failed to read {}: {}", filename, e))?;

        #[derive(Default)]
        struct IncludeContext {
            in_extern_c: bool,
        }

        let mut includes: Vec<(String, IncludeContext, usize)> = Vec::new();
        let mut scope_level: usize = 0;
        let mut in_extern_c = false;

        for (line_num, line) in content.lines().enumerate() {
            let line = line.trim();

            if line.starts_with("extern \"C\"") {
                in_extern_c = true;
                continue;
            }

            if line == "}" && in_extern_c {
                in_extern_c = false;
                continue;
            }

            if line.starts_with("#if") || line.starts_with("#ifdef") || line.starts_with("#ifndef")
            {
                scope_level += 1;
            } else if line.starts_with("#endif") {
                scope_level = scope_level.saturating_sub(1);
            } else if line.starts_with("#include") {
                if let Some(path) = line
                    .split('<')
                    .nth(1)
                    .and_then(|s| s.split('>').next())
                    .or_else(|| line.split('"').nth(1).and_then(|s| s.split('"').next()))
                {
                    includes.push((
                        path.to_string(),
                        IncludeContext { in_extern_c },
                        line_num + 1,
                    ));
                }
            }
        }

        let mut seen_includes: std::collections::HashMap<(String, bool), Vec<usize>> =
            std::collections::HashMap::new();

        for (path, context, line_num) in includes {
            seen_includes
                .entry((path, context.in_extern_c))
                .or_default()
                .push(line_num);
        }

        let duplicates: Vec<_> = seen_includes
            .iter()
            .filter(|(_, lines)| lines.len() > 1)
            .collect();

        if !duplicates.is_empty() {
            println!("Duplicate include(s) in {}:", filename);
            for ((path, _), lines) in duplicates {
                println!("{} at lines: {:?}", path, lines);
            }
            println!();
            failed_tests += 1;
        }
    }

    if failed_tests > 0 {
        return Err(format!(
            "Found {} files with duplicate includes",
            failed_tests
        ));
    }

    Ok(())
}

fn check_included_cpps() -> Result<(), String> {
    let output = Command::new("git")
        .args([
            "grep",
            "-E",
            r#"^#include [<"][^>"]+\.cpp[>"]"#,
            "--",
            "*.cpp",
            "*.h",
        ])
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Ok(());
    }

    let included_cpps = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    if !included_cpps.is_empty() {
        println!("The following files #include .cpp files:");
        println!("{}", included_cpps);
        return Err("Found .cpp files being included".to_string());
    }

    Ok(())
}

fn check_boost_dependencies() -> Result<(), String> {
    let output = Command::new("git")
        .args(["grep", "-E", r"^#include <boost/", "--", "*.cpp", "*.h"])
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Ok(());
    }

    let included_boosts = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    let mut filtered_included_boost_set = HashSet::new();
    for line in included_boosts.lines() {
        if let Some(boost_path) = line.split('<').nth(1).and_then(|s| s.split('>').next()) {
            filtered_included_boost_set.insert(boost_path.to_string());
        }
    }

    let mut exclusion_set = HashSet::new();
    for expected_boost in EXPECTED_BOOST_INCLUDES {
        for boost in &filtered_included_boost_set {
            if boost.contains(expected_boost) {
                exclusion_set.insert(boost.clone());
            }
        }
    }

    let extra_boosts: Vec<_> = filtered_included_boost_set
        .difference(&exclusion_set)
        .collect();

    if !extra_boosts.is_empty() {
        for boost in extra_boosts {
            println!(
                "A new Boost dependency in the form of \"{}\" appears to have been introduced:",
                boost
            );
            let output = Command::new("git")
                .args(["grep", boost, "--", "*.cpp", "*.h"])
                .output()
                .map_err(|e| format!("Failed to execute git grep: {}", e))?;
            println!("{}", String::from_utf8_lossy(&output.stdout));
        }
        return Err("Found unexpected Boost dependencies".to_string());
    }

    for expected_boost in EXPECTED_BOOST_INCLUDES {
        let output = Command::new("git")
            .args([
                "grep",
                "-q",
                &format!(r"^#include <{}>", expected_boost),
                "--",
                "*.cpp",
                "*.h",
            ])
            .output()
            .map_err(|e| format!("Failed to execute git grep: {}", e))?;

        if !output.status.success() {
            println!(
                "Good job! The Boost dependency \"{}\" is no longer used. Please remove it from EXPECTED_BOOST_INCLUDES in {}",
                expected_boost,
                file!()
            );
            return Err("Found unused expected Boost dependency".to_string());
        }
    }

    Ok(())
}

fn check_quote_syntax_includes() -> Result<(), String> {
    let output = Command::new("git")
        .args(["grep", r#"^#include ""#, "--", "*.cpp", "*.h"])
        .args(get_exclude_args(EXCLUDED_DIRS))
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Ok(());
    }

    let quote_syntax_includes = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    if !quote_syntax_includes.is_empty() {
        println!("Please use bracket syntax includes (\"#include <foo.h>\") instead of quote syntax includes:");
        println!("{}", quote_syntax_includes);
        return Err("Found quote syntax includes".to_string());
    }

    Ok(())
}

pub fn lint_includes() -> Result<(), String> {
    let mut failed = false;

    if let Err(e) = check_duplicate_includes() {
        println!("{}", e);
        failed = true;
    }

    if let Err(e) = check_included_cpps() {
        println!("{}", e);
        failed = true;
    }

    if let Err(e) = check_boost_dependencies() {
        println!("{}", e);
        failed = true;
    }

    if let Err(e) = check_quote_syntax_includes() {
        println!("{}", e);
        failed = true;
    }

    if failed {
        Err("Includes check failed".to_string())
    } else {
        Ok(())
    }
}
