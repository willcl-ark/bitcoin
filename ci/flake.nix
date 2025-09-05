{
  description = "Bitcoin Core build derivation";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    nixpkgs,
    flake-utils,
    ...
  }:
    flake-utils.lib.eachDefaultSystem (
      system: let
        pkgs = import nixpkgs {inherit system;};
        inherit (pkgs) lib ccacheStdenv;

        # Shared dependencies for build and runtime
        commonInputs = with pkgs; [
          boost
          capnproto
          libevent
          libsystemtap
          linuxPackages.bcc
          linuxPackages.bpftrace
          sqlite.dev
          zeromq
        ];

        bitcoind = ccacheStdenv.mkDerivation {
          pname = "bitcoin-core";
          version = "unstable";

          src = ../.;

          nativeBuildInputs =
            commonInputs
            ++ (with pkgs; [
              ccache
              cmake
              curlMinimal
              ninja
              pkg-config
            ]);

          buildInputs = commonInputs;

          env = {
            CMAKE_GENERATOR = "Ninja";
            LD_LIBRARY_PATH = lib.makeLibraryPath [pkgs.capnproto];
          };

          cmakeFlags = [
            "-DBUILD_DAEMON=ON"
            "-DBUILD_BITCOIN_BIN=OFF"
            "-DBUILD_CLI=OFF"
            "-DBUILD_TX=OFF"
            "-DBUILD_UTIL=OFF"
            "-DBUILD_WALLET_TOOL=OFF"
            "-DBUILD_TESTS=OFF"
            "-DBUILD_BENCH=OFF"
            "-DENABLE_WALLET=OFF"
            "-DENABLE_EXTERNAL_SIGNER=OFF"
          ];

          stripAllList = ["bin"];

          meta = with lib; {
            description = "Bitcoin Core client";
            homepage = "https://bitcoincore.org/";
            license = licenses.mit;
            platforms = platforms.all;
          };
        };
      in {
        formatter = pkgs.alejandra;
        packages = {
          default = bitcoind;
          inherit bitcoind;
        };
      }
    );
}
