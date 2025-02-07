use crate::{check_output, git, LintResult};

pub fn lint_doc_release_note_snippets() -> LintResult {
    let non_release_notes = check_output(git().args([
        "ls-files",
        "--",
        "doc/release-notes/",
        ":(exclude)doc/release-notes/*.*.md", // Assume that at least one dot implies a proper release note
    ]))?;
    if non_release_notes.is_empty() {
        Ok(())
    } else {
        println!("{non_release_notes}");
        Err(r#"
Release note snippets and other docs must be put into the doc/ folder directly.

The doc/release-notes/ folder is for archived release notes of previous releases only. Snippets are
expected to follow the naming "/doc/release-notes-<PR number>.md".
            "#
        .trim()
        .to_string())
    }
}
