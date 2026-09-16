;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(add-to-load-path (string-append (dirname (current-filename)) "/modules"))

(use-modules (bitcoin depends assembly)
             (guix gexp))

(let* ((target (or (getenv "HOST") (error "HOST is required")))
       (stage (string->symbol (or (getenv "GUIX_BUILD_STAGE") "build")))
       (darwin? (string-contains target "darwin"))
       (sdk (and darwin?
                 (local-file (canonicalize-path
                              (or (getenv "GUIX_DEPENDS_SDK")
                                  (error "GUIX_DEPENDS_SDK is required for Darwin")))
                             "bitcoin-macos-sdk" #:recursive? #t))))
  (depends-assembly target #:stage stage #:sdk sdk))
