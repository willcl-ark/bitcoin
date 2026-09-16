;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(define-module (bitcoin depends build)
  #:use-module (guix build utils)
  #:use-module (ice-9 ftw)
  #:use-module (rnrs io ports)
  #:use-module (rnrs bytevectors)
  #:export (merge-depends))

(define (same-file? first second)
  (call-with-input-file first
    (lambda (a)
      (call-with-input-file second
        (lambda (b)
          (let loop ()
            (let ((x (get-bytevector-n a 65536))
                  (y (get-bytevector-n b 65536)))
              (cond ((eof-object? x) (eof-object? y))
                    ((eof-object? y) #f)
                    ((bytevector=? x y) (loop))
                    (else #f)))))))))

(define (merge-depends source destination)
  "Copy SOURCE into DESTINATION, rejecting conflicting installed files."
  (let* ((source-stat (lstat source))
         (existing (false-if-exception (lstat destination)))
         (type (stat:type source-stat)))
    (when (and existing (not (eq? type (stat:type existing))))
      (error "depends file type collision" source destination))
    (case type
      ((directory)
       (mkdir-p destination)
       (for-each (lambda (name)
                   (merge-depends (string-append source "/" name)
                                  (string-append destination "/" name)))
                 (scandir source (lambda (name) (not (member name '("." "..")))))))
      ((symlink)
       (if existing
           (unless (string=? (readlink source) (readlink destination))
             (error "depends symlink collision" source destination))
           (symlink (readlink source) destination)))
      ((regular)
       (if existing
           (unless (and (= (logand (stat:perms source-stat) #o111)
                           (logand (stat:perms existing) #o111))
                        (same-file? source destination))
             (error "depends file collision" source destination))
           (copy-file source destination)))
      (else (error "unsupported depends file type" source type)))))
