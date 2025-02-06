pub const SHARED_EXCLUDED_SUBTREES: &[&str] = &[
    "src/leveldb/",
    "src/crc32c/",
    "src/secp256k1/",
    "src/minisketch/",
];

pub fn get_shared_exclude_args() -> Vec<String> {
    SHARED_EXCLUDED_SUBTREES
        .iter()
        .map(|dir| format!(":(exclude){}", dir))
        .collect()
}

// Helper function to get git exclude args commonly used across linters
pub fn get_exclude_args(additional_excludes: &[&str]) -> Vec<String> {
    let mut excludes = SHARED_EXCLUDED_SUBTREES.to_vec();
    excludes.extend_from_slice(additional_excludes);
    excludes
        .iter()
        .map(|dir| format!(":(exclude){}", dir))
        .collect()
}
