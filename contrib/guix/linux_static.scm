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

(define glibc-2.31
  (let ((commit "28eb5caf895ced5d895cb02757e109004a2d33e5"))
  (package
    (inherit glibc) ;; 2.39
    (version "2.31")
    (source (origin
              (method git-fetch)
              (uri (git-reference
                    (url "https://sourceware.org/git/glibc.git")
                    (commit commit)))
              (file-name (git-file-name "glibc" commit))
              (sha256
               (base32
                "07arjrc1smqy8wrhg38apr1s9ji7xv1rpzdapk4k2ps2n07irp58"))
              (patches (search-our-patches "glibc-guix-prefix.patch"
                                           "glibc-riscv-jumptarget.patch"))))
    (arguments
      (substitute-keyword-arguments (package-arguments glibc)
        ((#:configure-flags flags)
          `(append ,flags
            ;; https://www.gnu.org/software/libc/manual/html_node/Configuring-and-compiling.html
            (list "--enable-stack-protector=all",
                  "--enable-cet",
                  "--enable-bind-now",
                  "--disable-werror",
                  "--disable-timezone-tools",
                  "--disable-profile",
                  building-on)))
    ((#:phases phases)
        `(modify-phases ,phases
           (add-before 'configure 'set-etc-rpc-installation-directory
             (lambda* (#:key outputs #:allow-other-keys)
               ;; Install the rpc data base file under `$out/etc/rpc'.
               ;; Otherwise build will fail with "Permission denied."
               ;; Can be removed when we are building 2.32 or later.
               (let ((out (assoc-ref outputs "out")))
                 (substitute* "sunrpc/Makefile"
                   (("^\\$\\(inst_sysconfdir\\)/rpc(.*)$" _ suffix)
                    (string-append out "/etc/rpc" suffix "\n"))
                   (("^install-others =.*$")
                    (string-append "install-others = " out "/etc/rpc\n")))))))))))))

(packages->manifest
 (append
  (let ((target (getenv "HOST")))
    (cond ((string-contains target "-linux-")
           (list (make-bitcoin-cross-toolchain target
                                               #:base-libc glibc-2.31))) ;; will be 2.43 based
          (else '())))))
