{
  lib,
  stdenv,
  fetchurl,
  autoreconfHook,
  which,
  commonCFlags ? "",
  commonCXXFlags ? "",
  commonCPPFlags ? "",
}:
stdenv.mkDerivation rec {
  pname = "sqlite";
  version = "3460100";

  src = fetchurl {
    url = "https://sqlite.org/2024/sqlite-autoconf-${version}.tar.gz";
    sha256 = "67d3fe6d268e6eaddcae3727fce58fcc8e9c53869bdd07a0c61e38ddf2965071";
  };

  nativeBuildInputs = [
    autoreconfHook
    which
  ];

  configureFlags = [
    "--disable-shared"
    "--disable-readline"
    "--disable-dynamic-extensions"
    "--enable-option-checking"
    "--disable-rtree"
    "--disable-fts4"
    "--disable-fts5"
  ];

  NIX_CFLAGS_COMPILE = commonCFlags;
  NIX_CXXFLAGS_COMPILE = commonCXXFlags;
  NIX_CPPFLAGS = [
    commonCPPFlags
    "-DSQLITE_DQS=0"
    "-DSQLITE_DEFAULT_MEMSTATUS=0"
    "-DSQLITE_OMIT_DEPRECATED"
    "-DSQLITE_OMIT_SHARED_CACHE"
    "-DSQLITE_OMIT_JSON"
    "-DSQLITE_LIKE_DOESNT_MATCH_BLOBS"
    "-DSQLITE_OMIT_DECLTYPE"
    "-DSQLITE_OMIT_PROGRESS_CALLBACK"
    "-DSQLITE_OMIT_AUTOINIT"
  ];

  # Build only the library like sqlite.mk does
  buildFlags = [ "libsqlite3.la" ];

  installTargets = [
    "install-libLTLIBRARIES"
    "install-includeHEADERS"
  ];

  env = lib.optionalAttrs stdenv.hostPlatform.isFreeBSD {
    CFLAGS = "${commonCFlags} -isystem${stdenv.cc.libc_dev}/include";
  };

  # Remove .la files like sqlite.mk postprocess does
  postInstall = ''
    rm -f $out/lib/*.la
  '';

  meta = with lib; {
    description = "SQLite database library for Bitcoin Core";
    homepage = "https://www.sqlite.org/";
    license = licenses.publicDomain;
    platforms = platforms.all;
  };
}
