{
  description = "Bitcoin Core package flake";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
  };

  outputs =
    { self, nixpkgs, ... }:
    let
      lib = nixpkgs.lib;
      packageVersion =
        if self ? rev then
          lib.substring 0 12 self.rev
        else if self ? dirtyRev then
          lib.substring 0 12 self.dirtyRev
        else
          "unknown";
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          mkBitcoinCore =
            {
              packageSet,
              pname,
            }:
            let
              toolPkgs = packageSet.buildPackages;
            in
            packageSet.stdenv.mkDerivation {
              inherit pname;
              version = packageVersion;
              src = ./.;

              strictDeps = true;
              enableParallelBuilding = true;

              nativeBuildInputs = [
                toolPkgs.cmake
                toolPkgs.ninja
                toolPkgs.pkg-config
              ];

              buildInputs = [
                packageSet.boost
                packageSet.capnproto
                packageSet.libevent
                packageSet.sqlite
              ];

              cmakeFlags = [
                "-DBUILD_GUI=OFF"
                "-DBUILD_TESTS=OFF"
                "-DBUILD_TX=ON"
                "-DBUILD_UTIL=ON"
                "-DBUILD_WALLET_TOOL=ON"
                "-DBUILD_BENCH=OFF"
                "-DBUILD_FUZZ_BINARY=OFF"
                "-DWITH_ZMQ=OFF"
                "-DWITH_USDT=OFF"
                "-DAPPEND_LDFLAGS=-static"
              ];

              meta = with lib; {
                description = "Bitcoin Core static Linux binaries";
                homepage = "https://bitcoincore.org/";
                license = licenses.mit;
                mainProgram = "bitcoind";
                platforms = platforms.linux;
              };
            };

          bitcoin-core = mkBitcoinCore {
            packageSet = pkgs.pkgsStatic;
            pname = "bitcoin-core";
          };
        in
        {
          default = bitcoin-core;
          bitcoin-core = bitcoin-core;
        }
      );
    };
}
