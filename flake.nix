{
  description = "Bitcoin Core Docker image";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        overlays = [ (import ./overlays.nix) ];
      };

      bitcoinSource = pkgs.lib.cleanSourceWith {
        name = "bitcoin-core-source";
        src = ./.;
        filter =
          path: type:
          pkgs.lib.cleanSourceFilter path type
          && !builtins.elem (baseNameOf path) [
            "flake.lock"
            "flake.nix"
            "docker.nix"
            "implementation.md"
            "justfile"
            "overlays.nix"
          ];
      };

      zeromq = pkgs.zeromq.override {
        enableCurve = false;
        enableDrafts = false;
        libsodium = null;
      };

      runtimeRpath = pkgs.lib.makeLibraryPath [
        pkgs.capnproto-runtime
        zeromq
        pkgs.sqlite
        pkgs.stdenv.cc.cc
        pkgs.glibc
      ];

      nativeBuildInputs = with pkgs; [
        cmake
        ninja
        patchelf
        pkg-config
        python3
      ];

      buildInputs = with pkgs; [
        boost
        capnproto
        sqlite
        zeromq
      ];

      bitcoin-core = pkgs.stdenv.mkDerivation {
        pname = "bitcoin-core";
        version = "31.99.0";
        src = bitcoinSource;

        inherit nativeBuildInputs buildInputs;

        cmakeFlags = [
          "-DBUILD_BITCOIN_BIN=ON"
          "-DBUILD_CLI=ON"
          "-DBUILD_DAEMON=ON"
          "-DBUILD_GUI=OFF"
          "-DBUILD_TESTS=OFF"
          "-DBUILD_TX=ON"
          "-DBUILD_UTIL=ON"
          "-DBUILD_WALLET_TOOL=ON"
          "-DENABLE_EXTERNAL_SIGNER=OFF"
          "-DENABLE_IPC=ON"
          "-DENABLE_WALLET=ON"
          "-DINSTALL_MAN=OFF"
          "-DWITH_CCACHE=OFF"
          "-DWITH_USDT=OFF"
          "-DWITH_ZMQ=ON"
        ];

        installPhase = ''
          runHook preInstall
          install -Dm755 bin/bitcoin $out/bin/bitcoin
          install -Dm755 bin/bitcoind $out/bin/bitcoind
          install -Dm755 bin/bitcoin-cli $out/bin/bitcoin-cli
          install -Dm755 bin/bitcoin-tx $out/bin/bitcoin-tx
          install -Dm755 bin/bitcoin-util $out/bin/bitcoin-util
          install -Dm755 bin/bitcoin-wallet $out/bin/bitcoin-wallet
          install -Dm755 bin/bitcoin-node $out/libexec/bitcoin-node
          runHook postInstall
        '';

        postFixup = ''
          for binary in $out/bin/bitcoin-cli $out/libexec/bitcoin-node; do
            patchelf --set-rpath "${runtimeRpath}" "$binary"
            patchelf --shrink-rpath "$binary"
          done
        '';
      };

      docker-image = import ./docker.nix {
        inherit pkgs;
        bitcoinCore = bitcoin-core;
      };
    in
    {
      packages.${system} = {
        default = docker-image;
        inherit bitcoin-core docker-image;
      };

      devShells.${system}.default = pkgs.mkShell {
        inherit nativeBuildInputs buildInputs;
      };

      formatter.${system} = pkgs.nixfmt-tree;
    };
}
