#!/usr/bin/env -S guile -s
!#
;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(use-modules (bitcoin depends toolchain)
             (guix build syscalls)
             (guix build utils)
             (ice-9 popen)
             (ice-9 rdelim)
             (ice-9 string-fun)
             (srfi srfi-1)
             (srfi srfi-13))

(define hosts
  '("x86_64-linux-gnu"
    "arm-linux-gnueabihf"
    "aarch64-linux-gnu"
    "riscv64-linux-gnu"
    "powerpc64-linux-gnu"
    "powerpc64le-linux-gnu"
    "x86_64-w64-mingw32"
    "x86_64-apple-darwin"
    "arm64-apple-darwin"))

(define make-vars
  '("build"
    "crosscompiling"
    "host"
    "host_arch"
    "host_os"
    "host_CC"
    "host_CXX"
    "host_AR"
    "host_RANLIB"
    "host_STRIP"
    "host_OBJCOPY"
    "host_OBJDUMP"
    "host_CFLAGS"
    "host_release_CFLAGS"
    "host_debug_CFLAGS"
    "host_CXXFLAGS"
    "host_release_CXXFLAGS"
    "host_debug_CXXFLAGS"
    "host_CPPFLAGS"
    "host_release_CPPFLAGS"
    "host_debug_CPPFLAGS"
    "host_LDFLAGS"
    "host_release_LDFLAGS"
    "host_debug_LDFLAGS"
    "qt_packages_"
    "qrencode_packages_"
    "zmq_packages_"
    "wallet_packages_"
    "usdt_packages_"
    "ipc_packages_"
    "OSX_SDK"
    "linux_cmake_system_name"
    "linux_cmake_system_version"
    "mingw32_cmake_system_name"
    "mingw32_cmake_system_version"
    "darwin_cmake_system_name"
    "darwin_cmake_system_version"
    "freebsd_cmake_system_name"
    "freebsd_cmake_system_version"
    "netbsd_cmake_system_name"
    "netbsd_cmake_system_version"
    "openbsd_cmake_system_name"
    "openbsd_cmake_system_version"))

(define placeholder->make-var
  '(("@depends_crosscompiling@" . "crosscompiling")
    ("@host@" . "host")
    ("@host_arch@" . "host_arch")
    ("@CC@" . "host_CC")
    ("@CXX@" . "host_CXX")
    ("@OSX_SDK@" . "OSX_SDK")
    ("@AR@" . "host_AR")
    ("@RANLIB@" . "host_RANLIB")
    ("@STRIP@" . "host_STRIP")
    ("@OBJCOPY@" . "host_OBJCOPY")
    ("@OBJDUMP@" . "host_OBJDUMP")
    ("@CFLAGS@" . "host_CFLAGS")
    ("@CFLAGS_RELEASE@" . "host_release_CFLAGS")
    ("@CFLAGS_DEBUG@" . "host_debug_CFLAGS")
    ("@CXXFLAGS@" . "host_CXXFLAGS")
    ("@CXXFLAGS_RELEASE@" . "host_release_CXXFLAGS")
    ("@CXXFLAGS_DEBUG@" . "host_debug_CXXFLAGS")
    ("@CPPFLAGS@" . "host_CPPFLAGS")
    ("@CPPFLAGS_RELEASE@" . "host_release_CPPFLAGS")
    ("@CPPFLAGS_DEBUG@" . "host_debug_CPPFLAGS")
    ("@LDFLAGS@" . "host_LDFLAGS")
    ("@LDFLAGS_RELEASE@" . "host_release_LDFLAGS")
    ("@LDFLAGS_DEBUG@" . "host_debug_LDFLAGS")
    ("@qt_packages@" . "qt_packages_")
    ("@qrencode_packages@" . "qrencode_packages_")
    ("@zmq_packages@" . "zmq_packages_")
    ("@wallet_packages@" . "wallet_packages_")
    ("@usdt_packages@" . "usdt_packages_")
    ("@ipc_packages@" . "ipc_packages_")))

(define (assoc-ref/default alist key default)
  (let ((entry (assoc key alist)))
    (if entry (cdr entry) default)))

