use regex::Regex;
use std::process::Command;

const EXCLUDED_DIRS: &[&str] = &["src/crc32c/", "src/secp256k1/"];

fn get_exclude_args() -> Vec<String> {
    EXCLUDED_DIRS
        .iter()
        .map(|dir| format!(":(exclude){}", dir))
        .collect()
}

fn check_fileopens() -> Result<Vec<String>, String> {
    let output = Command::new("git")
        .args(["grep", "open(", "--", "*.py"])
        .args(get_exclude_args())
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Ok(Vec::new());
    }

    let content = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    // Match basic open patterns
    let binary_re = Regex::new(r#"['"]\s*[rwab][b]['"]\s*"#).unwrap();
    let kwargs_re = Regex::new(r"\*\*kwargs").unwrap();
    let encoding_re = Regex::new(r#"encoding=['"](utf-?8|ascii)['"]\s*"#).unwrap();

    let fileopens: Vec<String> = content
        .lines()
        .filter(|line| {
            // Keep the line if:
            // 1. It's not a binary mode open
            // 2. It's not using kwargs
            // 3. It doesn't specify utf8/ascii encoding
            // 4. It's opening for text mode (has 'r' or 'w' mode)
            !binary_re.is_match(line)
                && !kwargs_re.is_match(line)
                && !encoding_re.is_match(line)
                && (line.contains("'r'")
                    || line.contains("'w'")
                    || line.contains(r#""r""#)
                    || line.contains(r#""w""#)
                    || line.contains("'a'")
                    || line.contains(r#""a""#))
        })
        .map(String::from)
        .collect();

    Ok(fileopens)
}

fn check_checked_outputs() -> Result<Vec<String>, String> {
    let output = Command::new("git")
        .args(["grep", "check_output(", "--", "*.py"])
        .args(get_exclude_args())
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Ok(Vec::new());
    }

    let content = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    let encoding_re = Regex::new(r#"encoding=['"](utf-?8|ascii)['"]\s*"#).unwrap();

    let checked_outputs: Vec<String> = content
        .lines()
        .filter(|line| line.contains("text=True") && !encoding_re.is_match(line))
        .map(String::from)
        .collect();

    Ok(checked_outputs)
}

pub fn lint_python_utf8() -> Result<(), String> {
    let nonexplicit_utf8_fileopens = check_fileopens()?;
    if !nonexplicit_utf8_fileopens.is_empty() {
        println!("Python's open(...) seems to be used to open text files without explicitly specifying encoding='utf8':\n");
        for fileopen in nonexplicit_utf8_fileopens {
            println!("{}", fileopen);
        }
        println!("\n^^^");
        return Err("Python UTF-8 encoding check failed".to_string());
    }

    let nonexplicit_utf8_checked_outputs = check_checked_outputs()?;
    if !nonexplicit_utf8_checked_outputs.is_empty() {
        println!("Python's check_output(...) seems to be used to get program outputs without explicitly specifying encoding='utf8':\n");
        for checked_output in nonexplicit_utf8_checked_outputs {
            println!("{}", checked_output);
        }
        println!("\n^^^");
        return Err("Python UTF-8 encoding check failed".to_string());
    }

    Ok(())
}
