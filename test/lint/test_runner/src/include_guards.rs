use regex::Regex;
use std::path::Path;
use std::process::Command;

const HEADER_ID_PREFIX: &str = "BITCOIN_";
const HEADER_ID_SUFFIX: &str = "_H";

fn get_header_file_list() -> Result<Vec<String>, String> {
    let output = Command::new("git")
        .args(["ls-files", "--", "*.h"])
        .output()
        .map_err(|e| format!("Failed to execute git command: {}", e))?;

    if !output.status.success() {
        return Err("Git ls-files command failed".to_string());
    }

    let files = String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?;

    let excluded_prefixes = [
        "contrib/devtools/bitcoin-tidy",
        "src/crypto/ctaes",
        "src/tinyformat.h",
        "src/bench/nanobench.h",
        "src/test/fuzz/FuzzedDataProvider.h",
        "src/leveldb/",
        "src/crc32c/",
        "src/secp256k1/",
        "src/minisketch/",
    ];

    let files: Vec<String> = files
        .lines()
        .filter(|file| {
            !excluded_prefixes
                .iter()
                .any(|prefix| file.starts_with(prefix))
        })
        .map(String::from)
        .collect();

    Ok(files)
}

fn get_header_id(header_file: &str) -> String {
    let path = Path::new(header_file);
    let header_id_base = path
        .components()
        .skip(1) // Skip the first component
        .map(|comp| comp.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("_");

    let header_id_base = header_id_base
        .replace(".h", "")
        .replace('-', "_")
        .to_uppercase();

    format!("{}{}{}", HEADER_ID_PREFIX, header_id_base, HEADER_ID_SUFFIX)
}

pub fn check_include_guards() -> Result<(), String> {
    let header_files = get_header_file_list()?;
    let mut failed_tests = 0;

    for header_file in header_files {
        let header_id = get_header_id(&header_file);
        let pattern = format!(r"^#(ifndef|define|endif //) {}", header_id);
        let regex = Regex::new(&pattern).map_err(|e| format!("Invalid regex pattern: {}", e))?;

        let content = std::fs::read_to_string(&header_file)
            .map_err(|e| format!("Failed to read {}: {}", header_file, e))?;

        let count = content.lines().filter(|line| regex.is_match(line)).count();

        if count != 3 {
            println!(
                "{} seems to be missing the expected include guard:",
                header_file
            );
            println!("  #ifndef {}", header_id);
            println!("  #define {}", header_id);
            println!("  ...");
            println!("  #endif // {}\n", header_id);
            failed_tests += 1;
        }
    }

    if failed_tests > 0 {
        Err(format!(
            "Found {} files with incorrect include guards",
            failed_tests
        ))
    } else {
        Ok(())
    }
}
