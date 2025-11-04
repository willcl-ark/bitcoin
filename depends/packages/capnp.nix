{
  lib,
  stdenv,
  fetchurl,
  cmake,
  which,
}:
stdenv.mkDerivation rec {
  pname = "capnp";
  version = "1.2.0";

  src = fetchurl {
    url = "https://capnproto.org/capnproto-c++-${version}.tar.gz";
    sha256 = "ed00e44ecbbda5186bc78a41ba64a8dc4a861b5f8d4e822959b0144ae6fd42ef";
  };

  nativeBuildInputs = [
    cmake
    which
  ];

  cmakeFlags = [
    "-DBUILD_TESTING=OFF"
    "-DWITH_OPENSSL=OFF"
    "-DWITH_ZLIB=OFF"
  ];

  NIX_CXXFLAGS_COMPILE = [
    "-fdebug-prefix-map=${placeholder "out"}=/usr"
    "-fmacro-prefix-map=${placeholder "out"}=/usr"
  ];

  env = lib.optionalAttrs stdenv.hostPlatform.isFreeBSD {
    CXXFLAGS = "-isystem${stdenv.cc.libcxx.dev}/include/c++/v1 -isystem${stdenv.cc.libc_dev}/include";
  };

  postInstall = ''
    # Remove files that capnp.mk removes
    rm -rf $out/lib/pkgconfig
  '';

  meta = with lib; {
    description = "Cap'n Proto serialization library for Bitcoin Core";
    homepage = "https://capnproto.org/";
    license = licenses.mit;
    platforms = platforms.all;
  };
}
