;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.
;;;
;;; Run with the pinned Guix:
;;;   . contrib/guix/libexec/prelude.bash
;;;   HOST=x86_64-linux-gnu JOBS=10 time-machine repl -L contrib/guix/modules -- this-file

(use-modules (bitcoin depends packages)
             (bitcoin depends qt)
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

(define linux-base-packages
  '("boost" "sqlite" "systemtap" "zeromq" "capnp"
    "native_libmultiprocess" "native_capnp"))

(define linux-gui-packages
  (append linux-base-packages
          '("qt" "expat" "libxcb" "xcb_proto" "libXau" "xproto"
            "freetype" "fontconfig" "libxkbcommon" "libxcb_util"
            "libxcb_util_cursor" "libxcb_util_render" "libxcb_util_keysyms"
            "libxcb_util_image" "libxcb_util_wm" "qrencode" "native_qt")))

(define windows-base-packages
  '("boost" "sqlite" "zeromq"))

(define windows-gui-packages
  '("boost" "qt" "qrencode" "sqlite" "zeromq" "native_qt"))

(define darwin-base-packages
  '("boost" "sqlite" "zeromq" "capnp"
    "native_libmultiprocess" "native_capnp"))

(define darwin-gui-packages
  (append darwin-base-packages '("qt" "qrencode" "native_qt")))

(define qt-source-names
  '("qtbase-everywhere-opensource-src-6.8.4.tar.xz"
    "qttranslations-everywhere-opensource-src-6.8.4.tar.xz"
    "qttools-everywhere-opensource-src-6.8.4.tar.xz"
    "CMakeLists.txt-6.8.4"
    "ECMOptionalAddSubdirectory.cmake-6.8.4"
    "QtTopLevelHelpers.cmake-6.8.4"))

(define native-qt-patches
  '("qt/dont_hardcode_pwd.patch"
    "qt/qtbase_skip_tools.patch"
    "qt/rcc_hardcode_timestamp.patch"
    "qt/qttools_skip_dependencies.patch"
    "qt/fix-macos26-qyield.patch"
    "qt/fix_missed_headers.patch"))

(define cross-qt-patches
  '("qt/cocoa_compat.patch"
    "qt/dont_hardcode_pwd.patch"
    "qt/qtbase_avoid_qmain.patch"
    "qt/qtbase_plugins_cocoa.patch"
    "qt/qtbase_skip_tools.patch"
    "qt/rcc_hardcode_timestamp.patch"
    "qt/static_fixes.patch"
    "qt/fix-gcc16-qcompare.patch"
    "qt/fix-gcc16-sfinae-qregularexpression.patch"
    "qt/fix-gcc16-sfinae-qchar.patch"
    "qt/fix-gcc16-sfinae-qbitarray.patch"
    "qt/fix-gcc16-sfinae-qanystringview.patch"
    "qt/fix-macos26-qyield.patch"
    "qt/fix-qbytearray-include.patch"
    "qt/fix_openbsd_network_kernel.patch"
    "qt/fix_openbsd_plugin_qelfparser.patch"
    "qt/fix_missed_headers.patch"))

