use std::collections::HashSet;

pub const SHARED_EXCLUDED_SUBTREES: &[&str] = &[
    "src/leveldb/",
    "src/crc32c/",
    "src/secp256k1/",
    "src/minisketch/",
    "src/crypto/ctaes/",
];

pub const SHARED_EXCLUDED_FILES: &[&str] = &[
    "*.patch",
    "src/qt/locale",
    "contrib/windeploy/win-codesign.cert",
    "doc/README_windows.txt",
    "doc/release-notes/release-notes-0.*",
    "contrib/init/bitcoind.openrc",
    "contrib/macdeploy/macdeployqtplus",
    "src/crypto/sha256_sse4.cpp",
    "src/qt/res/src/*.svg",
    "test/functional/test_framework/crypto/ellswift_decode_test_vectors.csv",
    "test/functional/test_framework/crypto/xswiftec_inv_test_vectors.csv",
    "contrib/qos/tc.sh",
    "contrib/verify-commits/gpg.sh",
    "src/univalue/include/univalue_escapes.h",
    "src/univalue/test/object.cpp",
    "test/lint/git-subtree-check.sh",
];

pub fn get_subtrees() -> Vec<&'static str> {
    SHARED_EXCLUDED_SUBTREES.to_vec()
}

/// Return the pathspecs to exclude all subtrees
pub fn get_pathspecs_exclude_subtrees() -> Vec<String> {
    get_subtrees()
        .iter()
        .map(|s| format!(":(exclude){}", s))
        .collect()
}

pub fn get_shared_exclude_args() -> Vec<String> {
    SHARED_EXCLUDED_SUBTREES
        .iter()
        .map(|dir| format!(":(exclude){}", dir))
        .collect()
}

pub fn get_exclude_args(additional_excludes: &[&str]) -> Vec<String> {
    let mut excludes = HashSet::new();
    excludes.extend(SHARED_EXCLUDED_SUBTREES.iter().copied());
    excludes.extend(additional_excludes.iter().copied());
    excludes
        .into_iter()
        .map(|dir| format!(":(exclude){}", dir))
        .collect()
}

pub fn get_all_exclude_args() -> Vec<String> {
    let mut excludes = HashSet::new();
    excludes.extend(SHARED_EXCLUDED_SUBTREES.iter().copied());
    excludes.extend(SHARED_EXCLUDED_FILES.iter().copied());
    excludes
        .into_iter()
        .map(|path| format!(":(exclude){}", path))
        .collect()
}
