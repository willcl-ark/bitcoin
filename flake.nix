{
  description = "Bitcoin core build derivation with depends";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    nixpkgs,
    flake-utils,
    ...
  }:
    flake-utils.lib.eachDefaultSystem (system: let
      pkgs = import nixpkgs {inherit system;};

      # Define all depends sources with their hashes
      # These match the versions (currently) in depends/packages/*.mk
      dependsSources = {
        boost = {
          urlPrefix = "https://github.com/boostorg/boost/releases/download/boost-1.88.0";
          file = "boost-1.88.0-cmake.tar.gz";
          sha256 = "dcea50f40ba1ecfc448fdf886c0165cf3e525fef2c9e3e080b9804e8117b9694";
        };
        libevent = {
          urlPrefix = "https://github.com/libevent/libevent/releases/download/release-2.1.12-stable";
          file = "libevent-2.1.12-stable.tar.gz";
          sha256 = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb";
        };
        sqlite = {
          urlPrefix = "https://sqlite.org/2024";
          file = "sqlite-autoconf-3460100.tar.gz";
          sha256 = "67d3fe6d268e6eaddcae3727fce58fcc8e9c53869bdd07a0c61e38ddf2965071";
        };
        zeromq = {
          urlPrefix = "https://github.com/zeromq/libzmq/releases/download/v4.3.5";
          file = "zeromq-4.3.5.tar.gz";
          sha256 = "6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43";
        };
        capnp = {
          urlPrefix = "https://capnproto.org";
          file = "capnproto-c++-1.2.0.tar.gz";
          name = "capnproto-cxx-1.2.0.tar.gz"; # Saved with different name
          sha256 = "ed00e44ecbbda5186bc78a41ba64a8dc4a861b5f8d4e822959b0144ae6fd42ef";
        };
        systemtap = {
          urlPrefix = "https://sourceware.org/ftp/systemtap/releases";
          file = "systemtap-4.8.tar.gz";
          sha256 = "cbd50a4eba5b261394dc454c12448ddec73e55e6742fda7f508f9fbc1331c223";
        };
        # # Qt and X11 dependencies (only needed if building with GUI)
        # expat = {
        #   urlPrefix = "https://github.com/libexpat/libexpat/releases/download/R_2_4_8";
        #   file = "expat-2.4.8.tar.xz";
        #   sha256 = "f79b8f904b749e3e0d20afeadecf8249c55b2e32d4ebb089ae378df479dcaf25";
        # };
        # xcb_proto = {
        #   urlPrefix = "https://xorg.freedesktop.org/archive/individual/proto";
        #   file = "xcb-proto-1.15.2.tar.xz";
        #   sha256 = "7072beb1f680a2fe3f9e535b797c146d22528990c72f63ddb49d2f350a3653ed";
        # };
        # xproto = {
        #   urlPrefix = "https://xorg.freedesktop.org/releases/individual/proto";
        #   file = "xproto-7.0.31.tar.gz";
        #   sha256 = "6d755eaae27b45c5cc75529a12855fed5de5969b367ed05003944cf901ed43c7";
        # };
        # libXau = {
        #   urlPrefix = "https://xorg.freedesktop.org/releases/individual/lib";
        #   file = "libXau-1.0.9.tar.gz";
        #   sha256 = "1f123d8304b082ad63a9e89376400a3b1d4c29e67e3ea07b3f659cccca690eea";
        # };
        # libxcb = {
        #   urlPrefix = "https://xcb.freedesktop.org/dist";
        #   file = "libxcb-1.14.tar.xz";
        #   sha256 = "a55ed6db98d43469801262d81dc2572ed124edc3db31059d4e9916eb9f844c34";
        # };
        # libxcb_util = {
        #   urlPrefix = "https://xcb.freedesktop.org/dist";
        #   file = "xcb-util-0.4.0.tar.gz";
        #   sha256 = "0ed0934e2ef4ddff53fcc70fc64fb16fe766cd41ee00330312e20a985fd927a7";
        # };
        # libxcb_util_cursor = {
        #   urlPrefix = "https://xcb.freedesktop.org/dist";
        #   file = "xcb-util-cursor-0.1.5.tar.gz";
        #   sha256 = "0e9c5446dc6f3beb8af6ebfcc9e27bcc6da6fe2860f7fc07b99144dfa568e93b";
        # };
        # libxcb_util_image = {
        #   urlPrefix = "https://xcb.freedesktop.org/dist";
        #   file = "xcb-util-image-0.4.0.tar.gz";
        #   sha256 = "cb2c86190cf6216260b7357a57d9100811bb6f78c24576a3a5bfef6ad3740a42";
        # };
        # libxcb_util_keysyms = {
        #   urlPrefix = "https://xcb.freedesktop.org/dist";
        #   file = "xcb-util-keysyms-0.4.0.tar.gz";
        #   sha256 = "0807cf078fbe38489a41d755095c58239e1b67299f14460dec2ec811e96caa96";
        # };
        # libxcb_util_render = {
        #   urlPrefix = "https://xcb.freedesktop.org/dist";
        #   file = "xcb-util-renderutil-0.3.9.tar.gz";
        #   sha256 = "55eee797e3214fe39d0f3f4d9448cc53cffe06706d108824ea37bb79fcedcad5";
        # };
        # libxcb_util_wm = {
        #   urlPrefix = "https://xcb.freedesktop.org/dist";
        #   file = "xcb-util-wm-0.4.1.tar.gz";
        #   sha256 = "038b39c4bdc04a792d62d163ba7908f4bb3373057208c07110be73c1b04b8334";
        # };
        # libxkbcommon = {
        #   urlPrefix = "https://xkbcommon.org/download";
        #   file = "libxkbcommon-0.8.4.tar.xz";
        #   sha256 = "60ddcff932b7fd352752d51a5c4f04f3d0403230a584df9a2e0d5ed87c486c8b";
        # };
        # freetype = {
        #   urlPrefix = "https://download.savannah.gnu.org/releases/freetype";
        #   file = "freetype-2.11.0.tar.xz";
        #   sha256 = "8bee39bd3968c4804b70614a0a3ad597299ad0e824bc8aad5ce8aaf48067bde7";
        # };
        # fontconfig = {
        #   urlPrefix = "https://www.freedesktop.org/software/fontconfig/release";
        #   file = "fontconfig-2.12.6.tar.gz";
        #   sha256 = "064b9ebf060c9e77011733ac9dc0e2ce92870b574cca2405e11f5353a683c334";
        # };
        # qrencode = {
        #   urlPrefix = "https://fukuchi.org/works/qrencode";
        #   file = "qrencode-4.1.1.tar.gz";
        #   sha256 = "da448ed4f52aba6bcb0cd48cac0dd51b8692bccc4cd127431402fca6f8171e8e";
        # };
      };

      # Helper to fetch a single source
      mkFetchSource = {
        urlPrefix,
        file,
        sha256,
        ...
      }:
        pkgs.fetchurl {
          url = "${urlPrefix}/${file}";
          inherit sha256;
        };

      # Fetch all sources
      fetchedSources = pkgs.lib.mapAttrs (name: value: mkFetchSource value) dependsSources;

      mkBitcoinDerivation = targetPkgs: let
        lib = targetPkgs.lib;
        isMinGW = targetPkgs.stdenv.hostPlatform.isMinGW;

        # Map Nix target platforms to depends triplets
        getTriplet = stdenv:
          if stdenv.hostPlatform.config == "x86_64-unknown-linux-gnu"
          then "x86_64-pc-linux-gnu"
          else if stdenv.hostPlatform.config == "x86_64-unknown-linux-musl"
          then "x86_64-pc-linux-gnu" # Use gnu triplet for musl too
          else if stdenv.hostPlatform.config == "aarch64-unknown-linux-gnu"
          then "aarch64-linux-gnu"
          else if stdenv.hostPlatform.config == "aarch64-unknown-linux-musl"
          then "aarch64-linux-gnu"
          else if stdenv.hostPlatform.config == "x86_64-w64-mingw32"
          then "x86_64-w64-mingw32"
          else if stdenv.hostPlatform.config == "aarch64-apple-darwin"
          then "aarch64-apple-darwin"
          else if stdenv.hostPlatform.config == "x86_64-apple-darwin"
          then "x86_64-apple-darwin"
          else if stdenv.hostPlatform.config == "armv6l-unknown-linux-gnueabihf"
          then "arm-linux-gnueabihf"
          else if stdenv.hostPlatform.config == "riscv64-unknown-linux-gnu"
          then "riscv64-linux-gnu"
          else if stdenv.hostPlatform.config == "s390x-unknown-linux-gnu"
          then "s390x-linux-gnu"
          else if stdenv.hostPlatform.config == "x86_64-unknown-freebsd"
          then "x86_64-unknown-freebsd"
          else stdenv.hostPlatform.config;

        triplet = getTriplet targetPkgs.stdenv;
      in
        targetPkgs.stdenv.mkDerivation {
          pname = "bitcoin-core";
          version = "29.99.0";
          src = ./.;

          nativeBuildInputs = with pkgs;
            [
              # Build tools needed by depends and cmake
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
              which # Needed by depends!
              xz
            ]
            ++ lib.optionals isMinGW [
              pkgs.pkgsCross.mingwW64.stdenv.cc
            ];

          # No runtime dependencies - they're all built by depends!
          buildInputs = [];

          preConfigure = ''
            echo "Building dependencies with depends for triplet: ${triplet}"

            # Make scripts executable
            chmod +x depends/gen_id
            chmod +x depends/config.guess
            chmod +x depends/config.sub

            # Fix gen_id shebang for NixOS
            patchShebangs depends/gen_id

            # Create sources directory and copy pre-fetched sources
            mkdir -p depends/sources

            # Copy all fetched sources with proper naming
            ${lib.concatStringsSep "\n" (lib.mapAttrsToList (
                name: source: let
                  srcInfo = dependsSources.${name};
                  targetName = srcInfo.name or srcInfo.file;
                in "cp ${source} depends/sources/${lib.escapeShellArg targetName}"
              )
              fetchedSources)}

            # =======================
            # All lines until the following "====" are specific to freebsd workarounds.
            # I don't like how clunky these are, and not sure they are worth keeping.
            # But for now, here they are...

            ${lib.optionalString (triplet == "x86_64-unknown-freebsd") ''
              mkdir -p $PWD/bin

              # The llvm-binutils are all prefixed with x86_64-unknown-freebsd-
              # but the depends build system expects unprefixed names for native builds
              ln -sf ${pkgs.pkgsCross.x86_64-freebsd.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-ar $PWD/bin/ar
              ln -sf ${pkgs.pkgsCross.x86_64-freebsd.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-nm $PWD/bin/nm
              ln -sf ${pkgs.pkgsCross.x86_64-freebsd.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-ranlib $PWD/bin/ranlib
              ln -sf ${pkgs.pkgsCross.x86_64-freebsd.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-strip $PWD/bin/strip

              # For native builds, we need g++ and gcc to point to clang
              ln -sf ${pkgs.clang}/bin/clang++ $PWD/bin/g++
              ln -sf ${pkgs.clang}/bin/clang $PWD/bin/gcc

              export PATH="$PWD/bin:$PATH"

              # Also set up the cross-compilation environment variables
              export CXX_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc}/bin/x86_64-unknown-freebsd-c++"
              export CC_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc}/bin/x86_64-unknown-freebsd-cc"
              export AR_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-ar"
              export NM_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-nm"
              export RANLIB_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-ranlib"
              export STRIP_x86_64_unknown_freebsd="${targetPkgs.stdenv.cc.bintools}/bin/x86_64-unknown-freebsd-strip"
            ''}

            # Build depends
            ${lib.optionalString (triplet == "x86_64-unknown-freebsd") ''
              # Pass FreeBSD-specific flags as Make overrides to bypass the host file definitions
              make -C depends -j$NIX_BUILD_CORES HOST=${triplet} NO_QT=1 NO_QR=1 NO_WALLET=0 NO_ZMQ=0 NO_USDT=1 MULTIPROCESS=0 freebsd_CXXFLAGS="-isystem ${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1 -isystem ${targetPkgs.stdenv.cc.libc_dev}/include" x86_64_freebsd_CXXFLAGS="-isystem ${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1 -isystem ${targetPkgs.stdenv.cc.libc_dev}/include" freebsd_CFLAGS="-isystem ${targetPkgs.stdenv.cc.libc_dev}/include" x86_64_freebsd_CFLAGS="-isystem ${targetPkgs.stdenv.cc.libc_dev}/include"
            ''}

            # =======================

            ${lib.optionalString (triplet != "x86_64-unknown-freebsd") ''
              make -C depends -j$NIX_BUILD_CORES HOST=${triplet} NO_QT=1 NO_QR=1 NO_WALLET=0 NO_ZMQ=0 NO_USDT=1 MULTIPROCESS=0
            ''}
          '';

          cmakeFlags = [
            "--toolchain=depends/${triplet}/toolchain.cmake"
          ];

          buildPhase = ''
            runHook preBuild
            cmake --build . --parallel $NIX_BUILD_CORES --target bitcoind
            runHook postBuild
          '';

          env = {
            CMAKE_GENERATOR = "Ninja";
            LC_ALL = "C";
          };

          withStatic = true;

          meta = with lib; {
            description = "Bitcoin Core client";
            homepage = "https://bitcoincore.org/";
            license = licenses.mit;
            platforms = platforms.all;
          };
        };
    in {
      # Here we use a combination of `pkgsCross` and `pkgsStatic` to get static libs and cross-compilers as necessary
      packages = {
        default = mkBitcoinDerivation pkgs.pkgsStatic;
        aarch64-darwin = mkBitcoinDerivation pkgs.pkgsCross.aarch64-darwin.pkgsStatic; # Can only compile from darwin currently
        aarch64-linux = mkBitcoinDerivation pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic;
        raspberryPi = mkBitcoinDerivation pkgs.pkgsCross.raspberryPi.pkgsStatic;
        riscv64 = mkBitcoinDerivation pkgs.pkgsCross.riscv64.pkgsStatic;
        s390x = mkBitcoinDerivation pkgs.pkgsCross.s390x.pkgsStatic;
        x86_64-freebsd = mkBitcoinDerivation pkgs.pkgsCross.x86_64-freebsd.pkgsStatic;
        x86_64-windows = mkBitcoinDerivation pkgs.pkgsCross.mingwW64.pkgsStatic;
      };

      devShells.default = let
        defaultDerivation = mkBitcoinDerivation pkgs;
      in
        pkgs.mkShell {
          nativeBuildInputs = defaultDerivation.nativeBuildInputs;
          buildInputs = defaultDerivation.buildInputs;
          CMAKE_GENERATOR = "Ninja";
          shellHook = ''
            echo "Bitcoin Core development environment with depends build system"
          '';
        };

      formatter = pkgs.alejandra;
    });
}
