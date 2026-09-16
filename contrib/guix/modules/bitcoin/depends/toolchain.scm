;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(define-module (bitcoin depends toolchain)
  #:use-module (srfi srfi-1)
  #:use-module (srfi srfi-13)
  #:export (depends-toolchain-substitutions))

(define (target-arch target)
  (let ((arch (car (string-split target #\-))))
    (if (string=? arch "arm64") "aarch64" arch)))

(define (target-os target)
  (cond ((string-suffix? "-mingw32" target) "mingw32")
        ((string-contains target "darwin") "darwin")
        ((string-contains target "freebsd") "freebsd")
        ((string-contains target "netbsd") "netbsd")
        ((string-contains target "openbsd") "openbsd")
        ((string-contains target "-linux-") "linux")
        (else (error "unsupported depends target" target))))

(define (alist-ref/default alist key default)
  (let ((entry (assq key alist)))
    (if entry (cdr entry) default)))

(define (build-os build-triplet)
  (cond ((string-contains build-triplet "darwin") "darwin")
        ((string-contains build-triplet "freebsd") "freebsd")
        ((string-contains build-triplet "netbsd") "netbsd")
        ((string-contains build-triplet "openbsd") "openbsd")
        ((string-contains build-triplet "linux") "linux")
        (else build-triplet)))

(define (cross-compiling? target build-triplet)
  (not (string=? target build-triplet)))

(define (default-tool target build-triplet name)
  (let* ((cross? (cross-compiling? target build-triplet))
         (prefix (if cross? (string-append target "-") "")))
    (string-append prefix name)))

(define (tool tools key default)
  (alist-ref/default tools key default))

(define (gcc-tool target build-triplet tools key program)
  (tool tools key (default-tool target build-triplet program)))

(define (mingw-tool target tools key program)
  (tool tools key (default-tool target "" program)))

(define (darwin-sdk-prefix tools)
  (tool tools 'sdk-prefix
        "/bitcoin/depends/SDKs/Xcode-26.1.1-17B100-extracted-SDK-with-libcxx-headers"))

(define (darwin-tools target build-triplet tools)
  (let* ((sdk (darwin-sdk-prefix tools))
         (cc (tool tools 'cc "clang"))
         (cxx (tool tools 'cxx "clang++")))
    `((cc . ,(string-append
              cc " --target=" target
              " -isysroot" sdk " -nostdlibinc"
              " -iwithsysroot/usr/include"
              " -iframeworkwithsysroot/System/Library/Frameworks"))
      (cxx . ,(string-append
               cxx " --target=" target
               " -isysroot" sdk " -nostdlibinc"
               " -iwithsysroot/usr/include/c++/v1"
               " -iwithsysroot/usr/include"
               " -iframeworkwithsysroot/System/Library/Frameworks"))
      (ar . ,(tool tools 'ar "llvm-ar"))
      (ranlib . ,(tool tools 'ranlib "llvm-ranlib"))
      (strip . ,(tool tools 'strip "llvm-strip"))
      (objcopy . ,(tool tools 'objcopy "llvm-objcopy"))
      (objdump . ,(tool tools 'objdump "llvm-objdump")))))

(define (host-tools target build-triplet target-os tools)
  (cond ((string=? target-os "darwin")
         (darwin-tools target build-triplet tools))
        ((string=? target-os "mingw32")
         `((cc . ,(mingw-tool target tools 'cc "gcc"))
           (cxx . ,(mingw-tool target tools 'cxx "g++"))
           (ar . ,(gcc-tool target build-triplet tools 'ar "ar"))
           (ranlib . ,(gcc-tool target build-triplet tools 'ranlib "ranlib"))
           (strip . ,(gcc-tool target build-triplet tools 'strip "strip"))
           (objcopy . ,(gcc-tool target build-triplet tools 'objcopy "objcopy"))
           (objdump . ,(gcc-tool target build-triplet tools 'objdump "objdump"))))
        (else
         `((cc . ,(gcc-tool target build-triplet tools 'cc "gcc"))
           (cxx . ,(gcc-tool target build-triplet tools 'cxx "g++"))
           (ar . ,(gcc-tool target build-triplet tools 'ar "ar"))
           (ranlib . ,(gcc-tool target build-triplet tools 'ranlib "ranlib"))
           (strip . ,(gcc-tool target build-triplet tools 'strip "strip"))
           (objcopy . ,(gcc-tool target build-triplet tools 'objcopy "objcopy"))
           (objdump . ,(gcc-tool target build-triplet tools 'objdump "objdump"))))))

(define (host-system-name target-os)
  (cond ((string=? target-os "darwin") "Darwin")
        ((string=? target-os "freebsd") "FreeBSD")
        ((string=? target-os "netbsd") "NetBSD")
        ((string=? target-os "openbsd") "OpenBSD")
        ((string=? target-os "linux") "Linux")
        ((string=? target-os "mingw32") "Windows")
        (else "")))

(define (host-system-version target-os)
  (cond ((string=? target-os "darwin") "20.1")
        ((string=? target-os "linux") "3.17.0")
        ((string=? target-os "mingw32") "10.0")
        (else "")))

(define (host-flags target build-triplet target-os flags)
  (let* ((darwin? (string=? target-os "darwin"))
         (build-darwin? (string=? (build-os build-triplet) "darwin"))
         (darwin-ld-extra (if build-darwin? "" " -Wl,-no_adhoc_codesign -fuse-ld=lld"))
         (darwin-compiler-extra (if build-darwin? "" " -mlinker-version=711"))
         (cflags (if darwin?
                     (string-append "-mmacos-version-min=14.0" darwin-compiler-extra)
                     ""))
         (cxxflags (if darwin?
                       (string-append "-mmacos-version-min=14.0"
                                      " -Xclang -fno-cxx-modules"
                                      darwin-compiler-extra)
                       ""))
         (ldflags (if darwin?
                      (string-append "-Wl,-platform_version,macos,14.0,14.0"
                                     darwin-ld-extra)
                      "")))
    `((cflags . ,(alist-ref/default flags 'cflags cflags))
      (cflags-release . ,(alist-ref/default flags 'cflags-release "-O2"))
      (cflags-debug . ,(alist-ref/default flags 'cflags-debug ""))
      (cxxflags . ,(alist-ref/default flags 'cxxflags cxxflags))
      (cxxflags-release . ,(alist-ref/default flags 'cxxflags-release "-O2"))
      (cxxflags-debug . ,(alist-ref/default flags 'cxxflags-debug ""))
      (cppflags . ,(alist-ref/default flags 'cppflags ""))
      (cppflags-release . ,(alist-ref/default flags 'cppflags-release ""))
      (cppflags-debug . ,(alist-ref/default flags 'cppflags-debug ""))
      (ldflags . ,(alist-ref/default flags 'ldflags ldflags))
      (ldflags-release . ,(alist-ref/default flags 'ldflags-release ""))
      (ldflags-debug . ,(alist-ref/default flags 'ldflags-debug "")))))

(define linux-qt-packages
  '("qt" "expat" "libxcb" "xcb_proto" "libXau" "xproto" "freetype"
    "fontconfig" "libxkbcommon" "libxcb_util" "libxcb_util_cursor"
    "libxcb_util_render" "libxcb_util_keysyms" "libxcb_util_image"
    "libxcb_util_wm"))

(define (qt-packages target-os gui?)
  (if gui?
      (string-append
       " "
       (string-join
        (cond ((member target-os '("linux" "freebsd" "openbsd"))
               linux-qt-packages)
              ((member target-os '("darwin" "mingw32"))
               '("qt"))
              (else '()))
        " ")
       "  "
       (string-join (qrencode-packages target-os) " "))
      ""))

(define (qrencode-packages target-os)
  (if (member target-os '("linux" "freebsd" "openbsd" "darwin" "mingw32"))
      '("qrencode")
      '()))

(define (package-feature target-os gui?)
  `((qt-packages . ,(qt-packages target-os gui?))
    ;; Current base scripts pass NO_QT=1, not NO_QR=1, so this remains set.
    (qrencode-packages . ,(string-join (qrencode-packages target-os) " "))
    (zmq-packages . "zeromq")
    (wallet-packages . "sqlite")
    (usdt-packages . ,(if (string=? target-os "linux") "systemtap" ""))
    (ipc-packages . ,(if (string=? target-os "mingw32") "" "capnp"))))

(define (substitution key value)
  (cons (string-append "@" key "@") value))

(define* (depends-toolchain-substitutions target build-triplet gui?
                                          #:key
                                          (tools '())
                                          (flags '()))
  "Return depends/toolchain.cmake.in placeholder substitutions for TARGET.

TOOLS may override final tool paths with symbol keys cc, cxx, ar, ranlib,
strip, objcopy, objdump, and sdk-prefix.  FLAGS may override cflags,
cflags-release, cflags-debug, cxxflags, cxxflags-release, cxxflags-debug,
cppflags, cppflags-release, cppflags-debug, ldflags, ldflags-release, and
ldflags-debug."
  (unless (string? target)
    (error "target must be a string" target))
  (unless (string? build-triplet)
    (error "build triplet must be a string" build-triplet))
  (let* ((os (target-os target))
         (tool-values (host-tools target build-triplet os tools))
         (flag-values (host-flags target build-triplet os flags))
         (feature-values (package-feature os gui?))
         (value (lambda (key alist) (alist-ref/default alist key ""))))
    (list
     (substitution "depends_crosscompiling"
                   (if (cross-compiling? target build-triplet) "TRUE" "FALSE"))
     (substitution "host" target)
     (substitution "host_system_name" (host-system-name os))
     (substitution "host_system_version" (host-system-version os))
     (substitution "host_arch" (target-arch target))
     (substitution "CC" (value 'cc tool-values))
     (substitution "CXX" (value 'cxx tool-values))
     (substitution "OSX_SDK" (if (string=? os "darwin")
                                  (darwin-sdk-prefix tools)
                                  ""))
     (substitution "AR" (value 'ar tool-values))
     (substitution "RANLIB" (value 'ranlib tool-values))
     (substitution "STRIP" (value 'strip tool-values))
     (substitution "OBJCOPY" (value 'objcopy tool-values))
     (substitution "OBJDUMP" (value 'objdump tool-values))
     (substitution "CFLAGS" (value 'cflags flag-values))
     (substitution "CFLAGS_RELEASE" (value 'cflags-release flag-values))
     (substitution "CFLAGS_DEBUG" (value 'cflags-debug flag-values))
     (substitution "CXXFLAGS" (value 'cxxflags flag-values))
     (substitution "CXXFLAGS_RELEASE" (value 'cxxflags-release flag-values))
     (substitution "CXXFLAGS_DEBUG" (value 'cxxflags-debug flag-values))
     (substitution "CPPFLAGS" (value 'cppflags flag-values))
     (substitution "CPPFLAGS_RELEASE" (value 'cppflags-release flag-values))
     (substitution "CPPFLAGS_DEBUG" (value 'cppflags-debug flag-values))
     (substitution "LDFLAGS" (value 'ldflags flag-values))
     (substitution "LDFLAGS_RELEASE" (value 'ldflags-release flag-values))
     (substitution "LDFLAGS_DEBUG" (value 'ldflags-debug flag-values))
     (substitution "qt_packages" (value 'qt-packages feature-values))
     (substitution "qrencode_packages"
                   (value 'qrencode-packages feature-values))
     (substitution "zmq_packages" (value 'zmq-packages feature-values))
     (substitution "wallet_packages" (value 'wallet-packages feature-values))
     (substitution "usdt_packages" (value 'usdt-packages feature-values))
     (substitution "ipc_packages" (value 'ipc-packages feature-values)))))
