(use-modules (gnu packages)
             (gnu packages base)
             (guix build-system trivial)
             (guix download)
             (guix gexp)
             (guix git-download)
             (guix packages)
             ((guix utils) #:select (substitute-keyword-arguments))
             (toolchains))

;; dedup with re-export-syntax?
(define-syntax-rule (search-our-patches file-name ...)
  "Return the list of absolute file names corresponding to each
FILE-NAME found in ./patches relative to the current file."
  (parameterize
      ((%patch-path (list (string-append (dirname (current-filename)) "/patches"))))
    (list (search-patch file-name) ...)))

;; --enable-static-nss isn't used yet, because it has been broken
;; since 2.33: https://sourceware.org/bugzilla/show_bug.cgi?id=27959.
(define glibc-2.43
  (let ((commit "4070d808bea1c077eb7e7d52b52b91cae98205d5"))
  (package
    (inherit glibc) ;; 2.39
    (version "2.43")
    (source (origin
              (method git-fetch)
              (uri (git-reference
                    (url "https://sourceware.org/git/glibc.git")
                    (commit commit)))
              (file-name (git-file-name "glibc" commit))
              (sha256
               (base32
                "14f6ayljaja5wjz1bm4fwabxrjqbwh39781gx00l9ksvxnvqjp8c"))
              (patches (search-our-patches "glibc-guix-2.43-prefix.patch"
                                           "glibc-nss-nodlopen.patch"))))
    (arguments
      (substitute-keyword-arguments (package-arguments glibc)
        ((#:configure-flags flags)
          `(append ,flags
            ;; https://www.gnu.org/software/libc/manual/html_node/Configuring-and-compiling.html
            (list "--enable-bind-now",
                  "--enable-cet=yes",
                  "--enable-fortify-source",
                  "--enable-stack-protector=all",
                  "--disable-nscd",
                  "--disable-profile",
                  "--disable-pt_chown",
                  "--disable-timezone-tools",
                  "--disable-werror",
                  building-on))))))))

(packages->manifest
 (append
  (let ((target (getenv "HOST")))
    (cond ((string-contains target "-linux-")
           (list (make-bitcoin-cross-toolchain target
                                               #:base-libc glibc-2.43)))
          (else '())))))
