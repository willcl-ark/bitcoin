;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(define-module (bitcoin depends qt)
  #:use-module (srfi srfi-13)
  #:export (qt-recipes))

(define %qt-version "6.8.4")
(define %qt-suffix
  (string-append "everywhere-opensource-src-" %qt-version ".tar.xz"))
(define %qt-download-path
  (string-append "https://download.qt.io/archive/qt/6.8/"
                 %qt-version "/submodules"))
(define %qt-top-download-path
  (string-append "https://raw.githubusercontent.com/qt/qt5/refs/tags/v"
                 %qt-version "-lts-lgpl"))
(define %qt-top-cmake-download-path
  (string-append %qt-top-download-path "/cmake"))

(define (source file-name uri sha256)
  (list file-name uri sha256))

(define (submodule-source module sha256)
  (let ((file-name (string-append module "-" %qt-suffix)))
    (source file-name
            (string-append %qt-download-path "/" file-name)
            sha256)))

(define (top-source file-name download-path sha256)
  (source (string-append file-name "-" %qt-version)
          (string-append download-path "/" file-name)
          sha256))

(define %qtbase-source
  (submodule-source "qtbase"
                    "532dfbf3fa3cbc68fa37441ea9e81c5009da044eaecda78ffaeafd8bd125532f"))
(define %qttranslations-source
  (submodule-source "qttranslations"
                    "33b1fd1d75598cbf54da12263957f18292c9fb01e42fcc3ab9bd2f8ac79763b7"))
(define %qttools-source
  (submodule-source "qttools"
                    "c6030ea66d7be1ca7e3b40578beb35b0f4ff4014277d8e051d3219759f6ab399"))
(define %qt-top-cmakelists-source
  (top-source "CMakeLists.txt" %qt-top-download-path
              "54e9a4e554da37792446dda4f52bc308407b01a34bcc3afbad58e4e0f71fac9b"))
(define %qt-ecm-source
  (top-source "ECMOptionalAddSubdirectory.cmake" %qt-top-cmake-download-path
              "97ee8bbfcb0a4bdcc6c1af77e467a1da0c5b386c42be2aa97d840247af5f6f70"))
(define %qt-top-helpers-source
  (top-source "QtTopLevelHelpers.cmake" %qt-top-cmake-download-path
              "e11581b2101a6836ca991817d43d49e1f6016e4e672bbc3523eaa8b3eb3b64c2"))

(define %qt-sources
  (list %qtbase-source
        %qttranslations-source
        %qttools-source
        %qt-top-cmakelists-source
        %qt-ecm-source
        %qt-top-helpers-source))

(define (patch file-name)
  (list (string-append "qt/" file-name) 1))

(define %native-qt-patches
  (map patch
       (list "dont_hardcode_pwd.patch"
             "qtbase_skip_tools.patch"
             "rcc_hardcode_timestamp.patch"
             "qttools_skip_dependencies.patch"
             "fix-macos26-qyield.patch"
             "fix_missed_headers.patch")))

(define %qt-cross-patches
  (map patch
       (list "cocoa_compat.patch"
             "dont_hardcode_pwd.patch"
             "qtbase_avoid_qmain.patch"
             "qtbase_plugins_cocoa.patch"
             "qtbase_skip_tools.patch"
             "rcc_hardcode_timestamp.patch"
             "static_fixes.patch"
             "fix-gcc16-qcompare.patch"
             "fix-gcc16-sfinae-qregularexpression.patch"
             "fix-gcc16-sfinae-qchar.patch"
             "fix-gcc16-sfinae-qbitarray.patch"
             "fix-gcc16-sfinae-qanystringview.patch"
             "fix-macos26-qyield.patch"
             "fix-qbytearray-include.patch"
             "fix_openbsd_network_kernel.patch"
             "fix_openbsd_plugin_qelfparser.patch"
             "fix_missed_headers.patch")))

(define %qt-native-target-patches
  (append %qt-cross-patches
          (list (patch "qttools_skip_dependencies.patch"))))

(define %linux-qt-dependencies
  (list "freetype"
        "fontconfig"
        "libxcb"
        "libxkbcommon"
        "libxcb_util"
        "libxcb_util_cursor"
        "libxcb_util_render"
        "libxcb_util_keysyms"
        "libxcb_util_image"
        "libxcb_util_wm"))

(define (target-os target)
  (cond
   ((string-suffix? "-mingw32" target) 'mingw32)
   ((string-contains target "darwin") 'darwin)
   ((string-contains target "linux") 'linux)
   (else (error "unsupported Qt release target" target))))

(define (target-arch target)
  (let ((arch (car (string-split target #\-))))
    (if (string=? arch "arm64") "aarch64" arch)))

(define (cmake-system-name os)
  (case os
    ((linux) "Linux")
    ((mingw32) "Windows")
    ((darwin) "Darwin")
    (else (error "unsupported Qt target OS" os))))

(define (cmake-system-version os)
  (case os
    ((linux) "3.17.0")
    ((mingw32) "10.0")
    ((darwin) "20.1")
    (else (error "unsupported Qt target OS" os))))

(define (shell-arguments arguments)
  (string-join arguments " \\\n    "))

(define (qt-config-options os cross?)
  (append
   (list "-no-egl"
         "-no-eglfs"
         "-no-evdev"
         "-no-gif"
         "-no-glib"
         "-no-icu"
         "-no-ico"
         "-no-kms"
         "-no-linuxfb"
         "-no-libjpeg"
         "-no-libproxy"
         "-no-libudev"
         "-no-mtdev"
         "-no-opengl"
         "-no-openssl"
         "-no-openvg"
         "-no-reduce-relocations"
         "-no-schannel"
         "-no-sctp"
         "-no-securetransport"
         "-no-system-proxies"
         "-no-use-gold-linker"
         "-no-zstd"
         "-nomake examples"
         "-nomake tests"
         "-prefix \"${host_prefix}\""
         "-qt-doubleconversion"
         "-qt-harfbuzz")
   (if cross? (list "-qt-host-path \"${native_prefix}\"") '())
   (list "-qt-libpng"
         "-qt-pcre"
         "-qt-zlib"
         "-static"
         "-no-feature-backtrace"
         "-no-feature-colordialog"
         "-no-feature-concurrent"
         "-no-feature-dial"
         "-no-feature-gssapi"
         "-no-feature-http"
         "-no-feature-image_heuristic_mask"
         "-no-feature-keysequenceedit"
         "-no-feature-lcdnumber"
         "-no-feature-libresolv"
         "-no-feature-libstdcpp_assertions"
         "-no-feature-networkdiskcache"
         "-no-feature-networkproxy"
         "-no-feature-printsupport"
         "-no-feature-sessionmanager"
         "-no-feature-socks5"
         "-no-feature-sql"
         "-no-feature-textmarkdownreader"
         "-no-feature-textmarkdownwriter"
         "-no-feature-textodfwriter"
         "-no-feature-topleveldomain"
         "-no-feature-udpsocket"
         "-no-feature-undocommand"
         "-no-feature-undogroup"
         "-no-feature-undostack"
         "-no-feature-undoview"
         "-no-feature-vnc"
         "-no-feature-vulkan"
         "-no-feature-androiddeployqt"
         "-no-feature-macdeployqt"
         "-no-feature-qmake"
         "-no-feature-windeployqt")
   (if cross?
       '()
       (list "-feature-linguist"
             "-no-feature-assistant"
             "-no-feature-clang"
             "-no-feature-clangcpp"
             "-no-feature-designer"
             "-no-feature-pixeltool"
             "-no-feature-qdoc"
             "-no-feature-qtattributionsscanner"
             "-no-feature-qtdiag"
             "-no-feature-qtplugininfo"))
   (list "-release")
   (case os
     ((darwin)
      (list "-no-dbus"
            "-no-feature-printsupport"
            "-no-feature-freetype"
            "-no-pkg-config"))
     ((linux)
      (list "-fontconfig"
            "-no-feature-process"
            "-no-feature-xlib"
            "-no-xcb-xlib"
            "-pkg-config"
            "-system-freetype"
            "-xcb"
            "${LTO:+-ltcg}"))
     ((mingw32)
      (list "-no-dbus"
            "-no-feature-freetype"
            "-no-pkg-config"))
     (else (error "unsupported Qt target OS" os)))))

(define (qt-cmake-options os cross? arch)
  (append
   (list "-DCMAKE_PREFIX_PATH=\"${host_prefix}\""
         "-DQT_FEATURE_cxx20=ON"
         "-DQT_GENERATE_SBOM=OFF"
         "${V:+--log-level=STATUS}"
         "-DQT_USE_DEFAULT_CMAKE_OPTIMIZATION_FLAGS=ON"
         "-DCMAKE_C_FLAGS=\"${CPPFLAGS} -DPNG_RISCV_RVV_OPT=0 ${CFLAGS} -ffile-prefix-map=${source_dir}=/usr\""
         "-DCMAKE_C_FLAGS_RELEASE=\"${CFLAGS_RELEASE:?missing CFLAGS_RELEASE}\""
         "-DCMAKE_C_FLAGS_DEBUG=\"${CFLAGS_DEBUG}\""
         "-DCMAKE_CXX_FLAGS=\"${CPPFLAGS} ${CXXFLAGS} -ffile-prefix-map=${source_dir}=/usr\""
         "-DCMAKE_CXX_FLAGS_RELEASE=\"${CXXFLAGS_RELEASE:?missing CXXFLAGS_RELEASE}\""
         "-DCMAKE_CXX_FLAGS_DEBUG=\"${CXXFLAGS_DEBUG}\"")
   (if (eq? os 'darwin)
       (list "-DCMAKE_OBJC_FLAGS=\"${CPPFLAGS} ${CFLAGS} -ffile-prefix-map=${source_dir}=/usr\""
             "-DCMAKE_OBJC_FLAGS_RELEASE=\"${CFLAGS_RELEASE:?missing CFLAGS_RELEASE}\""
             "-DCMAKE_OBJC_FLAGS_DEBUG=\"${CFLAGS_DEBUG}\""
             "-DCMAKE_OBJCXX_FLAGS=\"${CPPFLAGS} ${CXXFLAGS} -ffile-prefix-map=${source_dir}=/usr\""
             "-DCMAKE_OBJCXX_FLAGS_RELEASE=\"${CXXFLAGS_RELEASE:?missing CXXFLAGS_RELEASE}\""
             "-DCMAKE_OBJCXX_FLAGS_DEBUG=\"${CXXFLAGS_DEBUG}\"")
       '())
   (list "-DCMAKE_EXE_LINKER_FLAGS=\"${LDFLAGS}\""
         "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=\"${LDFLAGS_RELEASE:-}\""
         "-DCMAKE_EXE_LINKER_FLAGS_DEBUG=\"${LDFLAGS_DEBUG:-}\""
         "-DCMAKE_AR=\"${ar}\"")
   (if cross?
       (list (string-append "-DCMAKE_SYSTEM_NAME=" (cmake-system-name os))
             (string-append "-DCMAKE_SYSTEM_VERSION=" (cmake-system-version os))
             (string-append "-DCMAKE_SYSTEM_PROCESSOR=" arch)
             "-DCMAKE_DISABLE_FIND_PACKAGE_Libb2=TRUE"
             "-DCMAKE_DISABLE_FIND_PACKAGE_WrapSystemDoubleConversion=TRUE"
             "-DCMAKE_DISABLE_FIND_PACKAGE_WrapSystemMd4c=TRUE"
             "-DCMAKE_DISABLE_FIND_PACKAGE_WrapZSTD=TRUE")
       '())
   (if (eq? os 'linux)
       (list "-DINPUT_dbus=runtime")
       '())
   (if (eq? os 'darwin)
       (list "-DCMAKE_INSTALL_NAME_TOOL=true"
             "-DCMAKE_FRAMEWORK_PATH=\"${OSX_SDK}/System/Library/Frameworks\""
             "-DQT_INTERNAL_APPLE_SDK_VERSION=\"${OSX_SDK_VERSION}\""
             "-DQT_INTERNAL_XCODE_VERSION=\"${XCODE_VERSION}\""
             "-DQT_NO_APPLE_SDK_MAX_VERSION_CHECK=ON")
       '())))

(define (native-qt-config-options)
  (list "-release"
        "-make tools"
        "-no-pkg-config"
        "-no-reduce-relocations"
        "-no-use-gold-linker"
        "-prefix \"${host_prefix}\""
        "-static"
        "-no-feature-concurrent"
        "-no-feature-network"
        "-no-feature-printsupport"
        "-no-feature-sql"
        "-no-feature-testlib"
        "-no-feature-xml"
        "-no-gui"
        "-no-widgets"
        "-no-glib"
        "-no-icu"
        "-no-libudev"
        "-no-openssl"
        "-no-zstd"
        "-qt-pcre"
        "-qt-zlib"
        "-no-feature-backtrace"
        "-no-feature-permissions"
        "-no-feature-process"
        "-no-feature-settings"
        "-no-feature-androiddeployqt"
        "-no-feature-macdeployqt"
        "-no-feature-windeployqt"
        "-no-feature-qmake"
        "-feature-linguist"
        "-no-feature-assistant"
        "-no-feature-clang"
        "-no-feature-clangcpp"
        "-no-feature-designer"
        "-no-feature-pixeltool"
        "-no-feature-qdoc"
        "-no-feature-qtattributionsscanner"
        "-no-feature-qtdiag"
        "-no-feature-qtplugininfo"))

(define (native-qt-cmake-options build-os)
  (append
   (list "-DCMAKE_EXE_LINKER_FLAGS=\"${build_LDFLAGS:-}\""
         "-DCMAKE_AR=\"${build_AR}\""
         "${V:+--log-level=STATUS}")
   (if (eq? build-os 'darwin)
       (list "-DQT_NO_APPLE_SDK_MAX_VERSION_CHECK=ON"
             "-DQT_NO_XCODE_MIN_VERSION_CHECK=ON")
       '())))

(define (extract-top-level-sources include-tools?)
  (string-append
   "mkdir -p qtbase\n"
   "tar --no-same-owner --strip-components=1 -xf "
   "\"${sources_dir}/qtbase-" %qt-suffix "\" -C qtbase\n"
   (if include-tools?
       (string-append
        "mkdir -p qttranslations\n"
        "tar --no-same-owner --strip-components=1 -xf "
        "\"${sources_dir}/qttranslations-" %qt-suffix "\" -C qttranslations\n"
        "mkdir -p qttools\n"
        "tar --no-same-owner --strip-components=1 -xf "
        "\"${sources_dir}/qttools-" %qt-suffix "\" -C qttools\n")
       "")
   "cp \"${sources_dir}/CMakeLists.txt-" %qt-version "\" ./CMakeLists.txt\n"
   "mkdir -p cmake\n"
   "cp \"${sources_dir}/ECMOptionalAddSubdirectory.cmake-" %qt-version
   "\" cmake/ECMOptionalAddSubdirectory.cmake\n"
   "cp \"${sources_dir}/QtTopLevelHelpers.cmake-" %qt-version
   "\" cmake/QtTopLevelHelpers.cmake"))

(define (qt-configure os cross? arch)
  (let ((env (if (eq? os 'darwin)
                 "env CC=\"${cc}\" CXX=\"${cxx}\" OBJC=\"${cc}\" OBJCXX=\"${cxx}\""
                 "env CC=\"${cc}\" CXX=\"${cxx}\"")))
    (string-append
     "cd qtbase\n"
     env " ./configure -top-level \\\n    "
     (shell-arguments (qt-config-options os cross?))
     " -- \\\n    "
     (shell-arguments (qt-cmake-options os cross? arch)))))

(define (native-qt-configure build-os)
  ;; native_qt.mk invokes Qt's configure directly, without the generic CMake
  ;; wrapper's flags. Do not leak the target Qt environment into native tools.
  (let ((env (string-append
              "env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS"
              " CC=\"${build_CC}\" CXX=\"${build_CXX}\""
              (if (eq? build-os 'darwin)
                  " OBJC=\"${build_CC}\" OBJCXX=\"${build_CXX}\""
                  ""))))
    (string-append
     "cd qtbase\n"
     env " ./configure -top-level \\\n    "
     (shell-arguments (native-qt-config-options))
     " -- \\\n    "
     (shell-arguments (native-qt-cmake-options build-os)))))

(define %cmake-build
  "cmake --build . -- -j\"${jobs}\"")

(define %cmake-install
  "cmake --install . --prefix \"${staged_prefix}\" --strip")

(define (recipe name native? sources dependencies patches extract configure postprocess)
  `((name . ,name)
    (version . ,%qt-version)
    (sources . ,sources)
    (native? . ,native?)
    (dependencies . ,dependencies)
    (patches . ,patches)
    (build-subdir . ".")
    (configure . ,configure)
    (build . ,%cmake-build)
    (install . ,%cmake-install)
    (postprocess . ,postprocess)
    (extract . ,extract)))

(define (native-qt-recipe build-os)
  (recipe "native_qt"
          #t
          %qt-sources
          '()
          %native-qt-patches
          (extract-top-level-sources #t)
          (native-qt-configure build-os)
          "rm -rf doc/\nmv translations/ .."))

(define (qt-dependencies os cross?)
  (append (if cross? (list "native_qt") '())
          (if (eq? os 'linux) %linux-qt-dependencies '())))

(define (qt-recipe target build-triplet)
  (let* ((os (target-os target))
         (cross? (not (string=? target build-triplet)))
         (patches (if cross? %qt-cross-patches %qt-native-target-patches)))
    (recipe "qt"
            #f
            %qt-sources
            (qt-dependencies os cross?)
            patches
            (extract-top-level-sources (not cross?))
            (qt-configure os cross? (target-arch target))
            "rm -rf doc/")))

(define (qt-recipes target build-triplet)
  (let ((build-os (target-os build-triplet)))
    (if (string=? target build-triplet)
        (list (qt-recipe target build-triplet))
        (list (native-qt-recipe build-os)
              (qt-recipe target build-triplet)))))
