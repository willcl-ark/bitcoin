;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(define-module (bitcoin depends gui)
  #:use-module (srfi srfi-13)
  #:export (gui-recipes))

(define (linux-target? target)
  (and (string? target)
       (string-contains target "-linux-")))

(define (source file-name uri sha256)
  (list file-name uri sha256))

(define (patch file-name strip-level)
  (list file-name strip-level))

(define* (recipe name
                 version
                 sources
                 #:key
                 (native? #f)
                 (dependencies '())
                 (patches '())
                 (build-subdir ".")
                 (configure "true")
                 (build "true")
                 (install "true")
                 (postprocess "true")
                 (extra-cflags "")
                 (extra-cxxflags "")
                 (extra-cppflags "")
                 (extra-ldflags "")
                 (preprocess #f))
  `((name . ,name)
    (version . ,version)
    (sources . ,sources)
    (native? . ,native?)
    (dependencies . ,dependencies)
    (patches . ,patches)
    (build-subdir . ,build-subdir)
    (configure . ,configure)
    (build . ,build)
    (install . ,install)
    (postprocess . ,postprocess)
    (extra-cflags . ,extra-cflags)
    (extra-cxxflags . ,extra-cxxflags)
    (extra-cppflags . ,extra-cppflags)
    (extra-ldflags . ,extra-ldflags)
    ,@(if preprocess
          `((preprocess . ,preprocess))
          '())))

(define copy-config-sub-guess-preprocess
  "cp -f \"$depends_dir/config.guess\" \"$depends_dir/config.sub\" .")

(define (gui-recipes target build-triplet)
  (unless (string? build-triplet)
    (error "build triplet must be a string" build-triplet))
  (if (linux-target? target)
      (list
       ;; depends/packages/expat.mk
       (recipe "expat"
               "2.7.3"
               (list (source "expat-2.7.3.tar.gz"
                             "https://github.com/libexpat/libexpat/releases/download/R_2_7_3//expat-2.7.3.tar.gz"
                             "821ac9710d2c073eaf13e1b1895a9c9aa66c1157a99635c639fbff65cdbdd732"))
               #:build-subdir "build"
               #:configure (string-append
                             "configure_cmake"
                             " -DCMAKE_BUILD_TYPE=None"
                             " -DEXPAT_BUILD_TOOLS=OFF"
                             " -DEXPAT_BUILD_EXAMPLES=OFF"
                             " -DEXPAT_BUILD_TESTS=OFF"
                             " -DBUILD_SHARED_LIBS=OFF"
                             " -S .. -B .")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf share lib/cmake")

       ;; depends/packages/libxcb.mk
       (recipe "libxcb"
               "1.17.0"
               (list (source "libxcb-1.17.0.tar.gz"
                             "https://xcb.freedesktop.org/dist/libxcb-1.17.0.tar.gz"
                             "2c69287424c9e2128cb47ffe92171e10417041ec2963bceafb65cb3fcf8f0b85"))
               #:dependencies '("xcb_proto" "libXau")
               #:patches (list (patch "libxcb/remove_pthread_stubs.patch" 1))
               #:preprocess "cp -f \"$depends_dir/config.guess\" \"$depends_dir/config.sub\" build-aux"
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-shared --disable-devel-docs --without-doxygen"
                             " --disable-dependency-tracking --enable-option-checking"
                             " --disable-composite --disable-damage --disable-dpms"
                             " --disable-dri2 --disable-dri3 --disable-glx"
                             " --disable-present --disable-record --disable-resource"
                             " --disable-screensaver --disable-xevie --disable-xfree86-dri"
                             " --disable-xinput --disable-xprint --disable-selinux"
                             " --disable-xtest --disable-xv --disable-xvmc --disable-xinerama")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf share lib/*.la")

       ;; depends/packages/xcb_proto.mk
       (recipe "xcb_proto"
               "1.17.0"
               (list (source "xcb-proto-1.17.0.tar.gz"
                             "https://xorg.freedesktop.org/archive/individual/proto/xcb-proto-1.17.0.tar.gz"
                             "392d3c9690f8c8202a68fdb89c16fd55159ab8d65000a6da213f4a1576e97a16"))
               #:configure "configure_autoconf"
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf lib/python*/site-packages/xcbgen/__pycache__")

       ;; depends/packages/libXau.mk
       (recipe "libXau"
               "1.0.12"
               (list (source "libXau-1.0.12.tar.gz"
                             "https://xorg.freedesktop.org/releases/individual/lib//libXau-1.0.12.tar.gz"
                             "2402dd938da4d0a332349ab3d3586606175e19cb32cb9fe013c19f1dc922dcee"))
               #:dependencies '("xproto")
               #:preprocess copy-config-sub-guess-preprocess
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-shared --disable-lint-library --without-lint"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf share lib/*.la")

       ;; depends/packages/xproto.mk
       (recipe "xproto"
               "7.0.31"
               (list (source "xproto-7.0.31.tar.gz"
                             "https://xorg.freedesktop.org/releases/individual/proto/xproto-7.0.31.tar.gz"
                             "6d755eaae27b45c5cc75529a12855fed5de5969b367ed05003944cf901ed43c7"))
               #:preprocess copy-config-sub-guess-preprocess
               #:configure (string-append
                             "configure_autoconf"
                             " --without-fop --without-xmlto --without-xsltproc --disable-specs"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" MKDIRPROG=\"mkdir -p\" DESTDIR=\"$stage_dir\" install")

       ;; depends/packages/freetype.mk
       (recipe "freetype"
               "2.11.1"
               (list (source "freetype-2.11.1.tar.gz"
                             "https://download.savannah.gnu.org/releases/freetype/freetype-2.11.1.tar.gz"
                             "f8db94d307e9c54961b39a1cc799a67d46681480696ed72ecf78d4473770f09b"))
               #:patches (list (patch "freetype/cmake_minimum.patch" 1)
                               (patch "freetype/openbsd_versioning.patch" 1))
               #:build-subdir "build"
               #:configure (string-append
                             "configure_cmake"
                             " -DCMAKE_BUILD_TYPE=None"
                             " -DBUILD_SHARED_LIBS=TRUE"
                             " -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE"
                             " -DCMAKE_DISABLE_FIND_PACKAGE_PNG=TRUE"
                             " -DCMAKE_DISABLE_FIND_PACKAGE_HarfBuzz=TRUE"
                             " -DCMAKE_DISABLE_FIND_PACKAGE_BZip2=TRUE"
                             " -DCMAKE_DISABLE_FIND_PACKAGE_BrotliDec=TRUE"
                             " -S .. -B .")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install")

       ;; depends/packages/fontconfig.mk
       (recipe "fontconfig"
               "2.12.6"
               (list (source "fontconfig-2.12.6.tar.gz"
                             "https://www.freedesktop.org/software/fontconfig/release//fontconfig-2.12.6.tar.gz"
                             "064b9ebf060c9e77011733ac9dc0e2ce92870b574cca2405e11f5353a683c334"))
               #:dependencies '("freetype" "expat")
               #:patches (list (patch "fontconfig/gperf_header_regen.patch" 1))
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-docs --disable-static --disable-libxml2 --disable-iconv"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf bin etc share var lib/*.la"
               #:extra-cflags "-Wno-implicit-function-declaration")

       ;; depends/packages/libxkbcommon.mk
       (recipe "libxkbcommon"
               "0.8.4"
               (list (source "libxkbcommon-0.8.4.tar.xz"
                             "https://xkbcommon.org/download//libxkbcommon-0.8.4.tar.xz"
                             "60ddcff932b7fd352752d51a5c4f04f3d0403230a584df9a2e0d5ed87c486c8b"))
               #:dependencies '("libxcb")
               #:preprocess "cp -f \"$depends_dir/config.guess\" \"$depends_dir/config.sub\" build-aux"
               #:configure (string-append
                             "configure_autoconf"
                             " --enable-option-checking --disable-dependency-tracking"
                             " --disable-shared --disable-docs")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm lib/*.la"
               #:extra-cflags "-Wno-error=array-bounds")

       ;; depends/packages/libxcb_util.mk
       (recipe "libxcb_util"
               "0.4.1"
               (list (source "xcb-util-0.4.1.tar.gz"
                             "https://xcb.freedesktop.org/dist/xcb-util-0.4.1.tar.gz"
                             "21c6e720162858f15fe686cef833cf96a3e2a79875f84007d76f6d00417f593a"))
               #:dependencies '("libxcb")
               #:preprocess copy-config-sub-guess-preprocess
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-shared --disable-devel-docs --without-doxygen"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf share/man share/doc lib/*.la")

       ;; depends/packages/libxcb_util_cursor.mk
       (recipe "libxcb_util_cursor"
               "0.1.6"
               (list (source "xcb-util-cursor-0.1.6.tar.gz"
                             "https://xcb.freedesktop.org/dist/xcb-util-cursor-0.1.6.tar.gz"
                             "eae38b2dfc5c529a886e507ef576b12d2a20aa1f149608e4853af760f31be60b"))
               #:dependencies '("libxcb" "libxcb_util_render" "libxcb_util_image")
               #:preprocess copy-config-sub-guess-preprocess
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-shared"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm lib/*.la")

       ;; depends/packages/libxcb_util_render.mk
       (recipe "libxcb_util_render"
               "0.3.10"
               (list (source "xcb-util-renderutil-0.3.10.tar.gz"
                             "https://xcb.freedesktop.org/dist/xcb-util-renderutil-0.3.10.tar.gz"
                             "e04143c48e1644c5e074243fa293d88f99005b3c50d1d54358954404e635128a"))
               #:dependencies '("libxcb")
               #:preprocess copy-config-sub-guess-preprocess
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-shared --disable-devel-docs --without-doxygen"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf share/man share/doc lib/*.la")

       ;; depends/packages/libxcb_util_keysyms.mk
       (recipe "libxcb_util_keysyms"
               "0.4.1"
               (list (source "xcb-util-keysyms-0.4.1.tar.gz"
                             "https://xcb.freedesktop.org/dist/xcb-util-keysyms-0.4.1.tar.gz"
                             "1fa21c0cea3060caee7612b6577c1730da470b88cbdf846fa4e3e0ff78948e54"))
               #:dependencies '("libxcb" "xproto")
               #:preprocess copy-config-sub-guess-preprocess
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-shared --disable-devel-docs --without-doxygen"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf share/man share/doc lib/*.la")

       ;; depends/packages/libxcb_util_image.mk
       (recipe "libxcb_util_image"
               "0.4.1"
               (list (source "xcb-util-image-0.4.1.tar.gz"
                             "https://xcb.freedesktop.org/dist/xcb-util-image-0.4.1.tar.gz"
                             "0ebd4cf809043fdeb4f980d58cdcf2b527035018924f8c14da76d1c81001293b"))
               #:dependencies '("libxcb" "libxcb_util")
               #:preprocess copy-config-sub-guess-preprocess
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-shared --disable-devel-docs --without-doxygen"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\" -C image"
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" -C image install"
               #:postprocess "rm -rf share/man share/doc lib/*.la")

       ;; depends/packages/libxcb_util_wm.mk
       (recipe "libxcb_util_wm"
               "0.4.2"
               (list (source "xcb-util-wm-0.4.2.tar.gz"
                             "https://xcb.freedesktop.org/dist/xcb-util-wm-0.4.2.tar.gz"
                             "dcecaaa535802fd57c84cceeff50c64efe7f2326bf752e16d2b77945649c8cd7"))
               #:dependencies '("libxcb")
               #:preprocess copy-config-sub-guess-preprocess
               #:configure (string-append
                             "configure_autoconf"
                             " --disable-shared --disable-devel-docs --without-doxygen"
                             " --disable-dependency-tracking --enable-option-checking")
               #:build "make -j \"$jobs\""
               #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
               #:postprocess "rm -rf share/man share/doc lib/*.la"))
      '()))
