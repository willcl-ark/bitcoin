use regex::Regex;
use std::process::Command;
use std::sync::mpsc;
use std::sync::Arc;
use std::thread;

const KNOWN_VIOLATIONS: &[&str] = &[
    "src/dbwrapper.cpp:.*vsnprintf",
    "src/test/fuzz/locale.cpp:.*setlocale",
    "src/test/util_tests.cpp:.*strtoll",
    "src/wallet/bdb.cpp:.*DbEnv::strerror",
    "src/util/syserror.cpp:.*strerror",
];

const REGEXP_EXTERNAL_DEPENDENCIES_EXCLUSIONS: &[&str] = &[
    "src/crypto/ctaes/",
    "src/leveldb/",
    "src/secp256k1/",
    "src/minisketch/",
    "src/tinyformat.h",
];

const LOCALE_DEPENDENT_FUNCTIONS: &[&str] = &[
    "alphasort", // LC_COLLATE (via strcoll)
    "asctime",   // LC_TIME (directly)
    "asprintf",  // (via vasprintf)
    "atof",      // LC_NUMERIC (via strtod)
    "atoi",      // LC_NUMERIC (via strtol)
    "atol",      // LC_NUMERIC (via strtol)
    "atoll",     // (via strtoll)
    "atoq",
    "btowc",   // LC_CTYPE (directly)
    "ctime",   // (via asctime or localtime)
    "dprintf", // (via vdprintf)
    "fgetwc",
    "fgetws",
    "fold_case", // boost::locale::fold_case
    "fprintf",   // (via vfprintf)
    "fputwc",
    "fputws",
    "fscanf",   // (via __vfscanf)
    "fwprintf", // (via __vfwprintf)
    "getdate",  // via __getdate_r => isspace // __localtime_r
    "getwc",
    "getwchar",
    "is_digit",   // boost::algorithm::is_digit
    "is_space",   // boost::algorithm::is_space
    "isalnum",    // LC_CTYPE
    "isalpha",    // LC_CTYPE
    "isblank",    // LC_CTYPE
    "iscntrl",    // LC_CTYPE
    "isctype",    // LC_CTYPE
    "isdigit",    // LC_CTYPE
    "isgraph",    // LC_CTYPE
    "islower",    // LC_CTYPE
    "isprint",    // LC_CTYPE
    "ispunct",    // LC_CTYPE
    "isspace",    // LC_CTYPE
    "isupper",    // LC_CTYPE
    "iswalnum",   // LC_CTYPE
    "iswalpha",   // LC_CTYPE
    "iswblank",   // LC_CTYPE
    "iswcntrl",   // LC_CTYPE
    "iswctype",   // LC_CTYPE
    "iswdigit",   // LC_CTYPE
    "iswgraph",   // LC_CTYPE
    "iswlower",   // LC_CTYPE
    "iswprint",   // LC_CTYPE
    "iswpunct",   // LC_CTYPE
    "iswspace",   // LC_CTYPE
    "iswupper",   // LC_CTYPE
    "iswxdigit",  // LC_CTYPE
    "isxdigit",   // LC_CTYPE
    "localeconv", // LC_NUMERIC + LC_MONETARY
    "mblen",      // LC_CTYPE
    "mbrlen",
    "mbrtowc",
    "mbsinit",
    "mbsnrtowcs",
    "mbsrtowcs",
    "mbstowcs", // LC_CTYPE
    "mbtowc",   // LC_CTYPE
    "mktime",
    "normalize", // boost::locale::normalize
    "printf",    // LC_NUMERIC
    "putwc",
    "putwchar",
    "scanf", // LC_NUMERIC
    "setlocale",
    "snprintf",
    "sprintf",
    "sscanf",
    "std::locale::global",
    "std::to_string",
    "stod",
    "stof",
    "stoi",
    "stol",
    "stold",
    "stoll",
    "stoul",
    "stoull",
    "strcasecmp",
    "strcasestr",
    "strcoll", // LC_COLLATE
    "strerror",
    "strfmon",
    "strftime", // LC_TIME
    "strncasecmp",
    "strptime",
    "strtod", // LC_NUMERIC
    "strtof",
    "strtoimax",
    "strtol", // LC_NUMERIC
    "strtold",
    "strtoll",
    "strtoq",
    "strtoul", // LC_NUMERIC
    "strtoull",
    "strtoumax",
    "strtouq",
    "strxfrm", // LC_COLLATE
    "swprintf",
    "to_lower", // boost::locale::to_lower
    "to_title", // boost::locale::to_title
    "to_upper", // boost::locale::to_upper
    "tolower",  // LC_CTYPE
    "toupper",  // LC_CTYPE
    "towctrans",
    "towlower",   // LC_CTYPE
    "towupper",   // LC_CTYPE
    "trim",       // boost::algorithm::trim
    "trim_left",  // boost::algorithm::trim_left
    "trim_right", // boost::algorithm::trim_right
    "ungetwc",
    "vasprintf",
    "vdprintf",
    "versionsort",
    "vfprintf",
    "vfscanf",
    "vfwprintf",
    "vprintf",
    "vscanf",
    "vsnprintf",
    "vsprintf",
    "vsscanf",
    "vswprintf",
    "vwprintf",
    "wcrtomb",
    "wcscasecmp",
    "wcscoll",  // LC_COLLATE
    "wcsftime", // LC_TIME
    "wcsncasecmp",
    "wcsnrtombs",
    "wcsrtombs",
    "wcstod", // LC_NUMERIC
    "wcstof",
    "wcstoimax",
    "wcstol", // LC_NUMERIC
    "wcstold",
    "wcstoll",
    "wcstombs", // LC_CTYPE
    "wcstoul",  // LC_NUMERIC
    "wcstoull",
    "wcstoumax",
    "wcswidth",
    "wcsxfrm", // LC_COLLATE
    "wctob",
    "wctomb", // LC_CTYPE
    "wctrans",
    "wctype",
    "wcwidth",
    "wprintf",
];

