use std::process::Command;

const EXPECTED_CIRCULAR_DEPENDENCIES: &[&str] = &[
    "chainparamsbase -> common/args -> chainparamsbase",
    "node/blockstorage -> validation -> node/blockstorage",
    "node/utxo_snapshot -> validation -> node/utxo_snapshot",
    "qt/addresstablemodel -> qt/walletmodel -> qt/addresstablemodel",
    "qt/recentrequeststablemodel -> qt/walletmodel -> qt/recentrequeststablemodel",
    "qt/sendcoinsdialog -> qt/walletmodel -> qt/sendcoinsdialog",
    "qt/transactiontablemodel -> qt/walletmodel -> qt/transactiontablemodel",
    "wallet/wallet -> wallet/walletdb -> wallet/wallet",
    "kernel/coinstats -> validation -> kernel/coinstats",
    "index/base -> node/context -> net_processing -> index/blockfilterindex -> index/base",
];

pub fn check_circular_dependencies() -> Result<(), String> {
    let git_root = crate::get_git_root();
    let script_path = git_root.join("contrib/devtools/circular-dependencies.py");
    let src_dir = git_root.join("src");

    let files_output = Command::new("git")
        .args(["ls-files", "--", "*.h", "*.cpp"])
        .current_dir(&src_dir)
        .output()
        .map_err(|e| format!("Failed to list files: {}", e))?;

    if !files_output.status.success() {
        return Err("Failed to get list of files".to_string());
    }

    let output_str = String::from_utf8_lossy(&files_output.stdout);
    let files: Vec<&str> = output_str.lines().collect();

    let mut code = Command::new(&script_path);
    code.current_dir(&src_dir);
    code.args(&files);

    let output = match code.output() {
        Ok(output) => output,
        Err(e) => {
            return Err(format!(
                "Failed to execute circular dependencies check: {}",
                e
            ))
        }
    };

    let found_deps = String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter(|line| line.starts_with("Circular dependency: "))
        .map(|line| line.trim_start_matches("Circular dependency: ").to_string())
        .collect::<Vec<_>>();

    let mut failed_tests = 0;

    // Check for unexpected dependencies
    for dep in &found_deps {
        if !EXPECTED_CIRCULAR_DEPENDENCIES.contains(&dep.as_str()) {
            eprintln!(
                "A new circular dependency in the form of \"{}\" appears to have been introduced.\n",
                dep
            );
            failed_tests += 1;
        }
    }

    // Check for missing expected dependencies
    for expected_dep in EXPECTED_CIRCULAR_DEPENDENCIES {
        if !found_deps.iter().any(|d| d == *expected_dep) {
            println!(
                "Good job! The circular dependency \"{}\" is no longer present.",
                expected_dep
            );
            println!(
                "Please remove it from EXPECTED_CIRCULAR_DEPENDENCIES in {}",
                file!()
            );
            println!("to make sure this circular dependency is not accidentally reintroduced.\n");
            failed_tests += 1;
        }
    }

    if failed_tests > 0 {
        Err("Circular dependencies check failed".to_string())
    } else {
        Ok(())
    }
}
