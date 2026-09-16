;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(define-module (bitcoin depends packages)
  #:use-module (bitcoin depends)
  #:use-module (bitcoin depends base)
  #:use-module (bitcoin depends gui)
  #:use-module (bitcoin depends qt)
  #:use-module (guix utils)
  #:use-module (srfi srfi-1)
  #:export (release-recipes release-packages))

(define (build-triplet system)
  ;; Match depends/config.guess on the supported release build machines.
  (cond ((string=? system "x86_64-linux") "x86_64-pc-linux-gnu")
        ((string=? system "aarch64-linux") "aarch64-unknown-linux-gnu")
        (else (error "unsupported release build system" system))))

(define* (release-recipes target #:key gui? (system (%current-system)))
  (let ((build (build-triplet system)))
    (append
     (filter (lambda (recipe)
               (let ((name (assoc-ref recipe 'name)))
                 (and (or gui? (not (string=? name "qrencode")))
                      (or (string-contains target "-linux-")
                          (not (string=? name "systemtap")))
                      (or (not (string-suffix? "-mingw32" target))
                          (not (member name '("capnp" "native_capnp" "native_libmultiprocess")))))))
             (base-recipes target build))
     (if gui? (append (gui-recipes target build) (qt-recipes target build)) '()))))

(define* (release-packages target #:key gui? sdk (system (%current-system)))
  "Return the selected package graph; dependencies are separate store builds."
  (let* ((recipes (release-recipes target #:gui? gui? #:system system))
         (build (build-triplet system))
         (cache '()))
    (define (recipe name)
      (or (find (lambda (entry) (string=? name (assoc-ref entry 'name))) recipes)
          (error "unknown depends package" name)))
    (define (closure name)
      (let ((direct (assoc-ref (recipe name) 'dependencies)))
        (sort (delete-duplicates (append direct (append-map closure direct))) string<?)))
    (define (package-for name)
      (or (assoc-ref cache name)
          (let ((package (depends-package (recipe name) target build
                                          (map package-for (closure name))
                                          #:sdk sdk)))
            (set! cache (acons name package cache))
            package)))
    (map (lambda (entry)
           (cons (assoc-ref entry 'name) (package-for (assoc-ref entry 'name))))
         recipes)))
