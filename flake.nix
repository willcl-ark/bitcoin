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

      dependsSources = {
        boost = pkgs.fetchurl {
          url = "https://github.com/boostorg/boost/releases/download/boost-1.88.0/boost-1.88.0-cmake.tar.gz";
          sha256 = "dcea50f40ba1ecfc448fdf886c0165cf3e525fef2c9e3e080b9804e8117b9694";
        };
        libevent = pkgs.fetchurl {
          url = "https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz";
          sha256 = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb";
        };
        sqlite = pkgs.fetchurl {
          url = "https://sqlite.org/2024/sqlite-autoconf-3460100.tar.gz";
          sha256 = "67d3fe6d268e6eaddcae3727fce58fcc8e9c53869bdd07a0c61e38ddf2965071";
        };
        zeromq = pkgs.fetchurl {
          url = "https://github.com/zeromq/libzmq/releases/download/v4.3.5/zeromq-4.3.5.tar.gz";
          sha256 = "6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43";
        };
        capnp = pkgs.fetchurl {
          url = "https://capnproto.org/capnproto-c++-1.2.0.tar.gz";
          name = "capnproto-cxx-1.2.0.tar.gz";
          sha256 = "ed00e44ecbbda5186bc78a41ba64a8dc4a861b5f8d4e822959b0144ae6fd42ef";
        };
        systemtap = pkgs.fetchurl {
          url = "https://sourceware.org/ftp/systemtap/releases/systemtap-4.8.tar.gz";
          sha256 = "cbd50a4eba5b261394dc454c12448ddec73e55e6742fda7f508f9fbc1331c223";
        };
      };

      # Platform triplet mapping
      # Nix : bitcoin core
      tripletMap = {
        "x86_64-unknown-linux-gnu" = "x86_64-pc-linux-gnu";
        "x86_64-unknown-linux-musl" = "x86_64-pc-linux-gnu";
        "aarch64-unknown-linux-gnu" = "aarch64-linux-gnu";
        "aarch64-unknown-linux-musl" = "aarch64-linux-gnu";
        "x86_64-w64-mingw32" = "x86_64-w64-mingw32";
        "x86_64-w64-windows-gnu" = "x86_64-w64-mingw32";
        "aarch64-apple-darwin" = "aarch64-apple-darwin";
        "x86_64-apple-darwin" = "x86_64-apple-darwin";
        "armv6l-unknown-linux-gnueabihf" = "arm-linux-gnueabihf";
        "riscv64-unknown-linux-gnu" = "riscv64-linux-gnu";
        "s390x-unknown-linux-gnu" = "s390x-linux-gnu";
        "x86_64-unknown-freebsd" = "x86_64-unknown-freebsd";
        "i686-unknown-linux-gnu" = "i686-pc-linux-gnu";
        "powerpc64le-unknown-linux-gnu" = "powerpc64le-linux-gnu";
      };

      # Package metadata
      pname = "bitcoin-core";
      version = "29.99.0";

      # Common native build inputs
      commonNativeBuildInputs = with pkgs; [
        autoconf
        automake
        bison
        cmake
        gnumake
        libtool
        ninja
        perl
        pkg-config
        python3
        which
        xz
      ];

      # Derivation factory
      mkBitcoinDerivation = {
        targetPkgs,
        extraConfig ? {},
      }: let
        isMinGW = targetPkgs.stdenv.hostPlatform.isMinGW;
        isFreeBSD = targetPkgs.stdenv.hostPlatform.isFreeBSD;
        triplet = tripletMap.${targetPkgs.stdenv.hostPlatform.config} or targetPkgs.stdenv.hostPlatform.config;

        customStdEnv =
          if isMinGW
          then
            # The default stdEnv for MinGW seeme to have great difficuly finding pthread headers, so we add and copy them manually
            targetPkgs.stdenv.override {
              cc = targetPkgs.stdenv.cc.override {
                extraPackages = [targetPkgs.windows.pthreads targetPkgs.windows.mcfgthreads];
                extraBuildCommands = ''
                  echo "-I${targetPkgs.windows.pthreads}/include" >> $out/nix-support/cc-cflags
                  echo "-L${targetPkgs.windows.pthreads}/lib -lpthread" >> $out/nix-support/cc-ldflags
                '';
              };
            }
          else targetPkgs.stdenv;
      in
        customStdEnv.mkDerivation ({
            inherit pname version;
            src = ./.;
            nativeBuildInputs = commonNativeBuildInputs;
            buildInputs = [];

            preConfigure = ''
              # Copy in pre-fetched depends sources
              mkdir -p depends/sources
              ${lib.concatStringsSep "\n" (lib.mapAttrsToList (
                  name: src: "cp ${src} depends/sources/${dependsSources.${name}.name or src.name}"
                )
                dependsSources)}

              # Fixup shebangs for Nix
              patchShebangs depends/gen_id

              # MinGW toolchain setup
              ${lib.optionalString isMinGW ''
                mkdir -p $PWD/bin
                for tool in gcc g++ ar nm ranlib strip ld objcopy objdump; do
                  ln -sf ${customStdEnv.cc}/bin/${triplet}-$tool $PWD/bin/${triplet}-$tool
                done
                ln -sf $PWD/bin/${triplet}-gcc $PWD/bin/${triplet}-gcc-posix
                ln -sf $PWD/bin/${triplet}-g++ $PWD/bin/${triplet}-g++-posix
                export PATH="$PWD/bin:$PATH"
                export CC_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-gcc"
                export CXX_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-g++"
                export AR_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-ar"
                export NM_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-nm"
                export RANLIB_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-ranlib"
                export STRIP_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-strip"
                export LD_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-ld"
                echo "link_directories(${targetPkgs.windows.pthreads}/lib ${targetPkgs.windows.mcfgthreads}/lib)" >> depends/${triplet}/toolchain.cmake
              ''}

              # FreeBSD toolchain setup
              ${lib.optionalString isFreeBSD ''
                mkdir -p $PWD/bin
                for tool in ar nm ranlib strip; do
                  ln -sf ${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-$tool $PWD/bin/$tool
                done
                ln -sf ${pkgs.clang}/bin/clang++ $PWD/bin/g++
                ln -sf ${pkgs.clang}/bin/clang $PWD/bin/gcc
                export PATH="$PWD/bin:$PATH"
                export CXX_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc}/bin/x86_64-unknown-freebsd-c++"
                export CC_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc}/bin/x86_64-unknown-freebsd-cc"
                export AR_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-ar"
                export NM_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-nm"
                export RANLIB_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-ranlib"
                export STRIP_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-strip"
              ''}

              # Build depends for freebsd
              ${lib.optionalString isFreeBSD ''
                make -C depends -j$NIX_BUILD_CORES HOST=${triplet} NO_QT=1 NO_QR=1 NO_USDT=1 \
                  freebsd_CXXFLAGS="-isystem ${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1 -isystem ${targetPkgs.stdenv.cc.libc_dev}/include" \
                  x86_64_freebsd_CXXFLAGS="-isystem ${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1 -isystem ${targetPkgs.stdenv.cc.libc_dev}/include" \
                  freebsd_CFLAGS="-isystem ${targetPkgs.stdenv.cc.libc_dev}/include" \
                  x86_64_freebsd_CFLAGS="-isystem ${targetPkgs.stdenv.cc.libc_dev}/include"
              ''}

              # Build depends for other platforms
              ${lib.optionalString (!isFreeBSD) ''
                make -C depends -j$NIX_BUILD_CORES HOST=${triplet} NO_QT=1 NO_QR=1 NO_USDT=1
              ''}
            '';

            cmakeFlags = ["--toolchain=depends/${triplet}/toolchain.cmake"];
            buildPhase = ''
              cmake --build . --parallel $NIX_BUILD_CORES --target bitcoind
            '';
            env = {
              CMAKE_GENERATOR = "Ninja";
              LC_ALL = "C";
              LD_LIBRARY_PATH = lib.makeLibraryPath [pkgs.stdenv.cc.cc pkgs.glibc];
            };
            withStatic = true; # This uses musl libc in most cases
            meta = {
              description = "Bitcoin Core client";
              homepage = "https://bitcoincore.org/";
              license = lib.licenses.mit;
              platforms = lib.platforms.all;
            };
          }
          // extraConfig);
    in {
      packages = let
        mkDockerImage = targetPkgs:
          pkgs.dockerTools.buildLayeredImage {
            name = pname;
            tag = "${version}-${targetPkgs.stdenv.hostPlatform.linuxArch}";
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
        default = mkBitcoinDerivation {targetPkgs = pkgs.pkgsStatic;};
        aarch64-darwin = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.aarch64-darwin.pkgsStatic;};
        aarch64-linux = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic;};
        i686-linux = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.gnu32.pkgsStatic;};
        powerpc64le-linux = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.powernv.pkgsStatic;};
        raspberryPi = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.raspberryPi.pkgsStatic;};
        riscv64 = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.riscv64.pkgsStatic;};
        s390x = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.s390x.pkgsStatic;};
        ucrt64 = mkBitcoinDerivation {
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
        x86_64-darwin = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.x86_64-darwin.pkgsStatic;};
        x86_64-freebsd = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.x86_64-freebsd.pkgsStatic;};
        x86_64-linux = mkBitcoinDerivation {targetPkgs = pkgs.pkgsCross.gnu64.pkgsStatic;};
        # Some "free" docker images
        docker-x64 = mkDockerImage pkgs.pkgsCross.gnu64.pkgsStatic;
        docker-aarch64 = mkDockerImage pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic;
      };

      devShells.default = pkgs.mkShell {
        nativeBuildInputs = commonNativeBuildInputs ++ [ pkgs.ccache ];
        buildInputs = [];
        CMAKE_GENERATOR = "Ninja";
        shellHook = ''
          echo "Bitcoin Core development shell"
        '';
      };

      formatter = pkgs.alejandra;
    });
}
