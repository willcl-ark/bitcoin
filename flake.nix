{
  description = "Bitcoin Core build via depends";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    nixpkgs,
    flake-utils,
    ...
  }:
    flake-utils.lib.eachDefaultSystem (system: let
      pkgs = import nixpkgs {inherit system;};
      lib = pkgs.lib;
      pname = "bitcoin-core";
      version = "29.99.0";
      dependencies = import ./depends {inherit pkgs lib;};

      mkBitcoinDerivation = {
        targetPkgs,
        extraConfig ? {},
      }: let
        deps = dependencies.mkDependencies targetPkgs;
        customStdEnv = dependencies.mkPlatformStdenv targetPkgs;
      in
        customStdEnv.mkDerivation ({
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
            cmakeFlags =
              [
                "-DBUILD_BITCOIN_BIN=OFF"
                "-DBUILD_DAEMON=ON"
                "-DBUILD_CLI=OFF"
                "-DBUILD_TX=OFF"
                "-DBUILD_UTIL=OFF"
                "-DBUILD_WALLET_TOOL=OFF"
                "-DBUILD_TESTS=OFF"
                "-DBUILD_BENCH=OFF"
                "-DENABLE_WALLET=OFF"
                "-DENABLE_EXTERNAL_SIGNER=OFF"
              ]
              ++ lib.optionals targetPkgs.stdenv.hostPlatform.isFreeBSD [
                ''-DCMAKE_CXX_FLAGS=-isystem${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1''
                ''-DCMAKE_C_FLAGS=-isystem${targetPkgs.stdenv.cc.libc_dev}/include''
              ];
            withStatic = true;
            separateDebugInfo = !targetPkgs.stdenv.hostPlatform.isDarwin; # Symbols on Darwin are different
            stripAllList = ["bin"]; # Use stripAll to remove everything possible, not only debug symbols
            meta = {
              description = "Bitcoin Core client";
              homepage = "https://bitcoincore.org/";
              license = lib.licenses.mit;
              platforms = lib.platforms.all;
            };
          }
          // extraConfig);
      platforms =
        {
          default = {targetPkgs = pkgs.pkgsStatic;};
          aarch64-darwin = {targetPkgs = pkgs.pkgsCross.aarch64-darwin.pkgsStatic;};
          aarch64-linux = {targetPkgs = pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic;};
          i686-linux = {targetPkgs = pkgs.pkgsCross.gnu32.pkgsStatic;};
          powerpc64le-linux = {targetPkgs = pkgs.pkgsCross.powernv.pkgsStatic;};
          raspberryPi = {targetPkgs = pkgs.pkgsCross.raspberryPi.pkgsStatic;};
          riscv64 = {targetPkgs = pkgs.pkgsCross.riscv64.pkgsStatic;};
          s390x = {targetPkgs = pkgs.pkgsCross.s390x.pkgsStatic;};
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
          x86_64-darwin = {targetPkgs = pkgs.pkgsCross.x86_64-darwin.pkgsStatic;};
          x86_64-linux = {targetPkgs = pkgs.pkgsCross.gnu64.pkgsStatic;};
        }
        // lib.optionalAttrs (pkgs.stdenv.isLinux) {
          # compat for freebsd only runs on linux
          x86_64-freebsd = {
            targetPkgs = pkgs.pkgsCross.x86_64-freebsd.pkgsStatic;
          };
        };

      mkDockerImage = {
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
            Cmd = ["/bin/bitcoind" "-printtoconsole"];
            ExposedPorts = {"8333/tcp" = {};};
            Volumes = {"/root/.bitcoin" = {};};
          };
        };
    in {
      packages =
        lib.mapAttrs (name: cfg: mkBitcoinDerivation cfg) platforms
        // {
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
        nativeBuildInputs = dependencies.commonNativeBuildInputs ++ [pkgs.ccache];
        buildInputs = [];
        CMAKE_GENERATOR = "Ninja";
        shellHook = ''
          echo "Bitcoin Core development shell"
        '';
      };

      formatter = pkgs.alejandra;
    });
}
