{
  pkgs,
  lib,
}: let
  # Individual cached source derivations - each tarball is cached separately in the nix store for re-use.
  # Let Nix garbage collection etc. manage their lifetime like any other package.
  dependsSources = {
    boost = {
      version = "1.88.0";
      tarball = pkgs.fetchurl {
        url = "https://github.com/boostorg/boost/releases/download/boost-1.88.0/boost-1.88.0-cmake.tar.gz";
        sha256 = "dcea50f40ba1ecfc448fdf886c0165cf3e525fef2c9e3e080b9804e8117b9694";
        name = "boost-1.88.0-cmake.tar.gz";
      };
    };
    libevent = {
      version = "2.1.12-stable";
      tarball = pkgs.fetchurl {
        url = "https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz";
        sha256 = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb";
        name = "libevent-2.1.12-stable.tar.gz";
      };
    };
    sqlite = {
      version = "3460100";
      tarball = pkgs.fetchurl {
        url = "https://sqlite.org/2024/sqlite-autoconf-3460100.tar.gz";
        sha256 = "67d3fe6d268e6eaddcae3727fce58fcc8e9c53869bdd07a0c61e38ddf2965071";
        name = "sqlite-autoconf-3460100.tar.gz";
      };
    };
    zeromq = {
      version = "4.3.5";
      tarball = pkgs.fetchurl {
        url = "https://github.com/zeromq/libzmq/releases/download/v4.3.5/zeromq-4.3.5.tar.gz";
        sha256 = "6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43";
        name = "zeromq-4.3.5.tar.gz";
      };
    };
  };

  # Triplet mapping for cross-compilation
  tripletMap = {
    # Format: <Nix triplet> = <bitcoin depends triplet>
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

  # Common native build inputs needed for all dependencies
  commonNativeBuildInputs = with pkgs; [
    # depends
    autoconf
    automake
    bison
    cmake
    gnumake
    libtool
    patch
    perl
    which
    xz
  ];

  # Platform-specific stdenv setup
  mkPlatformStdenv = targetPkgs:
    if targetPkgs.stdenv.hostPlatform.isMinGW
    then
      # The nix stdenv for MinGW seems to have great difficulty finding pthread headers, so add and copy them manually
      # TODO: fix this
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

  # Custom toolchain setup. Only needed for minGW currently
  setupCustomToolchain = {
    targetPkgs,
    customStdEnv,
    triplet,
    sourceRoot,
  }:
    lib.optionalString targetPkgs.stdenv.hostPlatform.isMinGW ''
      # Add Windows threading library directories to CMake configuration
      mkdir -p ${sourceRoot}/${triplet}
      echo "link_directories(${targetPkgs.windows.pthreads}/lib ${targetPkgs.windows.mcfgthreads}/lib)" >> ${sourceRoot}/${triplet}/toolchain.cmake
    '';

  # Build a single dependency package
  mkDependency = {
    targetPkgs,
    packageName,
    extraConfig ? {},
  }: let
    triplet = tripletMap.${targetPkgs.stdenv.hostPlatform.config} or targetPkgs.stdenv.hostPlatform.config;
    customStdEnv = mkPlatformStdenv targetPkgs;
  in
    customStdEnv.mkDerivation ({
        pname = packageName;
        version = dependsSources.${packageName}.version;
        src = builtins.path {
          path = ./.;
          name = "depends-source";
        };
        nativeBuildInputs = commonNativeBuildInputs ++ [pkgs.curl];
        buildInputs = [];
        dontConfigure = true;
        dontUseCmakeConfigure = true;

        postUnpack = ''
          mkdir -p $sourceRoot/sources
          ${lib.optionalString (dependsSources ? ${packageName}) ''
            # Link cached source from Nix store instead of downloading
            ln -s ${dependsSources.${packageName}.tarball} $sourceRoot/sources/${dependsSources.${packageName}.tarball.name}
          ''}

          # Copy patches to expected location for depends build system
          mkdir -p $sourceRoot/patches
          cp -r ${./patches}/* $sourceRoot/patches/

          # Fixup shebangs for Nix
          chmod +x $sourceRoot/gen_id
          patchShebangs $sourceRoot/gen_id

          ${setupCustomToolchain {
            inherit targetPkgs customStdEnv triplet;
            sourceRoot = "$sourceRoot";
          }}
        '';

        buildPhase = ''
          ${lib.optionalString targetPkgs.stdenv.hostPlatform.isFreeBSD ''
            export freebsd_CXXFLAGS="-isystem ${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1 -isystem ${targetPkgs.stdenv.cc.libc_dev}/include"
            export x86_64_freebsd_CXXFLAGS="-isystem ${targetPkgs.llvmPackages.libcxx.dev}/include/c++/v1 -isystem ${targetPkgs.stdenv.cc.libc_dev}/include"
            export freebsd_CFLAGS="-isystem ${targetPkgs.stdenv.cc.libc_dev}/include"
            export x86_64_freebsd_CFLAGS="-isystem ${targetPkgs.stdenv.cc.libc_dev}/include"
          ''}
          make -j$NIX_BUILD_CORES HOST=${triplet} ${packageName}
        '';

        installPhase = ''
          mkdir -p $out

          # Individual packages are cached as tar files, not installed to depends/${triplet}
          # We need to find and extract the cached tar file for this package
          CACHE_DIR="built/${triplet}/${packageName}"

          # Find the tar file
          TAR_FILE=$(find "$CACHE_DIR" -name "*.tar.gz" | head -1)
          [ -f "$TAR_FILE" ] || {
            echo "ERROR: No cached tar file found in $CACHE_DIR"
            echo "Expected to find a .tar.gz file for package '${packageName}'"
            exit 1
          }

          echo "Extracting cached ${packageName} from $TAR_FILE"
          tar -xzf "$TAR_FILE" -C $out/

          # Fix CMake configuration files to make them relocatable
          echo "Fixing CMake configuration files for ${packageName}"
          for cmake_config in $(find $out -name "*.cmake" -type f); do
            if [ -f "$cmake_config" ]; then
              echo "Processing CMake config: $cmake_config"

              # Replace hardcoded build paths with the current Nix store path
              # Handle various temporary path formats on different platforms, currently linux and macOS
              sed -i "s|/build/[^/]*/depends/${triplet}|$out|g" "$cmake_config"
              sed -i "s|/build/[^/]*-source/depends/[^/]*|$out|g" "$cmake_config"
              sed -i "s|/tmp/nix-build-[^/]*/source/depends/${triplet}|$out|g" "$cmake_config"
              sed -i "s|/private/tmp/nix-build-[^/]*/source/depends/${triplet}|$out|g" "$cmake_config"
              sed -i "s|set(_IMPORT_PREFIX \"[^\"]*\")|set(_IMPORT_PREFIX \"$out\")|g" "$cmake_config"

              # For libevent specifically, fix target import paths
              if [[ "${packageName}" == "libevent" ]]; then
                sed -i "s|IMPORTED_LOCATION_STATIC \"[^\"]*\(lib[^/]*\.a\)\"|IMPORTED_LOCATION_STATIC \"$out/lib/\1\"|g" "$cmake_config"
                sed -i "s|IMPORTED_LOCATION_NONE \"[^\"]*\(lib[^/]*\.a\)\"|IMPORTED_LOCATION_NONE \"\''${_IMPORT_PREFIX}/lib/\1\"|g" "$cmake_config"
              fi

              echo "Fixed paths in: $cmake_config"
            fi
          done
        '';

        env = {
          LC_ALL = "C";
        };
      }
      // extraConfig);

  # Build all dependencies for a target platform
  mkDependencies = targetPkgs:
    lib.mapAttrs (name: _:
      mkDependency {
        inherit targetPkgs;
        packageName = name;
      })
    dependsSources;
in {
  inherit
    dependsSources
    tripletMap
    commonNativeBuildInputs
    mkPlatformStdenv
    setupCustomToolchain
    mkDependency
    mkDependencies
    ;
}
