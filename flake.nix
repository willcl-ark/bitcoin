{
  description = "Bitcoin Core Docker image";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        overlays = [
          (final: prev: {
            capnproto = prev.capnproto.overrideAttrs (oldAttrs: rec {
              version = "1.5.0";
              src = prev.fetchFromGitHub {
                owner = "capnproto";
                repo = "capnproto";
                rev = "v${version}";
                hash = "sha256-2J3FYwPAtbahHI1y1KMqU8Gn2YlKyIW8kZIJz2Ja31w=";
              };
            });
          })
        ];
      };

      bitcoinUid = 101;
      bitcoinGid = 101;
      bitcoinUser = "bitcoin";
      bitcoinHome = "/home/${bitcoinUser}";
      bitcoinData = "${bitcoinHome}/.bitcoin";

      zeromq = pkgs.zeromq.override {
        enableCurve = false;
        enableDrafts = false;
        libsodium = null;
      };

      capnproto-runtime =
        pkgs.runCommand "capnproto-runtime-${pkgs.capnproto.version}"
          {
            nativeBuildInputs = [ pkgs.patchelf ];
          }
          ''
            runtime_rpath='${
              pkgs.lib.makeLibraryPath [
                pkgs.stdenv.cc.cc
                pkgs.glibc
              ]
            }:$ORIGIN'

            for library in libcapnp-rpc libcapnp libkj-async libkj; do
              install -Dm755 \
                "${pkgs.capnproto}/lib/$library.so.${pkgs.capnproto.version}" \
                "$out/lib/$library.so.${pkgs.capnproto.version}"
              patchelf --set-rpath "$runtime_rpath" \
                "$out/lib/$library.so.${pkgs.capnproto.version}"
            done
          '';

      runtimeRpath = pkgs.lib.makeLibraryPath [
        capnproto-runtime
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
        src = ./.;

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

      docker-image = pkgs.dockerTools.buildLayeredImage {
        name = "bitcoin-core";
        tag = "latest";
        architecture = "amd64";
        contents = [ bitcoin-core ];
        fakeRootCommands = ''
          mkdir -p ./home/${bitcoinUser}/.bitcoin
          chmod 0755 ./home ./home/${bitcoinUser}
          chmod 0700 ./home/${bitcoinUser}/.bitcoin
          chown -R ${toString bitcoinUid}:${toString bitcoinGid} ./home/${bitcoinUser}
        '';
        config = {
          Entrypoint = [ "/bin/bitcoind" ];
          Cmd = [ "-printtoconsole" ];
          User = "${toString bitcoinUid}:${toString bitcoinGid}";
          Env = [
            "BITCOIN_DATA=${bitcoinData}"
            "HOME=${bitcoinHome}"
            "PATH=/bin"
          ];
          WorkingDir = bitcoinHome;
          Volumes.${bitcoinData} = { };
          ExposedPorts = {
            "8333/tcp" = { };
            "18333/tcp" = { };
            "18444/tcp" = { };
            "38333/tcp" = { };
            "48333/tcp" = { };
          };
          StopSignal = "SIGTERM";
        };
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
