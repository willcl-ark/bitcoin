;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(define-module (bitcoin depends assembly)
  #:use-module (bitcoin depends)
  #:use-module (bitcoin depends packages)
  #:use-module (guix packages)
  #:use-module (guix gexp)
  #:use-module (guix modules)
  #:use-module (guix utils)
  #:use-module (guix build-system trivial)
  #:export (depends-assembly))

(define %root
  (canonicalize-path
   (string-append (dirname (search-path %load-path "bitcoin/depends/assembly.scm"))
                  "/../../../../..")))

(define* (depends-assembly target #:key sdk (system (%current-system)))
  (let ((packages (map cdr (release-packages target #:gui? #t #:sdk sdk #:system system)))
        (profile (depends-profile target))
        (build (cond ((string=? system "x86_64-linux") "x86_64-pc-linux-gnu")
                     ((string=? system "aarch64-linux") "aarch64-unknown-linux-gnu")
                     (else (error "unsupported release build system" system)))))
    (package
      (name (string-append "bitcoin-depends-" target))
      (version "1")
      (source #f)
      (build-system trivial-build-system)
      (arguments
       (list
        #:modules (source-module-closure '((bitcoin depends build)
                                          (bitcoin depends toolchain))
                                         #:select? (lambda (name)
                                                     (or (guix-module-name? name)
                                                         (eq? (car name) 'bitcoin))))
        #:builder
        #~(begin
            (use-modules (bitcoin depends build) (bitcoin depends toolchain)
                         (guix build utils) (ice-9 textual-ports)
                         (ice-9 string-fun) (srfi srfi-1) (srfi srfi-13))
            (define prefix (string-append #$output "/prefix"))
            (define bin (string-append #$profile "/bin/"))
            (define tools
              (cond
               ((string-contains #$target "darwin")
                (map (lambda (entry)
                       (cons (car entry) (string-append bin (cdr entry))))
                     '((cc . "clang") (cxx . "clang++") (ar . "llvm-ar")
                       (ranlib . "llvm-ranlib") (strip . "llvm-strip")
                       (objcopy . "llvm-objcopy") (objdump . "llvm-objdump"))))
               ((string-suffix? "-mingw32" #$target)
                (map (lambda (entry)
                       (let* ((program (string-append #$target "-" (cdr entry)))
                              (posix (string-append program "-posix")))
                         (cons (car entry)
                               (if (file-exists? (string-append bin posix)) posix program))))
                     '((cc . "gcc") (cxx . "g++"))))
               (else '())))
            (mkdir-p prefix)
            ;; Copying payloads alone would not keep the individual package
            ;; outputs reachable from this assembly's garbage-collector root.
            (mkdir (string-append #$output "/packages"))
            (for-each
             (lambda (name input)
               (merge-depends input prefix)
               (symlink input (string-append #$output "/packages/" name)))
             '#$(map package-name packages)
             (list #$@packages))
            (when #$sdk (symlink #$sdk (string-append #$output "/sdk")))
            (for-each
             (lambda (gui?)
               (let* ((substitutions (depends-toolchain-substitutions #$target #$build gui? #:tools tools))
                      (template (call-with-input-file
                                    #$(local-file (string-append %root "/depends/toolchain.cmake.in"))
                                  get-string-all))
                      (text (fold (lambda (entry text)
                                    (string-replace-substring text (car entry) (cdr entry)))
                                  template substitutions)))
                 (call-with-output-file
                     (string-append prefix (if gui? "/toolchain.cmake" "/toolchain-base.cmake"))
                   (lambda (port) (display text port)))))
             '(#f #t)))))
      (home-page "https://bitcoincore.org")
      (synopsis "Bitcoin Core release dependency prefix")
      (description "The shared dependency package set for both release build stages.")
      (license #f))))
