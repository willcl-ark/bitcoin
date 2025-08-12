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
      };

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

      pname = "bitcoin-core";
      version = "29.99.0";

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

      mkPlatformStdenv = targetPkgs:
        if targetPkgs.stdenv.hostPlatform.isMinGW
        then
          # The nix stdenv for MinGW seems to have great difficulty finding pthread headers, so we add and copy them manually
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

      # Depends expects these tools to be explicitly prefixed as below
      mkMinGWToolchainSetup = {targetPkgs, customStdEnv, triplet, sourceRoot}:
        lib.optionalString targetPkgs.stdenv.hostPlatform.isMinGW ''
          mkdir -p ${sourceRoot}/bin
          for tool in gcc g++ ar nm ranlib strip ld objcopy objdump; do
            ln -sf ${customStdEnv.cc}/bin/${triplet}-$tool ${sourceRoot}/bin/${triplet}-$tool
          done
          ln -sf ${sourceRoot}/bin/${triplet}-gcc ${sourceRoot}/bin/${triplet}-gcc-posix
          ln -sf ${sourceRoot}/bin/${triplet}-g++ ${sourceRoot}/bin/${triplet}-g++-posix
          export PATH="${sourceRoot}/bin:$PATH"
          export CC_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-gcc"
          export CXX_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-g++"
          export AR_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-ar"
          export NM_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-nm"
          export RANLIB_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-ranlib"
          export STRIP_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-strip"
          export LD_${lib.replaceStrings ["-"] ["_"] triplet}="${triplet}-ld"
          mkdir -p ${sourceRoot}/depends/${triplet}
          echo "link_directories(${targetPkgs.windows.pthreads}/lib ${targetPkgs.windows.mcfgthreads}/lib)" >> ${sourceRoot}/depends/${triplet}/toolchain.cmake
        '';

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
            version = "depends";
            src = ./.;
            nativeBuildInputs = commonNativeBuildInputs ++ [pkgs.curl];
            buildInputs = [];
            dontConfigure = true;
            dontUseCmakeConfigure = true;

            postUnpack = ''
              # Copy in the pre-fetched source for this package
              mkdir -p $sourceRoot/depends/sources
              ${lib.optionalString (dependsSources ? ${packageName}) ''
                cp ${dependsSources.${packageName}} $sourceRoot/depends/sources/${dependsSources.${packageName}.name or dependsSources.${packageName}.name}
              ''}

              # Fixup shebangs for Nix
              chmod +x $sourceRoot/depends/gen_id
              patchShebangs $sourceRoot/depends/gen_id

              # Custom toolchain setup
              # Only needed for ucrt64 and x86_64-freebsd currently
              ${mkMinGWToolchainSetup {inherit targetPkgs customStdEnv triplet; sourceRoot = "$sourceRoot";}}
            '';

            buildPhase = ''
              # Build dependency using depends system
              make -C depends -j$NIX_BUILD_CORES HOST=${triplet} ${packageName}
            '';

            installPhase = ''
              mkdir -p $out

              # Individual packages are cached as tar files, not installed to depends/${triplet}
              # We need to find and extract the cached tar file for this package
              CACHE_DIR="depends/built/${triplet}/${packageName}"

              if [ -d "$CACHE_DIR" ]; then
                echo "Found cache directory: $CACHE_DIR"
                TAR_FILE=$(find "$CACHE_DIR" -name "*.tar.gz" | head -1)

                if [ -n "$TAR_FILE" ] && [ -f "$TAR_FILE" ]; then
                  echo "Extracting cached ${packageName} from $TAR_FILE"
                  tar -xzf "$TAR_FILE" -C $out/

                  # Claude has entered the chat.
                  # The main build using depends as distinct Nix packages was failing as the cmake config files contained hardcoded absolute filepaths.
                  # These were not available in mkBitcoinDerivation as they are ephemeral to the individual package build steps, so we needed to make them relocatable.
                  # Claude took a look at this flake and a few config files and one-shotted this replacement script to use.

                  # Fix CMake configuration files to use relocatable paths
                  echo "Fixing CMake configuration files for ${packageName}"
                  for cmake_config in $(find $out -name "*.cmake" -type f); do
                    if [ -f "$cmake_config" ]; then
                      echo "Processing CMake config: $cmake_config"

                      # Replace hardcoded build paths with the current Nix store path
                      # Pattern: /build/xyz-source/depends/x86_64-pc-linux-gnu -> $out
                      sed -i "s|/build/[^/]*/depends/${triplet}|$out|g" "$cmake_config"

                      # Also handle any remaining build directory references
                      sed -i "s|/build/[^/]*-source/depends/[^/]*|$out|g" "$cmake_config"

                      # For libevent specifically, fix target import paths
                      if [[ "${packageName}" == "libevent" ]]; then
                        # Replace absolute paths with the current output directory
                        sed -i "s|IMPORTED_LOCATION_STATIC \"[^\"]*\(lib[^/]*\.a\)\"|IMPORTED_LOCATION_STATIC \"$out/lib/\1\"|g" "$cmake_config"
                        # Also fix any references to the library files directly
                        sed -i "s|\"/build/[^\"]*\(/lib/[^\"]*\)\"|\"\''${_IMPORT_PREFIX}\1\"|g" "$cmake_config"
                      fi

                      echo "Fixed paths in: $cmake_config"
                    fi
                  done
                  # Thanks Claude.

                else
                  echo "ERROR: No cached tar file found in $CACHE_DIR"
                  exit 1
                fi
              else
                echo "ERROR: Cache directory $CACHE_DIR not found!"
                exit 1
              fi
            '';

            env = {
              LC_ALL = "C";
            };
          }
          // extraConfig);

      mkDependencies = targetPkgs: {
        boost = mkDependency {
          inherit targetPkgs;
          packageName = "boost";
        };
        libevent = mkDependency {
          inherit targetPkgs;
          packageName = "libevent";
        };
        sqlite = mkDependency {
          inherit targetPkgs;
          packageName = "sqlite";
        };
        zeromq = mkDependency {
          inherit targetPkgs;
          packageName = "zeromq";
        };
      };

      mkBitcoinDerivation = {
        targetPkgs,
        extraConfig ? {},
      }: let
        dependencies = mkDependencies targetPkgs;
        customStdEnv = mkPlatformStdenv targetPkgs;
      in
        customStdEnv.mkDerivation ({
            inherit pname version;
            src = ./.;
            nativeBuildInputs = commonNativeBuildInputs;
            buildInputs = builtins.attrValues dependencies;
            buildPhase = ''
              cmake --build . --parallel $NIX_BUILD_CORES --target bitcoind
            '';
            env = {
              CMAKE_GENERATOR = "Ninja";
              LC_ALL = "C";
              LD_LIBRARY_PATH = lib.makeLibraryPath [pkgs.stdenv.cc.cc pkgs.glibc];
            };
            withStatic = true;
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

        # docker images
        docker-x64 = mkDockerImage pkgs.pkgsCross.gnu64.pkgsStatic;
        docker-aarch64 = mkDockerImage pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic;
      };

      devShells.default = pkgs.mkShell {
        nativeBuildInputs = commonNativeBuildInputs ++ [pkgs.ccache];
        buildInputs = [];
        CMAKE_GENERATOR = "Ninja";
        shellHook = ''
          echo "Bitcoin Core development shell"
        '';
      };

      formatter = pkgs.alejandra;
    });
}
