{
  lib,
  stdenv,
  fetchurl,
  cmake,
  which,
  commonCFlags ? "",
  commonCXXFlags ? "",
  commonCPPFlags ? "",
}:
stdenv.mkDerivation rec {
  pname = "boost";
  version = "1.88.0";

  src = fetchurl {
    url = "https://github.com/boostorg/boost/releases/download/boost-${version}/boost-${version}-cmake.tar.gz";
    sha256 = "dcea50f40ba1ecfc448fdf886c0165cf3e525fef2c9e3e080b9804e8117b9694";
  };

  nativeBuildInputs = [
    cmake
    which
  ];

  patches = [
    ../patches/boost/skip_compiled_targets.patch
  ];

  cmakeFlags = [
    "-DBOOST_INCLUDE_LIBRARIES=multi_index;signals2;test"
    "-DBOOST_TEST_HEADERS_ONLY=ON"
    "-DBOOST_ENABLE_MPI=OFF"
    "-DBOOST_ENABLE_PYTHON=OFF"
    "-DBOOST_INSTALL_LAYOUT=system"
    "-DBUILD_TESTING=OFF"
    "-DCMAKE_DISABLE_FIND_PACKAGE_ICU=ON"
  ];

  NIX_CFLAGS_COMPILE = commonCFlags;
  NIX_CXXFLAGS_COMPILE = commonCXXFlags;
  NIX_CPPFLAGS = commonCPPFlags;

  env = lib.optionalAttrs stdenv.hostPlatform.isFreeBSD {
    CXXFLAGS = "${commonCXXFlags} -isystem${stdenv.cc.libcxx.dev}/include/c++/v1 -isystem${stdenv.cc.libc_dev}/include";
  };

  meta = with lib; {
    description = "Boost C++ Libraries for Bitcoin Core";
    homepage = "https://www.boost.org/";
    license = licenses.boost;
    platforms = platforms.all;
  };
}