(define (split-make-line line)
  (let ((separator (string-index line #\=)))
    (cons (substring line 0 separator)
          (substring line (+ separator 1)))))

(define (run-make host gui?)
  (let* ((mode (if gui? "gui" "base"))
         (scratch (mkdtemp!
                   (string-copy
                    (string-append "/tmp/bitcoin-toolchain-" host "-"
                                   mode "-XXXXXX"))))
         (args (append
                '("-s" "-C" "depends")
                (list (string-append "HOST=" host)
                      (string-append "SOURCES_PATH=" scratch "/sources")
                      (string-append "WORK_PATH=" scratch "/work")
                      (string-append "BASE_CACHE=" scratch "/built")
                      (string-append "SDK_PATH=" scratch "/SDKs")
                      "DEBUG="
                      "LTO=")
                (if gui? '() '("NO_QT=1"))
                (map (lambda (var) (string-append "print-" var)) make-vars))))
    (dynamic-wind
      (lambda () #t)
      (lambda ()
        (for-each unsetenv
                  '("CC" "CXX" "AR" "RANLIB" "STRIP" "OBJCOPY" "OBJDUMP"
                    "CFLAGS" "CXXFLAGS" "CPPFLAGS" "LDFLAGS"
                    "DEBUG" "LTO" "NO_BOOST" "NO_QT" "NO_QR" "NO_WALLET"
                    "NO_ZMQ" "NO_USDT" "NO_IPC"))
        (let* ((port (apply open-pipe* OPEN_READ "make" args))
               (lines (let loop ((line (read-line port)) (result '()))
                        (if (eof-object? line)
                            (reverse result)
                            (loop (read-line port)
                                  (cons (split-make-line line) result)))))
               (status (close-pipe port)))
          (unless (zero? status)
            (error "make print failed" host mode status))
          lines))
      (lambda ()
        (delete-file-recursively scratch)))))

(define (before-target-flag command)
  (let ((index (string-contains command " --target=")))
    (if index (substring command 0 index) command)))

(define (tools-from-make make-values)
  (let ((darwin? (string=? (assoc-ref/default make-values "host_os" "")
                           "darwin")))
    `((cc . ,(if darwin?
                 (before-target-flag
                  (assoc-ref/default make-values "host_CC" ""))
                 (assoc-ref/default make-values "host_CC" "")))
      (cxx . ,(if darwin?
                  (before-target-flag
                   (assoc-ref/default make-values "host_CXX" ""))
                  (assoc-ref/default make-values "host_CXX" "")))
      (ar . ,(assoc-ref/default make-values "host_AR" ""))
      (ranlib . ,(assoc-ref/default make-values "host_RANLIB" ""))
      (strip . ,(assoc-ref/default make-values "host_STRIP" ""))
      (objcopy . ,(assoc-ref/default make-values "host_OBJCOPY" ""))
      (objdump . ,(assoc-ref/default make-values "host_OBJDUMP" ""))
      (sdk-prefix . ,(assoc-ref/default make-values "OSX_SDK" "")))))

(define (expected-substitutions make-values)
  (let* ((host-os (assoc-ref/default make-values "host_os" ""))
         (system-name-key (string-append host-os "_cmake_system_name"))
         (system-version-key (string-append host-os "_cmake_system_version")))
    (append
     (map (lambda (entry)
            (cons (car entry)
                  (assoc-ref/default make-values (cdr entry) "")))
          placeholder->make-var)
     `(("host_system_name" .
        ,(assoc-ref/default make-values system-name-key ""))
       ("host_system_version" .
        ,(assoc-ref/default make-values system-version-key ""))))))

(define (normalize-substitution-key key)
  (if (string-prefix? "@" key)
      key
      (string-append "@" key "@")))

(define (compare-host host gui?)
  (let* ((make-values (run-make host gui?))
         (scheme-values
          (depends-toolchain-substitutions
           host
           (assoc-ref/default make-values "build" "")
           gui?
           #:tools (tools-from-make make-values)))
         (expected
          (map (lambda (entry)
                 (cons (normalize-substitution-key (car entry)) (cdr entry)))
               (expected-substitutions make-values)))
         (mismatches
          (filter-map
           (lambda (expected-entry)
             (let* ((key (car expected-entry))
                    (expected-value (cdr expected-entry))
                    (actual-value (assoc-ref/default scheme-values key #f)))
               (and (not (equal? expected-value actual-value))
                    (list key expected-value actual-value))))
           expected)))
    (for-each
     (lambda (mismatch)
       (format #t "~a ~a mismatch ~a~%  make:   ~s~%  scheme: ~s~%"
               host (if gui? "gui" "base")
               (first mismatch) (second mismatch) (third mismatch)))
     mismatches)
    (null? mismatches)))

(define failures
  (append-map
   (lambda (host)
     (filter-map
      (lambda (gui?)
        (and (not (compare-host host gui?))
             (cons host (if gui? "gui" "base"))))
      '(#f #t)))
   hosts))

(unless (null? failures)
  (format #t "toolchain substitution mismatches: ~s~%" failures)
  (exit 1))

(format #t "toolchain substitutions match Make for ~a host/mode pairs~%"
        (* (length hosts) 2))
