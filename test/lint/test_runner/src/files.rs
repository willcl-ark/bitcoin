use regex::Regex;
use std::fs;
use std::process::Command;

const ALL_SOURCE_FILENAMES_REGEXP: &str = r"^.*\.(cpp|h|py|sh)$";
const ALLOWED_FILENAME_REGEXP: &str = "^[a-zA-Z0-9/_.@][a-zA-Z0-9/_.@-]*$";
const ALLOWED_SOURCE_FILENAME_REGEXP: &str = "^[a-z0-9_./-]+$";
const ALLOWED_SOURCE_FILENAME_EXCEPTION_REGEXP: &str =
    r"^src/(secp256k1/|minisketch/|test/fuzz/FuzzedDataProvider.h)";
const ALLOWED_PERMISSION_NON_EXECUTABLES: u32 = 0o644;
const ALLOWED_PERMISSION_EXECUTABLES: u32 = 0o755;

const ALLOWED_EXECUTABLE_SHEBANG: [(&str, &[&str]); 2] = [
    ("py", &[r"#!/usr/bin/env python3"]),
    ("sh", &[r"#!/usr/bin/env bash", r"#!/bin/sh"]),
];

struct FileMeta {
    file_path: String,
    permissions: u32,
}

pub fn lint_files() -> Result<(), String> {
    let files = get_git_file_metadata()?;
    let mut failed_tests = 0;

    failed_tests += check_all_filenames(&files)?;
    failed_tests += check_source_filenames(&files)?;
    failed_tests += check_all_file_permissions(&files)?;
    failed_tests += check_shebang_file_permissions(&files)?;

    if failed_tests > 0 {
        Err(format!(
            "ERROR: There were {} failed tests in the lint-files lint test. Please resolve the above errors.",
            failed_tests
        ))
    } else {
        Ok(())
    }
}

fn get_git_file_metadata() -> Result<Vec<FileMeta>, String> {
    let output = Command::new("git")
        .args(["ls-files", "-z", "--full-name", "--stage"])
        .output()
        .map_err(|e| format!("Failed to execute git command: {}", e))?;

    if !output.status.success() {
        return Err("Git command failed".to_string());
    }

    let content = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    let mut files = Vec::new();
    for line in content.split('\0').filter(|s| !s.is_empty()) {
        let parts: Vec<&str> = line.split('\t').collect();
        if parts.len() != 2 {
            continue;
        }

        let meta_parts: Vec<&str> = parts[0].split_whitespace().collect();
        if meta_parts.is_empty() {
            continue;
        }

        let permissions = u32::from_str_radix(meta_parts[0], 8)
            .map_err(|e| format!("Failed to parse permissions: {}", e))?;

        files.push(FileMeta {
            file_path: parts[1].to_string(),
            permissions: permissions & 0o7777,
        });
    }

    Ok(files)
}

fn check_all_filenames(files: &[FileMeta]) -> Result<i32, String> {
    let filename_regex = Regex::new(ALLOWED_FILENAME_REGEXP)
        .map_err(|e| format!("Invalid filename regex: {}", e))?;

    let mut failed_tests = 0;
    for file in files {
        if !filename_regex.is_match(&file.file_path) {
            println!(
                "File {} does not match the allowed filename regexp '{}'.",
                file.file_path, ALLOWED_FILENAME_REGEXP
            );
            failed_tests += 1;
        }
    }

    Ok(failed_tests)
}

fn check_source_filenames(files: &[FileMeta]) -> Result<i32, String> {
    let source_regex = Regex::new(ALL_SOURCE_FILENAMES_REGEXP)
        .map_err(|e| format!("Invalid source filename regex: {}", e))?;
    let filename_regex = Regex::new(ALLOWED_SOURCE_FILENAME_REGEXP)
        .map_err(|e| format!("Invalid source filename regex: {}", e))?;
    let exception_regex = Regex::new(ALLOWED_SOURCE_FILENAME_EXCEPTION_REGEXP)
        .map_err(|e| format!("Invalid exception regex: {}", e))?;

    let mut failed_tests = 0;
    for file in files {
        if source_regex.is_match(&file.file_path)
            && !filename_regex.is_match(&file.file_path)
            && !exception_regex.is_match(&file.file_path)
        {
            println!(
                "File {} does not match the allowed source filename regexp '{}', or the exception regexp '{}'.",
                file.file_path, ALLOWED_SOURCE_FILENAME_REGEXP, ALLOWED_SOURCE_FILENAME_EXCEPTION_REGEXP
            );
            failed_tests += 1;
        }
    }

    Ok(failed_tests)
}

