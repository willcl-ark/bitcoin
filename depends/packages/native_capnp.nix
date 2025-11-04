{
  lib,
  stdenv,
  fetchurl,
  cmake,
  which,
}:
stdenv.mkDerivation rec {
  pname = "native-capnp";
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

  postInstall = ''
    # Remove pkg-config files as we don't need them
    rm -rf $out/lib/pkgconfig
  '';

  meta = with lib; {
    description = "Cap'n Proto native tools for Bitcoin Core build";
    homepage = "https://capnproto.org/";
    license = licenses.mit;
    platforms = platforms.all;
  };
}