const CHUNK_SIZE: usize = 20;

#[derive(Clone)]
struct FailureInfo {
    function: String,
    matches: Vec<String>,
}

fn get_exclude_args() -> Vec<String> {
    REGEXP_EXTERNAL_DEPENDENCIES_EXCLUSIONS
        .iter()
        .map(|dir| format!(":(exclude){}", dir))
        .collect()
}

fn find_locale_dependent_function_uses(functions: &[&str]) -> Result<Vec<String>, String> {
    let regexp = format!(
        r#"[^a-zA-Z0-9_\\`'\"<>]({})((_r|_s)?[^a-zA-Z0-9_\\`'\"<>])"#,
        functions.join("|")
    );

    let mut git_grep_command = Command::new("git");
    git_grep_command
        .args(["grep", "-E", &regexp, "--", "*.cpp", "*.h"])
        .args(get_exclude_args());

    let output = git_grep_command
        .output()
        .map_err(|e| format!("Failed to execute git grep: {}", e))?;

    if !output.status.success() && !output.status.code().map_or(false, |c| c == 1) {
        return Ok(Vec::new());
    }

    Ok(String::from_utf8(output.stdout)
        .map_err(|e| format!("Invalid UTF-8 in git output: {}", e))?
        .lines()
        .map(String::from)
        .collect())
}

pub fn lint_locale_dependence() -> Result<(), String> {
    let chunks: Vec<Vec<&str>> = LOCALE_DEPENDENT_FUNCTIONS
        .chunks(CHUNK_SIZE)
        .map(|chunk| chunk.to_vec())
        .collect();

    let (tx, rx) = mpsc::channel();
    let known_violations_regex =
        Arc::new(Regex::new(&KNOWN_VIOLATIONS.join("|")).expect("Invalid regex pattern"));

    let mut handles = vec![];

    for chunk in chunks {
        let tx = tx.clone();
        let known_violations_regex = Arc::clone(&known_violations_regex);

        handles.push(thread::spawn(move || {
            if let Ok(git_grep_output) = find_locale_dependent_function_uses(&chunk) {
                for &func in &chunk {
                    let re_func = Regex::new(&format!(
                        r"[^a-zA-Z0-9_'\x22<>]{}(_r|_s)?[^a-zA-Z0-9_'\x22<>]",
                        func
                    ))
                    .expect("Invalid regex pattern");

                    let re_comment =
                        Regex::new(&format!(r"\.(c|cpp|h):\s*(//|\*|/\*|\x22).*{}", func))
                            .expect("Invalid regex pattern");

                    let matches: Vec<String> = git_grep_output
                        .iter()
                        .filter(|line| {
                            re_func.is_match(line)
                                && !re_comment.is_match(line)
                                && !known_violations_regex.is_match(line)
                        })
                        .cloned()
                        .collect();

                    if !matches.is_empty() {
                        let _ = tx.send(FailureInfo {
                            function: func.to_string(),
                            matches,
                        });
                    }
                }
            }
        }));
    }

    // Close sender
    drop(tx);

    // Wait for all threads to complete
    for handle in handles {
        handle.join().unwrap();
    }

    // Collect results
    let failures: Vec<FailureInfo> = rx.iter().collect();

    if !failures.is_empty() {
        let mut error_msg = String::new();
        for failure in failures {
            error_msg.push_str(&format!(
                "The locale dependent function {}(...) appears to be used:\n",
                failure.function
            ));
            for line in &failure.matches {
                error_msg.push_str(&format!("{}\n", line));
            }
            error_msg.push('\n');
        }

        error_msg.push_str(
            "Unnecessary locale dependence can cause bugs that are very tricky to isolate and fix.\n\
            Please avoid using locale-dependent functions if possible.\n\n\
            Advice not applicable in this specific case? Add an exception by updating the KNOWN_VIOLATIONS list in the module."
        );

        Err(error_msg)
    } else {
        Ok(())
    }
}