(define native-target-qt-patches
  (append cross-qt-patches '("qt/qttools_skip_dependencies.patch")))

(define linux-qt-dependencies
  '("freetype" "fontconfig" "libxcb" "libxkbcommon" "libxcb_util"
    "libxcb_util_cursor" "libxcb_util_render" "libxcb_util_keysyms"
    "libxcb_util_image" "libxcb_util_wm"))

(define (assert condition message . irritants)
  (unless condition (apply error message irritants)))

(define (recipe-names target gui?)
  (map (lambda (recipe) (assoc-ref recipe 'name))
       (release-recipes target #:gui? gui? #:system "x86_64-linux")))

(define (assert-same-set actual expected context)
  (let ((actual-sorted (sort actual string<?))
        (expected-sorted (sort expected string<?)))
    (assert (equal? actual-sorted expected-sorted)
            "unexpected package set" context actual-sorted expected-sorted)))

(define (expected-packages target gui?)
  (cond
   ((string-suffix? "-mingw32" target)
    (if gui? windows-gui-packages windows-base-packages))
   ((string-contains target "darwin")
    (if gui? darwin-gui-packages darwin-base-packages))
   (else
    (if gui? linux-gui-packages linux-base-packages))))

(define (recipe-by-name recipes name)
  (or (find (lambda (recipe) (string=? name (assoc-ref recipe 'name)))
            recipes)
      (error "missing recipe" name)))

(define (patch-names recipe)
  (map car (assoc-ref recipe 'patches)))

(define (source-names recipe)
  (map car (assoc-ref recipe 'sources)))

(define (hex-digit? character)
  (or (char-numeric? character)
      (char<=? #\a character #\f)
      (char<=? #\A character #\F)))

(define (source-hash? source)
  (and (= (string-length (third source)) 64)
       (string-every hex-digit? (third source))))

(define (assert-qt-source-shape recipe context)
  (let ((sources (assoc-ref recipe 'sources)))
    (assert (equal? (source-names recipe) qt-source-names)
            "unexpected Qt source names" context (source-names recipe))
    (assert (every (lambda (source)
                     (string-contains (second source) "download.qt.io"))
                   (take sources 3))
            "Qt submodule source URL changed" context sources)
    (assert (every (lambda (source)
                     (string-contains (second source)
                                      "raw.githubusercontent.com/qt/qt5"))
                   (drop sources 3))
            "Qt top-level source URL changed" context sources)
    (assert (every source-hash? sources)
            "Qt source hash is not a SHA256 hex string" context sources)))

(define (assert-qt-extract recipe extracts-tools? context)
  (let ((extract (assoc-ref recipe 'extract)))
    (assert (string-contains extract "qtbase-everywhere-opensource-src-6.8.4.tar.xz")
            "Qt extract does not unpack qtbase" context extract)
    (assert (string-contains extract "CMakeLists.txt-6.8.4")
            "Qt extract does not copy top-level CMakeLists.txt" context extract)
    (assert (string-contains extract "ECMOptionalAddSubdirectory.cmake-6.8.4")
            "Qt extract does not copy ECMOptionalAddSubdirectory.cmake" context extract)
    (assert (string-contains extract "QtTopLevelHelpers.cmake-6.8.4")
            "Qt extract does not copy QtTopLevelHelpers.cmake" context extract)
    (assert (eq? (if (string-contains extract "qttranslations-everywhere-opensource-src-6.8.4.tar.xz") #t #f)
                 extracts-tools?)
            "Qt extract qttranslations condition mismatch" context extract)
    (assert (eq? (if (string-contains extract "qttools-everywhere-opensource-src-6.8.4.tar.xz") #t #f)
                 extracts-tools?)
            "Qt extract qttools condition mismatch" context extract)))

(define (assert-qt-recipe recipe expected-dependencies expected-patches extracts-tools? context)
  (assert (string=? (assoc-ref recipe 'version) "6.8.4")
          "unexpected Qt version" context (assoc-ref recipe 'version))
  (assert-qt-source-shape recipe context)
  (assert (equal? (assoc-ref recipe 'dependencies) expected-dependencies)
          "unexpected Qt dependencies" context (assoc-ref recipe 'dependencies))
  (assert (equal? (patch-names recipe) expected-patches)
          "unexpected Qt patch order" context (patch-names recipe))
  (assert-qt-extract recipe extracts-tools? context))

(for-each
 (lambda (target)
   (for-each
    (lambda (gui?)
      (assert-same-set (recipe-names target gui?)
                       (expected-packages target gui?)
                       (list target (if gui? 'gui 'base))))
    '(#f #t)))
 release-targets)

(let ((native-linux-qt
       (recipe-by-name (qt-recipes "x86_64-linux-gnu" "x86_64-linux-gnu") "qt"))
      (cross-linux-recipes
       (qt-recipes "arm-linux-gnueabihf" "x86_64-pc-linux-gnu"))
      (cross-windows-recipes
       (qt-recipes "x86_64-w64-mingw32" "x86_64-pc-linux-gnu"))
      (cross-darwin-recipes
       (qt-recipes "arm64-apple-darwin" "x86_64-pc-linux-gnu")))
  (assert (string-contains
           (assoc-ref (recipe-by-name cross-darwin-recipes "qt") 'configure)
           "-DCMAKE_SYSTEM_PROCESSOR=aarch64")
          "Darwin Qt must use the canonical aarch64 processor name")
  (assert (equal? (map (lambda (recipe) (assoc-ref recipe 'name))
                       cross-windows-recipes)
                  '("native_qt" "qt"))
          "cross Qt recipe order changed")
  (assert-qt-recipe native-linux-qt
                    linux-qt-dependencies
                    native-target-qt-patches
                    #t
                    'native-linux-qt)
  (assert-qt-recipe (recipe-by-name cross-linux-recipes "qt")
                    (cons "native_qt" linux-qt-dependencies)
                    cross-qt-patches
                    #f
                    'cross-linux-qt)
  (assert-qt-recipe (recipe-by-name cross-windows-recipes "native_qt")
                    '()
                    native-qt-patches
                    #t
                    'native-qt)
  (assert-qt-recipe (recipe-by-name cross-windows-recipes "qt")
                    '("native_qt")
                    cross-qt-patches
                    #f
                    'cross-windows-qt)
  (assert-qt-recipe (recipe-by-name cross-darwin-recipes "qt")
                    '("native_qt")
                    cross-qt-patches
                    #f
                    'cross-darwin-qt))

(format #t "recipe graph and Qt metadata checks passed for ~a release targets~%"
        (length release-targets))
