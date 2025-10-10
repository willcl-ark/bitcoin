{
  description = "Bitcoin Core build via depends";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      ...
    }:
    let
      # Return pkgs for a system
      pkgsFor = system: import nixpkgs { inherit system; };

      pname = "bitcoin-core";
      version = "30.99.0";

      mkBitcoinForSystem =
        system:
        let
          pkgs = pkgsFor system;
          lib = pkgs.lib;
          dependencies = import ./depends { inherit pkgs lib; };

          mkBitcoinDerivation =
            {
              targetPkgs,
              extraConfig ? { },
            }:
            let
              deps = dependencies.mkDependencies targetPkgs;
              customStdEnv = dependencies.mkPlatformStdenv targetPkgs;
            in
            customStdEnv.mkDerivation (
              {
                inherit pname version;
                src = builtins.path {
                  path = ./.;
                  name = "source";
                };
                nativeBuildInputs = with pkgs; [
                  cmake
                  ninja
                  pkg-config
                  python3
                ];
                buildInputs = builtins.attrValues deps;
                env = {
                  CMAKE_GENERATOR = "Ninja";
                  LC_ALL = "C";
                };
                cmakeFlags = [
                  "-DBUILD_BITCOIN_BIN=ON"
                  "-DBUILD_DAEMON=ON"
                  "-DBUILD_CLI=ON"
                  "-DBUILD_TX=ON"
                  "-DBUILD_UTIL=ON"
                  "-DBUILD_WALLET_TOOL=ON"
                  "-DBUILD_TESTS=ON"
                  "-DBUILD_BENCH=OFF"
                  "-DENABLE_WALLET=ON"
                  "-DENABLE_EXTERNAL_SIGNER=OFF"
                  "-DWITH_ZMQ=ON"
                  "-DENABLE_IPC=ON"
                ]
                ++ lib.optionals targetPkgs.stdenv.hostPlatform.isFreeBSD [
                  ''-DCMAKE_CXX_FLAGS=-isystem${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1''
                  ''-DCMAKE_C_FLAGS=-isystem${targetPkgs.stdenv.cc.libc_dev}/include''
                ];
                withStatic = true;
                separateDebugInfo = !targetPkgs.stdenv.hostPlatform.isDarwin; # Symbols on Darwin are different
                stripAllList = [ "bin" ]; # Use stripAll to remove everything possible, not only debug symbols
                meta = {
                  description = "Bitcoin Core client";
                  homepage = "https://bitcoincore.org/";
                  license = lib.licenses.mit;
                  platforms = lib.platforms.all;
                };
              }
              // extraConfig
            );

          platforms = {
            default = {
              targetPkgs = pkgs.pkgsStatic;
            };
            aarch64-darwin = {
              targetPkgs = pkgs.pkgsCross.aarch64-darwin.pkgsStatic;
            };
            aarch64-linux = {
              targetPkgs = pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic;
            };
            i686-linux = {
              targetPkgs = pkgs.pkgsCross.gnu32.pkgsStatic;
            };
            powerpc64le-linux = {
              targetPkgs = pkgs.pkgsCross.powernv.pkgsStatic;
            };
            raspberryPi = {
              targetPkgs = pkgs.pkgsCross.raspberryPi.pkgsStatic;
            };
            riscv64 = {
              targetPkgs = pkgs.pkgsCross.riscv64.pkgsStatic;
            };
            s390x = {
              targetPkgs = pkgs.pkgsCross.s390x.pkgsStatic;
            };
            ucrt64 = {
              targetPkgs = import nixpkgs {
                inherit system;
                crossSystem = {
                  config = "x86_64-w64-mingw32";
                  libc = "ucrt";
                  threadsCross = {
                    model = "posix";
                    package = pkgs.pkgsCross.mingwW64.threads;
                  };
                };
              };
            };
            x86_64-darwin = {
              targetPkgs = pkgs.pkgsCross.x86_64-darwin.pkgsStatic;
            };
            x86_64-linux = {
              targetPkgs = pkgs.pkgsCross.gnu64.pkgsStatic;
            };
          }
          // lib.optionalAttrs (pkgs.stdenv.isLinux) {
            # compat for freebsd only runs on linux
            x86_64-freebsd = {
              targetPkgs = pkgs.pkgsCross.x86_64-freebsd.pkgsStatic;
            };
          };

          mkDockerImage =
            {
              targetPkgs,
              arch,
            }:
            pkgs.dockerTools.buildLayeredImage {
              name = pname;
              tag = "${version}-${arch}";
              contents = [
                (mkBitcoinDerivation {
                  inherit targetPkgs;
                  extraConfig = {
                    installPhase = ''
                      mkdir -p $out/bin
                      cp bin/bitcoind $out/bin/
                    '';
                  };
                })
                pkgs.bash
                pkgs.coreutils
              ];
              config = {
                Cmd = [
                  "/bin/bitcoind"
                  "-printtoconsole"
                ];
                ExposedPorts = {
                  "8333/tcp" = { };
                };
                Volumes = {
                  "/root/.bitcoin" = { };
                };
              };
            };
        in
        {
          inherit mkBitcoinDerivation platforms mkDockerImage;
        };
    in
    (flake-utils.lib.eachDefaultSystem (
      system:
      let
        inherit (mkBitcoinForSystem system) mkBitcoinDerivation platforms mkDockerImage;
        pkgs = pkgsFor system;
        lib = pkgs.lib;
        dependencies = import ./depends { inherit pkgs lib; };
      in
      {
        packages = lib.mapAttrs (name: cfg: mkBitcoinDerivation cfg) platforms // {
          docker-x64 = mkDockerImage {
            targetPkgs = pkgs.pkgsCross.gnu64.pkgsStatic;
            arch = "x86_64";
          };
          docker-aarch64 = mkDockerImage {
            targetPkgs = pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic;
            arch = "aarch64";
          };
        };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = dependencies.commonNativeBuildInputs ++ [ pkgs.ccache ];
          buildInputs = [ ];
          CMAKE_GENERATOR = "Ninja";
          shellHook = ''
            echo "Bitcoin Core development shell"
          '';
        };

        formatter = pkgs.nixfmt-tree;
      }
    ))
    // {
      hydraJobs =
        let
          system = "x86_64-linux";
          inherit (mkBitcoinForSystem system) mkBitcoinDerivation platforms;
          lib = (pkgsFor system).lib;
          # Build all platforms except Darwin (which don't cross-compile well from Linux) and default
          # TODO: Does this actually work on hydra though?
          nonDarwinPlatforms = lib.filterAttrs (
            name: _: !lib.hasSuffix "darwin" name && name != "default"
          ) platforms;
        in
        lib.mapAttrs (_: platformConfig: mkBitcoinDerivation platformConfig) nonDarwinPlatforms;
    };
}
