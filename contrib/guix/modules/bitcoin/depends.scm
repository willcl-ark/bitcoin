;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(define-module (bitcoin depends)
  #:use-module (guix packages)
  #:use-module (guix profiles)
  #:use-module (guix gexp)
  #:use-module (guix modules)
  #:use-module (guix download)
  #:use-module (guix git-download)
  #:use-module (guix grafts)
  #:use-module (guix base16)
  #:use-module (guix utils)
  #:use-module ((guix ui) #:select (make-user-module))
  #:use-module (guix build-system trivial)
  #:use-module (srfi srfi-1)
  #:use-module (ice-9 match)
  #:export (depends-package depends-profile))

(define %guix-directory
  (canonicalize-path
   (string-append (dirname (search-path %load-path "bitcoin/depends.scm")) "/../..")))
(define %root (canonicalize-path (string-append %guix-directory "/../..")))

(define (quote-shell text)
  (string-append "'" (string-join (string-split text #\') "'\\''") "'"))

(define (assignment name value)
  (string-append name "=" (quote-shell value) "\n"))

(define (recipe-field recipe field default)
  (let ((entry (assq field recipe)))
    (if entry (cdr entry) default)))

(define (depends-profile target)
  ;; These are the same manifests and profile options as `time-machine shell'.
  (let ((previous (getenv "HOST"))
        (graft? (%graft?)))
    (with-parameters ((%graft? graft?))
    (dynamic-wind
      (lambda () (setenv "HOST" target))
      (lambda ()
        (profile
          (content
           (concatenate-manifests
            (map (lambda (file)
                   (save-module-excursion
                    (lambda ()
                      (set-current-module (make-user-module '((guix profiles) (gnu))))
                      (primitive-load (string-append %guix-directory "/" file)))))
                 '("manifest_build.scm" "manifest_gui.scm"))))
          (allow-collisions? #t)))
      (lambda ()
        (if previous (setenv "HOST" previous) (unsetenv "HOST")))))))

(define (recipe-source source)
  (match source
    ((name uri hash)
     (origin
       (method url-fetch)
       (uri (list uri (string-append "https://bitcoincore.org/depends-sources/" name)))
       (file-name name)
       (sha256 (base16-string->bytevector hash))))))

(define* (depends-package recipe target build-triplet dependencies #:key sdk)
  "Build RECIPE with already built DEPENDENCIES at the release depends prefix."
  (define (field key default) (recipe-field recipe key default))
  (let* ((name (field 'name #f))
         (version (field 'version #f))
         (sources (field 'sources '()))
         (patches (field 'patches '()))
         (local (field 'local-source #f))
         ;; A trivial builder normally lowers its inputs without grafts. The
         ;; release shell uses the caller's graft policy, which we must retain.
         (environment (depends-profile target))
         (script
          (mixed-text-file
           (string-append name "-build.sh")
           "set -eo pipefail\n"
           "export GUIX_ENVIRONMENT=" environment "\n"
           "toolchain_script=" (local-file (string-append %guix-directory "/libexec/toolchain.sh")) "\n"
           (assignment "target" target)
           (assignment "build_triplet" build-triplet)
           (assignment "native" (if (field 'native? #f) "1" "0"))
           (assignment "autoconf_with_pic" (if (field 'autoconf-with-pic? #t) "1" "0"))
           "source " (local-file (string-append %guix-directory "/libexec/depends-build.sh")) "\n"
           (assignment "source_dir" (string-append "/bitcoin/depends/work/build/" target "/" name "/" version))
           "sources_dir=/bitcoin/depends/sources\n"
           "depends_dir=/bitcoin/depends\n"
           "stage_dir=/bitcoin/depends/work/staging\n"
           "staged_prefix=$stage_dir$prefix\n"
           "build_dir=$source_dir/" (quote-shell (field 'build-subdir ".")) "\n"
           "mkdir -p \"$source_dir\" \"$sources_dir\" \"$staged_prefix\" \"$host_prefix/lib\"\n"
           ;; References in a mixed-text-file are lowered as declared inputs.
           "source "
           (apply mixed-text-file
                  (string-append name "-sources.sh")
                  (append
                   (list "cp " (local-file (string-append %root "/depends/config.guess"))
                         " \"$depends_dir/config.guess\"\ncp "
                         (local-file (string-append %root "/depends/config.sub"))
                         " \"$depends_dir/config.sub\"\n")
                   (if sdk
                       (list "mkdir -p /bitcoin/depends/SDKs\nln -s " sdk
                             " /bitcoin/depends/SDKs/Xcode-26.1.1-17B100-extracted-SDK-with-libcxx-headers\n")
                       '())
                   (append-map
                    (lambda (source)
                      (list "cp " (recipe-source source) " \"$sources_dir/" (car source) "\"\n"))
                    sources)
                   (if local
                       (list "cp -a --no-preserve=ownership " (local-file (string-append %root "/" local)
                                                   #:recursive? #t
                                                   #:select? (git-predicate %root))
                             "/. \"$source_dir/\"\nchmod -R u+w \"$source_dir\"\n")
                       '())
                   '("chmod -R u+w \"$host_prefix\"\ncd \"$source_dir\"\n")
                   (if local
                       '()
                       (list (field 'extract
                                    (string-append "tar --no-same-owner --strip-components=1 -xf \"$sources_dir/"
                                                   (caar sources) "\"")) "\n"))
                   (append-map
                    (lambda (patch)
                      (match patch
                        ((file strip)
                         (list "patch -p" (number->string strip) " < "
                               (local-file (string-append %root "/depends/patches/" file)) "\n"))))
                    patches)))
           "\n"
           (field 'preprocess "true") "\n"
           "mkdir -p \"$build_dir\"\ncd \"$build_dir\"\n"
           "cflags+=\" " (field 'extra-cflags "") "\"\n"
           "cxxflags+=\" " (field 'extra-cxxflags "") "\"\n"
           "cppflags+=\" " (field 'extra-cppflags "") "\"\n"
           "ldflags+=\" " (field 'extra-ldflags "") "\"\n"
           "(\n" (field 'configure "true") "\n)\n"
           "(\n" (field 'build "true") "\n)\n"
           "(\n" (field 'install "true") "\n)\n"
           "cd \"$staged_prefix\"\n"
           (field 'postprocess "true") "\n"
           "find \"$stage_dir$host_prefix\" -print0 | xargs -0r touch -h -m -t 200001011200\n"
           "cp -a \"$stage_dir$host_prefix/.\" \"$out/\"\n")))
    ;; The scripts containing file-like inputs are sourced below, rather than
    ;; being expanded into host paths during evaluation.
    (package
      (name (string-append "bitcoin-depends-" name "-" target))
      (version version)
      (source #f)
      (build-system trivial-build-system)
      (arguments
       (list
        #:modules (source-module-closure '((gnu build linux-container)
                                          (bitcoin depends build))
                                         #:select? (lambda (name)
                                                     (or (guix-module-name? name)
                                                         (eq? (car name) 'bitcoin))))
        #:builder
        #~(begin
            (use-modules (bitcoin depends build)
                         (gnu build linux-container) (gnu system file-systems)
                         (guix build utils))
            (let ((work (string-append (getcwd) "/bitcoin"))
                  (cores (number->string (parallel-job-count))))
              (mkdir-p work)
              (mkdir #$output)
              (let ((status
                     (call-with-container
                      (list (file-system (device "/gnu/store") (mount-point "/gnu/store")
                                         (type "none") (flags '(bind-mount)))
                            (file-system (device work) (mount-point "/bitcoin")
                                         (type "none") (flags '(bind-mount)))
                            ;; The daemon bind-mounts outputs separately. A
                            ;; non-recursive store bind would hide that mount.
                            (file-system (device #$output) (mount-point "/output")
                                         (type "none") (flags '(bind-mount))))
                      (lambda ()
                        (setenv "jobs" cores)
                        (setenv "out" "/output")
                        (setenv "PATH" (string-append #$environment "/bin"))
                        (setenv "SOURCE_DATE_EPOCH" "1")
                        (mkdir-p "/tmp")
                        (mkdir-p "/bin")
                        (symlink (string-append #$environment "/bin/bash") "/bin/sh")
                        (mkdir-p "/usr/bin")
                        (symlink (string-append #$environment "/bin/env") "/usr/bin/env")
                        (chdir "/bitcoin")
                        (for-each
                         (lambda (input)
                           (merge-depends input (string-append "/bitcoin/depends/" #$target)))
                         (list #$@dependencies))
                        (invoke (string-append #$environment "/bin/bash") #$script))
                      #:namespaces '(user mnt pid)
                      #:writable-root? #t)))
                (unless (zero? status) (error "depends build failed" status)))))))
      (home-page "https://bitcoincore.org")
      (synopsis (string-append "Bitcoin Core depends package " name))
      (description "A dependency built with Bitcoin Core's release toolchain and recipe.")
      (license #f))))
