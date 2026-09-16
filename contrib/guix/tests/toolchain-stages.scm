;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(use-modules (bitcoin toolchains)
             (bitcoin manifests)
             (bitcoin depends packages)
             (bitcoin depends assembly)
             (guix packages)
             (guix gexp)
             (guix profiles)
             (guix derivations)
             (guix monads)
             (guix store)
             (guix tests)
             (srfi srfi-1))

(define target "x86_64-linux-gnu")

(define (check condition message)
  (unless condition (error message)))

(with-store store
  (define (package-drv package)
    (derivation-file-name (package-derivation store package)))
  (define (package-drvs gui?)
    (map (lambda (entry) (cons (car entry) (package-drv (cdr entry))))
         (release-packages target #:gui? gui?)))
  (define (assembly-drv stage)
    (package-drv (depends-assembly target #:stage stage)))
  (define (manifest-drv stage)
    (derivation-file-name
     (run-with-store store
       (profile-derivation
        (build-manifest target (toolchain-packages (release-toolchain target stage)))))))
  (let* ((base (package-drvs #f))
         (gui (package-drvs #t))
         (base-assembly (assembly-drv 'build))
         (gui-assembly (assembly-drv 'gui))
         (base-manifest (manifest-drv 'build))
         (gui-manifest (manifest-drv 'gui))
         (default-toolchain (release-toolchain target 'gui))
         (packages (toolchain-packages default-toolchain))
         ;; Change a toolchain input's derivation without building a compiler.
         (alternate (append (drop-right packages 1)
                            (list (package
                                    (inherit (last packages))
                                    (version "stage-test"))))))
    (check (equal? base-manifest gui-manifest) "default stage compilers differ")
    (check (not (assoc "qt" base)) "base graph unexpectedly includes Qt")
    (for-each (lambda (entry)
                (check (equal? (cdr entry) (assoc-ref gui (car entry)))
                       "identical stage toolchains do not share a dependency"))
              base)
    (mock ((bitcoin toolchains) gui-toolchain
           (lambda (_)
             (toolchain
               (packages alternate)
               (environment (toolchain-environment-file target alternate)))))
      (check (equal? base (package-drvs #f)) "GUI toolchain changed base dependencies")
      (check (equal? base-assembly (assembly-drv 'build)) "GUI toolchain changed base prefix")
      (check (equal? base-manifest (manifest-drv 'build)) "GUI toolchain changed base manifest")
      (check (not (equal? gui-manifest (manifest-drv 'gui))) "GUI manifest ignored its toolchain")
      (check (not (equal? gui-assembly (assembly-drv 'gui))) "GUI prefix ignored its toolchain")
      (let ((changed (package-drvs #t)))
        (for-each (lambda (entry)
                    (check (not (equal? (cdr entry) (assoc-ref changed (car entry))))
                           "GUI dependency ignored its toolchain"))
                  gui)))
    (mock ((bitcoin toolchains) gui-toolchain
           (lambda (_)
             (toolchain
               (inherit default-toolchain)
               (environment (plain-file "toolchain-test-environment" "# changed environment\n")))))
      (check (equal? base (package-drvs #f)) "GUI environment changed base dependencies")
      (check (equal? base-manifest (manifest-drv 'build)) "GUI environment changed base manifest")
      (check (not (equal? gui-assembly (assembly-drv 'gui))) "GUI prefix ignored its environment")
      (let ((changed (package-drvs #t)))
        (for-each (lambda (entry)
                    (check (not (equal? (cdr entry) (assoc-ref changed (car entry))))
                           "GUI dependency ignored its environment"))
                  gui)))))

(display "stage toolchain sharing and independent invalidation checks passed\n")
