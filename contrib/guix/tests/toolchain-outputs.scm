;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(use-modules (bitcoin toolchains)
             (guix derivations)
             (guix gexp)
             (guix store)
             (srfi srfi-1)
             (srfi srfi-13))

;; A generated script can contain the right store path while its derivation
;; omits that output. Check the declared inputs, not just the script's text.
(with-store store
  (for-each
   (lambda (target)
     (let ((drv (run-with-store store
                  (lower-object
                   (toolchain-environment (release-toolchain target 'build))))))
       (define (require-output name output)
         (unless (any (lambda (input)
                        (and (string-contains (derivation-input-path input)
                                              (string-append "-" name "-"))
                             (member output (derivation-input-sub-derivations input))))
                      (derivation-inputs drv))
           (error "toolchain environment omits a required input output"
                  target name output)))
       (if (string-contains target "darwin")
           (begin
             (require-output "clang-toolchain" "out")
             (require-output "libcxx" "out"))
           (begin
             (require-output "gcc-toolchain" "out")
             (require-output (string-append "gcc-cross-" target) "out")
             (require-output (string-append "gcc-cross-" target) "lib")))
       (when (string-contains target "-linux-")
         (require-output "gcc-toolchain" "static")
         (require-output (string-append "glibc-cross-" target) "out")
         (require-output (string-append "glibc-cross-" target) "static"))))
   '("x86_64-linux-gnu" "arm-linux-gnueabihf" "aarch64-linux-gnu"
     "riscv64-linux-gnu" "powerpc64-linux-gnu" "x86_64-w64-mingw32"
     "x86_64-apple-darwin" "arm64-apple-darwin")))

(display "toolchain output declarations checked for 8 release targets\n")
