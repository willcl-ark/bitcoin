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

      dependsSources = {
        boost = pkgs.fetchurl {
          url = "https://github.com/boostorg/boost/releases/download/boost-1.88.0/boost-1.88.0-cmake.tar.gz";
          sha256 = "dcea50f40ba1ecfc448fdf886c0165cf3e525fef2c9e3e080b9804e8117b9694";
        };
        libevent = pkgs.fetchurl {
          url = "https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz";
          sha256 = "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb";
        };
      };

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

      commonNativeBuildInputs = with pkgs; [
        # depends
        autoconf
        automake
        bison
        gnumake
        libtool
        patch
        perl
        which
        xz
        # primary
        cmake
        ninja
        pkg-config
        python3
      ];

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
              mkdir -p $sourceRoot/depends/sources
              ${lib.optionalString (dependsSources ? ${packageName}) ''
                cp ${dependsSources.${packageName}} $sourceRoot/depends/sources/${dependsSources.${packageName}.name or dependsSources.${packageName}.name}
              ''}

              # Fixup shebangs for Nix
              chmod +x $sourceRoot/depends/gen_id
              patchShebangs $sourceRoot/depends/gen_id

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
              make -C depends -j$NIX_BUILD_CORES HOST=${triplet} ${packageName}
            '';

            installPhase = ''
              mkdir -p $out

              # Individual packages are cached as tar files, not installed to depends/${triplet}
              # We need to find and extract the cached tar file for this package
              CACHE_DIR="depends/built/${triplet}/${packageName}"

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
                  sed -i "s|/build/[^/]*/depends/${triplet}|$out|g" "$cmake_config"
                  sed -i "s|/build/[^/]*-source/depends/[^/]*|$out|g" "$cmake_config"

                  # For libevent specifically, fix target import paths
                  if [[ "${packageName}" == "libevent" ]]; then
                    sed -i "s|IMPORTED_LOCATION_STATIC \"[^\"]*\(lib[^/]*\.a\)\"|IMPORTED_LOCATION_STATIC \"$out/lib/\1\"|g" "$cmake_config"
                    sed -i "s|\"/build/[^\"]*\(/lib/[^\"]*\)\"|\"\''${_IMPORT_PREFIX}\1\"|g" "$cmake_config"
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

      mkDependencies = targetPkgs:
        lib.mapAttrs (name: _:
          mkDependency {
            inherit targetPkgs;
            packageName = name;
          })
        dependsSources;

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
            meta = {
              description = "Bitcoin Core client";
              homepage = "https://bitcoincore.org/";
              license = lib.licenses.mit;
              platforms = lib.platforms.all;
            };
          }
          // extraConfig);
      platforms = {
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
        x86_64-freebsd = {targetPkgs = pkgs.pkgsCross.x86_64-freebsd.pkgsStatic;};
        x86_64-linux = {targetPkgs = pkgs.pkgsCross.gnu64.pkgsStatic;};
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
