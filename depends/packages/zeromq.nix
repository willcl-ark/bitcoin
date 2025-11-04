{
  lib,
  stdenv,
  fetchurl,
  cmake,
  which,
}:
stdenv.mkDerivation rec {
  pname = "zeromq";
  version = "4.3.5";

  src = fetchurl {
    url = "https://github.com/zeromq/libzmq/releases/download/v${version}/zeromq-${version}.tar.gz";
    sha256 = "6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43";
  };

  nativeBuildInputs = [
    cmake
    which
  ];

  patches = [
    ../patches/zeromq/builtin_sha1.patch
    ../patches/zeromq/cacheline_undefined.patch
    ../patches/zeromq/cmake_minimum.patch
    ../patches/zeromq/fix_have_windows.patch
    ../patches/zeromq/macos_mktemp_check.patch
    ../patches/zeromq/no_librt.patch
    ../patches/zeromq/openbsd_kqueue_headers.patch
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=None"
    "-DWITH_DOCS=OFF"
    "-DWITH_LIBSODIUM=OFF"
    "-DWITH_LIBBSD=OFF"
    "-DENABLE_CURVE=OFF"
    "-DENABLE_CPACK=OFF"
    "-DBUILD_SHARED=OFF"
    "-DBUILD_TESTS=OFF"
    "-DZMQ_BUILD_TESTS=OFF"
    "-DENABLE_DRAFTS=OFF"
  ]
  ++ lib.optionals stdenv.hostPlatform.isMinGW [
    "-DZMQ_WIN32_WINNT=0x0A00"
    "-DZMQ_HAVE_IPC=OFF"
  ];

  NIX_CXXFLAGS_COMPILE = [
    "-fdebug-prefix-map=${placeholder "out"}=/usr"
    "-fmacro-prefix-map=${placeholder "out"}=/usr"
  ];

  env = lib.optionalAttrs stdenv.hostPlatform.isFreeBSD {
    CXXFLAGS = "-isystem${stdenv.cc.libcxx.dev}/include/c++/v1 -isystem${stdenv.cc.libc_dev}/include";
  };

  postInstall = ''
    # Remove files that zeromq.mk removes
    rm -rf $out/share $out/lib/pkgconfig
  '';

  meta = with lib; {
    description = "ZeroMQ library for Bitcoin Core";
    homepage = "https://zeromq.org/";
    license = licenses.lgpl3Plus;
    platforms = platforms.all;
  };
}
