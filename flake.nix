{
  description = "Bitcoin core build derivation";

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

      mkBitcoinDerivation = targetPkgs: let
        lib = targetPkgs.lib;
        isLinux = targetPkgs.stdenv.isLinux;
        isDarwin = targetPkgs.stdenv.isDarwin;
        isMinGW = targetPkgs.stdenv.hostPlatform.isMinGW;
        hostPlatform = targetPkgs.stdenv.hostPlatform.config;

        nixStorePrefix = "/nix/store";
        filePrefixMap = "-ffile-prefix-map=${nixStorePrefix}=/usr";
        baseCFlags = "-O2 -g ${filePrefixMap}";
        platformCFlags =
          baseCFlags
          + (
            if isMinGW
            then " -fno-ident"
            else ""
          );
        platformCXXFlags =
          platformCFlags
          + (
            if hostPlatform == "arm-linux-gnueabihf"
            then " -Wno-psabi"
            else ""
          );
      in
        targetPkgs.stdenv.mkDerivation {
          pname = "bitcoin-core";
          version = "29.99.0";
          src = ./.;

          nativeBuildInputs = with pkgs;
            [
              bison
              cmake
              curlMinimal
              ninja
              pkg-config
              xz
            ]
            ++ lib.optionals (system == "x86_64-linux") [libsystemtap linuxPackages.bcc linuxPackages.bpftrace];

          buildInputs = with targetPkgs;
            [
              boost
              capnproto
              libevent
              sqlite.dev
              zeromq
            ]
            ++ lib.optionals (isLinux && targetPkgs.stdenv.hostPlatform.system == "x86_64-linux") [libsystemtap linuxPackages.bcc linuxPackages.bpftrace];

          cmakeFlags = ["-DBUILD_GUI=OFF" "-DBUILD_TESTS=OFF" "-DBUILD_BENCH=OFF"];

          buildPhase = ''
            runHook preBuild
            cmake --build . --parallel $NIX_BUILD_CORES --target bitcoind
            runHook postBuild
          '';

          env = {
            CMAKE_GENERATOR = "Ninja";
            LD_LIBRARY_PATH = lib.makeLibraryPath [targetPkgs.capnproto];
            LC_ALL = "C";
            CFLAGS = lib.optionalString (!isDarwin) platformCFlags;
            CXXFLAGS = lib.optionalString (!isDarwin) platformCXXFlags;
            LDFLAGS = lib.concatStringsSep " " (
              lib.optionals isLinux ["-Wl,--as-needed" "-Wl,-O2"]
              ++ lib.optionals isMinGW ["-Wl,--no-insert-timestamp"]
              ++ lib.optionals isDarwin ["-Wl,-platform_version,macos,13.0,14.0" "-Wl,-no_adhoc_codesign"]
            );
          };

          meta = with lib; {
            description = "Bitcoin Core client";
            homepage = "https://bitcoincore.org/";
            license = licenses.mit;
            platforms = platforms.all;
          };
        };
    in {
      packages = {
        default = mkBitcoinDerivation pkgs;
        aarch64-linux = mkBitcoinDerivation pkgs.pkgsCross.aarch64-multiplatform;
        aarch64-darwin = mkBitcoinDerivation pkgs.pkgsCross.aarch64-darwin;
        # Currently failing on zeromq builds, no `WINVER` set?
        # x86_64-windows = mkBitcoinDerivation pkgs.pkgsCross.mingwW64;
      };

      devShells.default = let
        defaultDerivation = mkBitcoinDerivation pkgs;
      in
        pkgs.mkShell {
          nativeBuildInputs = defaultDerivation.nativeBuildInputs;
          buildInputs = defaultDerivation.buildInputs;
          CMAKE_GENERATOR = "Ninja";
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [pkgs.capnproto];
          shellHook = ''
            echo "Bitcoin Core development environment"
          '';
        };

      formatter = pkgs.alejandra;
    });
}