fn check_all_file_permissions(files: &[FileMeta]) -> Result<i32, String> {
    let mut failed_tests = 0;
    for file in files {
        if file.permissions == ALLOWED_PERMISSION_EXECUTABLES {
            let content = fs::read_to_string(&file.file_path)
                .map_err(|e| format!("Failed to read {}: {}", file.file_path, e))?;

            let first_line = content.lines().next().unwrap_or("");
            if !first_line.starts_with("#!") {
                println!(
                    "File \"{}\" has permission {:03o} (executable) and is thus expected to contain a shebang '#!'. Add shebang or do \"chmod {:03o} {}\" to make it non-executable.",
                    file.file_path, ALLOWED_PERMISSION_EXECUTABLES, ALLOWED_PERMISSION_NON_EXECUTABLES, file.file_path
                );
                failed_tests += 1;
            }

            if let Some(extension) = file.file_path.split('.').last() {
                if let Some(shebangs) = ALLOWED_EXECUTABLE_SHEBANG
                    .iter()
                    .find(|(ext, _)| *ext == extension)
                    .map(|(_, shebangs)| *shebangs)
                {
                    if !shebangs.contains(&first_line) {
                        println!(
                            "File \"{}\" is missing expected shebang {}",
                            file.file_path,
                            shebangs.join(" or ")
                        );
                        failed_tests += 1;
                    }
                }
            }
        } else if file.permissions != ALLOWED_PERMISSION_NON_EXECUTABLES {
            println!(
                "File \"{}\" has unexpected permission {:03o}. Do \"chmod {:03o} {}\" (if non-executable) or \"chmod {:03o} {}\" (if executable).",
                file.file_path, file.permissions, ALLOWED_PERMISSION_NON_EXECUTABLES, file.file_path,
                ALLOWED_PERMISSION_EXECUTABLES, file.file_path
            );
            failed_tests += 1;
        }
    }

    Ok(failed_tests)
}

fn check_shebang_file_permissions(files: &[FileMeta]) -> Result<i32, String> {
    let output = Command::new("git")
        .args(["grep", "--full-name", "--line-number", "-I", "^#!"])
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Ok(0);
    }

    let content = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    let mut failed_tests = 0;
    for line in content.lines() {
        if !line.contains(":1:") {
            continue;
        }

        let file_path = line.split(":1:").next().unwrap_or("");
        if let Some(file_meta) = files.iter().find(|f| f.file_path == file_path) {
            if file_meta.permissions != ALLOWED_PERMISSION_EXECUTABLES {
                // Get full extension after first dot
                if let Some(extension) = file_path.split_once('.').map(|(_, ext)| ext) {
                    match extension {
                        "bash" | "init" | "openrc" | "sh.in" => continue,
                        "py" => {
                            let content = fs::read_to_string(file_path)
                                .map_err(|e| format!("Failed to read {}: {}", file_path, e))?;
                            if !content.contains("if __name__ == '__main__':")
                                && !content.contains("if __name__ == \"__main__\":")
                            {
                                continue;
                            }
                        }
                        _ => {}
                    }
                }

                println!(
                    "File \"{}\" contains a shebang line, but has the file permission {:03o} instead of the expected executable permission {:03o}. Do \"chmod {:03o} {}\" (or remove the shebang line).",
                    file_path, file_meta.permissions, ALLOWED_PERMISSION_EXECUTABLES,
                    ALLOWED_PERMISSION_EXECUTABLES, file_path
                );
                failed_tests += 1;
            }
        }
    }

    Ok(failed_tests)
}
