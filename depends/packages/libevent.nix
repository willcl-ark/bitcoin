{
  lib,
  stdenv,
  fetchurl,
  cmake,
  which,
}:
stdenv.mkDerivation rec {
  pname = "libevent";
  version = "2.1.12-stable";

  src = fetchurl {
    url = "https://github.com/libevent/libevent/releases/download/release-${version}/libevent-${version}.tar.gz";
    sha256 = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb";
  };

  nativeBuildInputs = [
    cmake
    which
  ];

  patches = [
    ../patches/libevent/cmake_fixups.patch
    ../patches/libevent/netbsd_fixup.patch
    ../patches/libevent/winver_fixup.patch
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=None"
    "-DEVENT__DISABLE_BENCHMARK=ON"
    "-DEVENT__DISABLE_OPENSSL=ON"
    "-DEVENT__DISABLE_SAMPLES=ON"
    "-DEVENT__DISABLE_REGRESS=ON"
    "-DEVENT__DISABLE_TESTS=ON"
    "-DEVENT__LIBRARY_TYPE=STATIC"
  ];

  NIX_CFLAGS_COMPILE = [
    "-fdebug-prefix-map=${placeholder "out"}=/usr"
    "-fmacro-prefix-map=${placeholder "out"}=/usr"
  ];

  NIX_CPPFLAGS = [
    "-D_GNU_SOURCE"
    "-U_FORTIFY_SOURCE"
    "-D_FORTIFY_SOURCE=3"
  ]
  ++ lib.optionals stdenv.hostPlatform.isMinGW [
    "-D_WIN32_WINNT=0x0A00"
  ];

  env = lib.optionalAttrs stdenv.hostPlatform.isFreeBSD {
    CFLAGS = "-isystem${stdenv.cc.libc_dev}/include";
  };

  postInstall = ''
    # Remove files that libevent.mk removes
    rm -rf $out/bin $out/lib/pkgconfig
    rm -f $out/include/ev*.h
    rm -f $out/include/event2/*_compat.h
    rm -f $out/lib/libevent.a
  '';

  meta = with lib; {
    description = "Libevent library for Bitcoin Core";
    homepage = "https://libevent.org/";
    license = licenses.bsd3;
    platforms = platforms.all;
  };
}
