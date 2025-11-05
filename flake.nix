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
          inherit (pkgs) lib;

          # Construct our build env
          mkPlatformStdenv =
            targetPkgs:
            if targetPkgs.stdenv.hostPlatform.isMinGW then
              targetPkgs.stdenv.override {
                cc = targetPkgs.stdenv.cc.override {
                  extraPackages = [
                    targetPkgs.windows.pthreads
                    targetPkgs.windows.mcfgthreads
                  ];
                  extraBuildCommands = ''
                    echo "-I${targetPkgs.windows.pthreads}/include" >> $out/nix-support/cc-cflags
                    echo "-L${targetPkgs.windows.pthreads}/lib -lpthread" >> $out/nix-support/cc-ldflags
                  '';
                };
              }
            else
              targetPkgs.stdenv;

          mkBitcoinDerivation =
            {
              targetPkgs,
              extraConfig ? { },
            }:
            let
              customStdEnv = mkPlatformStdenv targetPkgs;

              # Map both build directory and nix store to /usr
              baseCFlags = "-O2 -g -ffile-prefix-map=/build=/usr -fmacro-prefix-map=/build=/usr -ffile-prefix-map=${builtins.storeDir}=/usr -fmacro-prefix-map=${builtins.storeDir}=/usr";

              commonCFlags =
                if targetPkgs.stdenv.hostPlatform.isDarwin then
                  ""
                else if targetPkgs.stdenv.hostPlatform.isMinGW then
                  "${baseCFlags} -fno-ident"
                else
                  baseCFlags;

              commonCXXFlags =
                if targetPkgs.stdenv.hostPlatform.isDarwin then
                  ""
                else if targetPkgs.stdenv.hostPlatform.isMinGW then
                  "${baseCFlags} -fno-ident"
                else if targetPkgs.stdenv.hostPlatform.system == "arm-linux" then
                  "${baseCFlags} -Wno-psabi"
                else
                  baseCFlags;

              commonCPPFlags = "-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3";

              platformLdflags =
                if targetPkgs.stdenv.hostPlatform.isLinux then
                  "-Wl,--as-needed -static-libstdc++ -Wl,-O2"
                else if targetPkgs.stdenv.hostPlatform.isMinGW then
                  "-Wl,--no-insert-timestamp"
                else
                  "";

              dependsPackages = import ./depends {
                hostPkgs = pkgs;
                inherit targetPkgs customStdEnv version;
                inherit commonCFlags commonCXXFlags commonCPPFlags;
                inherit (targetPkgs) lib;
              };
            in
            customStdEnv.mkDerivation (
              {
                inherit pname version;
                src = builtins.path {
                  path = ./.;
                  name = "source";
                };
                nativeBuildInputs =
                  with pkgs;
                  [
                    cmake
                    ninja
                    pkg-config
                    python3
                  ]
                  ++ builtins.attrValues dependsPackages.native;
                buildInputs = builtins.attrValues dependsPackages.target;
                env = {
                  CMAKE_GENERATOR = "Ninja";
                  LC_ALL = "C";
                  TZ = "UTC";
                  CFLAGS = commonCFlags;
                  CXXFLAGS = commonCXXFlags;
                  LDFLAGS = platformLdflags;
                  LIBRARY_PATH = "";
                  CPATH = "";
                  C_INCLUDE_PATH = "";
                  CPLUS_INCLUDE_PATH = "";
                  OBJC_INCLUDE_PATH = "";
                  OBJCPLUS_INCLUDE_PATH = "";
                }
                // lib.optionalAttrs targetPkgs.stdenv.hostPlatform.isFreeBSD {
                  CXXFLAGS = "${commonCXXFlags} -isystem${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1 -isystem${targetPkgs.stdenv.cc.libc_dev}/include";
                  CFLAGS = "${commonCFlags} -isystem${targetPkgs.stdenv.cc.libc_dev}/include";
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
                  "-DMPGEN_EXECUTABLE=${dependsPackages.native.nativeLibmultiprocess}/bin/mpgen"
                  "-DREDUCE_EXPORTS=ON"
                  "-DBUILD_GUI_TESTS=OFF"
                  "-DBUILD_FUZZ_BINARY=OFF"
                  "-DCMAKE_SKIP_RPATH=TRUE"
                ];
                withStatic = true;
                separateDebugInfo = !targetPkgs.stdenv.hostPlatform.isDarwin; # TODO: Symbols on Darwin are different
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
          // lib.optionalAttrs pkgs.stdenv.isLinux {
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
        inherit (pkgs) lib;
        dependencies = import ./depends { inherit pkgs lib; };
      in
      {
        packages = lib.mapAttrs (_name: cfg: mkBitcoinDerivation cfg) platforms // {
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
          inherit (pkgsFor system) lib;
          # Build all platforms except Darwin (which don't cross-compile well from Linux) and default
          nonDarwinPlatforms = lib.filterAttrs (
            name: _: !lib.hasSuffix "darwin" name && name != "default"
          ) platforms;
        in
        lib.mapAttrs (_: mkBitcoinDerivation) nonDarwinPlatforms;
    };
}
