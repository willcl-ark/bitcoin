;;; Copyright (c) The Bitcoin Core developers
;;; Distributed under the MIT software license, see the accompanying
;;; file COPYING or https://opensource.org/license/mit.

(define-module (bitcoin depends base)
  #:use-module (srfi srfi-13)
  #:export (base-recipes))

(define %capnp-version "1.5.0")
(define %capnp-source-hash
  "77dbc13ca82d9c87ddb4581dd49559d45b63096433d3dadea08b7f31b360a5ba")

(define (target->os target)
  (cond ((string-suffix? "-mingw32" target) "mingw32")
        ((string-contains target "darwin") "darwin")
        ((string-contains target "freebsd") "freebsd")
        ((string-contains target "netbsd") "netbsd")
        ((string-contains target "openbsd") "openbsd")
        ((string-contains target "-linux-") "linux")
        (else (error "unsupported depends target" target))))

(define (patch file strip-level)
  (list file strip-level))

(define* (recipe name
                 version
                 sources
                 #:key
                 local-source
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
                 (autoconf-with-pic? #t))
  (append
   `((name . ,name)
     (version . ,version)
     (sources . ,sources))
   (if local-source
       `((local-source . ,local-source))
       '())
   `((native? . ,native?)
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
     (extra-ldflags . ,extra-ldflags))
   (if autoconf-with-pic?
       '()
       `((autoconf-with-pic? . #f)))))

(define (source filename uri sha256)
  (list filename uri sha256))

(define (capnp-sources)
  (list (source (string-append "capnproto-cxx-" %capnp-version ".tar.gz")
                (string-append "https://capnproto.org/capnproto-c++-"
                               %capnp-version ".tar.gz")
                %capnp-source-hash)))

(define (base-recipes target build-triplet)
  (unless (string? build-triplet)
    (error "build triplet must be a string" build-triplet))
  (let* ((target-os (target->os target))
         (mingw? (string=? target-os "mingw32"))
         (zeromq-mingw-options
          (if mingw?
              " -DZMQ_WIN32_WINNT=0x0A00 -DZMQ_HAVE_IPC=OFF"
              "")))
    (list
     ;; depends/packages/boost.mk
     (recipe
      "boost"
      "1.92.0"
      (list (source
             "boost-1.92.0-cmake.tar.gz"
             "https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.gz"
             "f51707c27359a0df0cac1beada86de31bb5eed5e8285592dadec384df99c2984"))
      #:build-subdir "build"
      #:configure
      (string-append
       "configure_cmake"
       " -DBOOST_INCLUDE_LIBRARIES=\"multi_index;test\""
       " -DBOOST_TEST_HEADERS_ONLY=ON"
       " -DBOOST_ENABLE_MPI=OFF"
       " -DBOOST_ENABLE_PYTHON=OFF"
       " -DBOOST_INSTALL_LAYOUT=system"
       " -DBUILD_TESTING=OFF"
       " -DCMAKE_DISABLE_FIND_PACKAGE_ICU=ON"
       " -DCMAKE_INSTALL_INCLUDEDIR=boost/include"
       " -S .. -B .")
      #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
      #:postprocess "rm -rf share")

     ;; depends/packages/sqlite.mk
     (recipe
      "sqlite"
      "3500400"
      (list (source
             "sqlite-autoconf-3500400.tar.gz"
             "https://sqlite.org/2025/sqlite-autoconf-3500400.tar.gz"
             "a3db587a1b92ee5ddac2f66b3edb41b26f9c867275782d46c3a088977d6a5b18"))
      #:patches (list (patch "sqlite/autosetup-fixup.patch" 1))
      #:configure
      (string-append
       "CC_FOR_BUILD=\"$build_CC\" configure_autoconf"
       " --disable-shared --disable-readline --disable-rtree"
       " --disable-fts4 --disable-fts5")
      #:build "make -j \"$jobs\" libsqlite3.a"
      #:install
      "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install-headers install-lib"
      #:extra-cppflags
      (string-append
       "-DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0"
       " -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE"
       " -DSQLITE_OMIT_JSON -DSQLITE_LIKE_DOESNT_MATCH_BLOBS"
       " -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_PROGRESS_CALLBACK"
       " -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION")
      #:autoconf-with-pic? #f)

     ;; depends/packages/zeromq.mk
     (recipe
      "zeromq"
      "4.3.5"
      (list (source
             "zeromq-4.3.5.tar.gz"
             "https://github.com/zeromq/libzmq/releases/download/v4.3.5//zeromq-4.3.5.tar.gz"
             "6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43"))
      #:patches
      (list (patch "zeromq/macos_mktemp_check.patch" 1)
            (patch "zeromq/builtin_sha1.patch" 1)
            (patch "zeromq/cacheline_undefined.patch" 1)
            (patch "zeromq/fix_have_windows.patch" 1)
            (patch "zeromq/openbsd_kqueue_headers.patch" 1)
            (patch "zeromq/cmake_minimum.patch" 1)
            (patch "zeromq/no_librt.patch" 1)
            (patch "zeromq/add_new_include.patch" 1))
      #:build-subdir "build"
      #:configure
      (string-append
       "configure_cmake"
       " -DCMAKE_BUILD_TYPE=None -DWITH_DOCS=OFF -DWITH_LIBSODIUM=OFF"
       " -DWITH_LIBBSD=OFF -DENABLE_CURVE=OFF -DENABLE_CPACK=OFF"
       " -DBUILD_SHARED=OFF -DBUILD_TESTS=OFF -DZMQ_BUILD_TESTS=OFF"
       " -DENABLE_DRAFTS=OFF -DZMQ_BUILD_TESTS=OFF"
       zeromq-mingw-options
       " -S .. -B .")
      #:build "make -j \"$jobs\""
      #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
      #:postprocess "rm -rf share lib/pkgconfig"
      #:extra-cxxflags
      "-fdebug-prefix-map=${source_dir}=/usr -fmacro-prefix-map=${source_dir}=/usr")

     ;; depends/packages/native_capnp.mk
     (recipe
      "native_capnp"
      %capnp-version
      (capnp-sources)
      #:native? #t
      #:configure
      (string-append
       "configure_cmake"
       " -DBUILD_TESTING=OFF"
       " -DWITH_OPENSSL=OFF"
       " -DWITH_ZLIB=OFF"
       " .")
      #:build "make -j \"$jobs\""
      #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
      #:postprocess "rm -rf lib/pkgconfig")

     ;; depends/packages/capnp.mk
     (recipe
      "capnp"
      %capnp-version
      (capnp-sources)
      #:patches (list (patch "capnp/macos_accept_dead_socket.patch" 2))
      #:configure
      (string-append
       "configure_cmake"
       " -DBUILD_TESTING=OFF"
       " -DWITH_OPENSSL=OFF"
       " -DWITH_ZLIB=OFF"
       " .")
      #:build "make -j \"$jobs\""
      #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
      #:postprocess "rm -rf lib/pkgconfig"
      #:extra-cxxflags
      "-fdebug-prefix-map=${source_dir}=/usr -fmacro-prefix-map=${source_dir}=/usr")

     ;; depends/packages/native_libmultiprocess.mk
     (recipe
      "native_libmultiprocess"
      "local"
      '()
      #:local-source "src/ipc/libmultiprocess"
      #:native? #t
      #:dependencies '("native_capnp")
      #:configure "configure_cmake ."
      #:build "make -j \"$jobs\""
      #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install-bin")

     ;; depends/packages/systemtap.mk
     (recipe
      "systemtap"
      "5.3"
      (list (source
             "systemtap-5.3.tar.gz"
             "https://sourceware.org/ftp/systemtap/releases/systemtap-5.3.tar.gz"
             "966a360fb73a4b65a8d0b51b389577b3c4f92a327e84aae58682103e8c65a69a"))
      #:patches
      (list
       (patch "systemtap/remove_SDT_ASM_SECTION_AUTOGROUP_SUPPORT_check.patch"
              1))
      #:install
      (string-append
       "mkdir -p \"$staged_prefix/systemtap/include/sys\" && "
       "cp includes/sys/sdt.h "
       "\"$staged_prefix/systemtap/include/sys/sdt.h\""))

     ;; depends/packages/qrencode.mk
     (recipe
      "qrencode"
      "4.1.1"
      (list (source
             "qrencode-4.1.1.tar.gz"
             "https://fukuchi.org/works/qrencode/qrencode-4.1.1.tar.gz"
             "da448ed4f52aba6bcb0cd48cac0dd51b8692bccc4cd127431402fca6f8171e8e"))
      #:patches (list (patch "qrencode/cmake_fixups.patch" 1))
      #:configure
      (string-append
       "configure_cmake"
       " -DWITH_TOOLS=NO -DWITH_TESTS=NO -DGPROF=OFF -DCOVERAGE=OFF"
       " -DCMAKE_DISABLE_FIND_PACKAGE_PNG=TRUE -DWITHOUT_PNG=ON"
       " -DCMAKE_DISABLE_FIND_PACKAGE_ICONV=TRUE"
       " -S . -B .")
      #:build "make -j \"$jobs\""
      #:install "make -j \"$jobs\" DESTDIR=\"$stage_dir\" install"
      #:postprocess "rm -rf share lib/pkgconfig"
      #:extra-cflags
      "-Wno-int-conversion -Wno-implicit-function-declaration"))))
