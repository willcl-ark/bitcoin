;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.
;;;
;;; Run with the pinned Guix: guix repl -L contrib/guix/modules -- this-file

(use-modules (bitcoin depends build) (guix build utils) (guix build syscalls))

(define (assert condition message)
  (unless condition (error message)))

(define (rejects? thunk)
  (catch #t (lambda () (thunk) #f) (lambda _ #t)))

(let* ((root (mkdtemp! (string-copy "/tmp/bitcoin-depends-merge.XXXXXX")))
       (source (string-append root "/source"))
       (target (string-append root "/target")))
  (dynamic-wind
    (lambda ()
      (mkdir source)
      (call-with-output-file (string-append source "/header")
        (lambda (port) (display "header contents\n" port)))
      (symlink "header" (string-append source "/link")))
    (lambda ()
      (merge-depends source target)
      (merge-depends source target)
      (assert (string=? (readlink (string-append target "/link")) "header")
              "relative symlink was not preserved")
      (chmod (string-append source "/header") #o755)
      (assert (rejects? (lambda () (merge-depends source target)))
              "executable-mode collision was accepted")
      (chmod (string-append source "/header") #o644)
      (call-with-output-file (string-append source "/header")
        (lambda (port) (display "different contents\n" port)))
      (assert (rejects? (lambda () (merge-depends source target)))
              "file-content collision was accepted")
      (delete-file (string-append source "/header"))
      (mkdir (string-append source "/header"))
      (assert (rejects? (lambda () (merge-depends source target)))
              "file/directory collision was accepted")
      (rmdir (string-append source "/header"))
      (delete-file (string-append source "/link"))
      (symlink "elsewhere" (string-append source "/link"))
      (assert (rejects? (lambda () (merge-depends source target)))
              "symlink collision was accepted")
      (display "depends merge checks passed\n"))
    (lambda () (delete-file-recursively root))))
