;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.
;;;
;;; Run with the pinned Guix:
;;;   . contrib/guix/libexec/prelude.bash
;;;   HOST=x86_64-linux-gnu JOBS=10 time-machine repl -L contrib/guix/modules -- this-file

(use-modules (bitcoin depends packages)
             (guix build syscalls)
             (guix build utils)
             (ice-9 popen)
             (ice-9 rdelim)
             (srfi srfi-1)
             (srfi srfi-13))

(define release-targets
  '("x86_64-linux-gnu"
    "arm-linux-gnueabihf"
    "aarch64-linux-gnu"
    "riscv64-linux-gnu"
    "powerpc64-linux-gnu"
    "x86_64-w64-mingw32"
    "x86_64-apple-darwin"
    "arm64-apple-darwin"))

(define source-vars
  '("version"
    "download_path"
    "download_file"
    "file_name"
    "sha256_hash"
    "qttranslations_file_name"
    "qttranslations_sha256_hash"
    "qttools_file_name"
    "qttools_sha256_hash"
    "top_download_path"
    "top_cmakelists_file_name"
    "top_cmakelists_download_file"
    "top_cmakelists_sha256_hash"
    "top_cmake_download_path"
    "top_cmake_ecmoptionaladdsubdirectory_file_name"
    "top_cmake_ecmoptionaladdsubdirectory_download_file"
    "top_cmake_ecmoptionaladdsubdirectory_sha256_hash"
    "top_cmake_qttoplevelhelpers_file_name"
    "top_cmake_qttoplevelhelpers_download_file"
    "top_cmake_qttoplevelhelpers_sha256_hash"))

(define (assoc-ref/default alist key default)
  (let ((entry (assoc key alist)))
    (if entry (cdr entry) default)))

(define (split-make-line line)
  (let ((separator (string-index line #\=)))
    (cons (substring line 0 separator)
          (substring line (+ separator 1)))))

(define (write-source-makefile file package)
  (call-with-output-file file
    (lambda (port)
      (display "BASEDIR := $(CURDIR)\n" port)
      (display "PATCHES_PATH := $(BASEDIR)/patches\n" port)
      (display "FALLBACK_DOWNLOAD_PATH := https://bitcoincore.org/depends-sources\n" port)
      (when (string=? package "capnp")
        (display "include packages/native_capnp.mk\n" port)
        (for-each
         (lambda (var)
           (format port "native_capnp_~a := $(native_capnp_~a)~%" var var))
         source-vars))
      (format port "include packages/~a.mk~%" package)
      (display "print-%:\n\t@echo '$*=$($*)'\n" port))))

(define (run-make package host)
  (let* ((scratch (mkdtemp! (string-copy "/tmp/bitcoin-sources.XXXXXX")))
         (makefile (string-append scratch "/sources.mk"))
         (targets (map (lambda (var)
                         (string-append "print-" package "_" var))
                       source-vars))
         (args (append
                '("-s" "-C" "depends")
                (list "-f" makefile
                      (string-append "HOST=" host)
                      (string-append "SOURCES_PATH=" scratch "/sources")
                      (string-append "WORK_PATH=" scratch "/work")
                      (string-append "BASE_CACHE=" scratch "/built")
                      (string-append "SDK_PATH=" scratch "/SDKs")
                      "DEBUG="
                      "LTO=")
                targets)))
    (dynamic-wind
      (lambda () (write-source-makefile makefile package))
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
            (error "make metadata print failed" package host status))
          lines))
      (lambda () (delete-file-recursively scratch)))))

(define (package-value make-values package var)
  (assoc-ref/default make-values (string-append package "_" var) ""))

(define (join-url path file)
  (string-append path "/" file))

(define (standard-source make-values package)
  (let* ((file-name (package-value make-values package "file_name"))
         (download-file (or (and=> (package-value make-values package "download_file")
                                   (lambda (value)
                                     (and (not (string-null? value)) value)))
                            file-name)))
    (list file-name
          (join-url (package-value make-values package "download_path")
                    download-file)
          (package-value make-values package "sha256_hash"))))

(define (qt-sources make-values package)
  (let ((version (package-value make-values package "version")))
    (list
     (standard-source make-values package)
     (list (package-value make-values package "qttranslations_file_name")
           (join-url (package-value make-values package "download_path")
                     (package-value make-values package "qttranslations_file_name"))
           (package-value make-values package "qttranslations_sha256_hash"))
     (list (package-value make-values package "qttools_file_name")
           (join-url (package-value make-values package "download_path")
                     (package-value make-values package "qttools_file_name"))
           (package-value make-values package "qttools_sha256_hash"))
     (list (string-append (package-value make-values package
                                         "top_cmakelists_file_name")
                          "-" version)
           (join-url (package-value make-values package "top_download_path")
                     (package-value make-values package
                                    "top_cmakelists_download_file"))
           (package-value make-values package "top_cmakelists_sha256_hash"))
     (list (string-append (package-value make-values package
                                         "top_cmake_ecmoptionaladdsubdirectory_file_name")
                          "-" version)
           (join-url (package-value make-values package "top_cmake_download_path")
                     (package-value make-values package
                                    "top_cmake_ecmoptionaladdsubdirectory_download_file"))
           (package-value make-values package
                          "top_cmake_ecmoptionaladdsubdirectory_sha256_hash"))
     (list (string-append (package-value make-values package
                                         "top_cmake_qttoplevelhelpers_file_name")
                          "-" version)
           (join-url (package-value make-values package "top_cmake_download_path")
                     (package-value make-values package
                                    "top_cmake_qttoplevelhelpers_download_file"))
           (package-value make-values package
                          "top_cmake_qttoplevelhelpers_sha256_hash")))))

(define (make-sources package host)
  (let ((make-values (run-make package host)))
    (if (member package '("qt" "native_qt"))
        (qt-sources make-values package)
        (list (standard-source make-values package)))))

(define (local-libmultiprocess? recipe)
  (and (string=? (assoc-ref recipe 'name) "native_libmultiprocess")
       (null? (assoc-ref recipe 'sources))
       (string=? (assoc-ref recipe 'local-source)
                 "src/ipc/libmultiprocess")
       (file-exists? "src/ipc/libmultiprocess/CMakeLists.txt")))

(define (compare-recipe target recipe)
  (let ((name (assoc-ref recipe 'name))
        (sources (assoc-ref recipe 'sources)))
    (cond
     ((string=? name "native_libmultiprocess")
      (if (local-libmultiprocess? recipe)
          '()
          (list (list target name "local-source mismatch" sources))))
     ((null? sources)
      (list (list target name "missing remote sources" sources)))
     (else
      (let ((expected (make-sources name target)))
        (if (equal? sources expected)
            '()
            (list (list target name sources expected))))))))

(define failures
  (append-map
   (lambda (target)
     (append-map (lambda (recipe) (compare-recipe target recipe))
                 (release-recipes target #:gui? #t #:system "x86_64-linux")))
   release-targets))

(for-each
 (lambda (failure)
   (format #t "source metadata mismatch: ~s~%" failure))
 failures)

(unless (null? failures)
  (exit 1))

(format #t "source metadata matches Make for ~a default GUI release targets~%"
        (length release-targets))
