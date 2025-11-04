{
  lib,
  stdenv,
  cmake,
  which,
  pkg-config,
  native_capnp,
  version,
}:
stdenv.mkDerivation rec {
  pname = "native-libmultiprocess";
  inherit version;

  # Use the libmultiprocess source from the Bitcoin Core tree
  src = builtins.path {
    path = ../../src/ipc/libmultiprocess;
    name = "libmultiprocess-source";
  };

  nativeBuildInputs = [
    cmake
    which
    pkg-config
  ];

  buildInputs = [
    native_capnp # Need native capnp libraries and tools for building
  ];

  cmakeFlags = [
    "-DBUILD_TESTING=OFF"
    "-DCapnProto_DIR=${native_capnp}/lib/cmake/CapnProto"
  ];

  # Only build and install the mpgen binary - we don't need the library
  buildPhase = ''
    make mpgen
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp mpgen $out/bin/
  '';

  meta = with lib; {
    description = "Native libmultiprocess mpgen tool for Bitcoin Core build";
    homepage = "https://github.com/chaincodelabs/libmultiprocess";
    license = licenses.mit;
    platforms = platforms.all;
  };
}
