{
  description = "Bitcoin Core";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forEachSystem =
        function:
        nixpkgs.lib.genAttrs systems (
          system:
          function {
            pkgs = import nixpkgs { inherit system; };
          }
        );
      buildTools =
        pkgs: with pkgs; [
          cmake
          ninja
          pkg-config
          python3
        ];
      commonLibraries =
        pkgs: with pkgs; [
          boost
          capnproto
          sqlite
          zeromq
          libsodium # For test_bitcoin. ZeroMQ's exported CMake target links libsodium directly.
        ];
      devShellPackages = pkgs: buildTools pkgs ++ commonLibraries pkgs;
      functionalTestTools = pkgs: [
        pkgs.capnproto
        pkgs.python3
        pkgs.python3Packages.pycapnp
        pkgs.python3Packages.pyzmq
      ];
    in
    {
      packages = forEachSystem (
        { pkgs }:
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "bitcoin-core";
            version = "31.99.0";
            src = ./.;
            enableParallelBuilding = true;

            nativeBuildInputs = [ pkgs.capnproto ] ++ buildTools pkgs;
            buildInputs = commonLibraries pkgs;

            cmakeFlags = [
              "-DBUILD_BENCH=OFF"
              "-DBUILD_BITCOIN_BIN=ON"
              "-DBUILD_CLI=ON"
              "-DBUILD_DAEMON=ON"
              "-DBUILD_FOR_FUZZING=OFF"
              "-DBUILD_FUZZ_BINARY=OFF"
              "-DBUILD_GUI=OFF"
              "-DBUILD_KERNEL_LIB=OFF"
              "-DBUILD_TESTS=ON"
              "-DBUILD_TX=ON"
              "-DBUILD_UTIL=ON"
              "-DBUILD_UTIL_CHAINSTATE=OFF"
              "-DBUILD_WALLET_TOOL=ON"
              "-DENABLE_IPC=ON"
              "-DENABLE_WALLET=ON"
              "-DWITH_EMBEDDED_ASMAP=ON"
              "-DWITH_EXTERNAL_LIBMULTIPROCESS=OFF"
              "-DWITH_USDT=OFF"
              "-DWITH_ZMQ=ON"
            ];

            installPhase = ''
              runHook preInstall
              cmake --install .
              install -Dm755 $out/libexec/test_bitcoin $out/bin/test_bitcoin
              install -Dm644 ${self}/share/rpcauth/rpcauth.py $out/share/rpcauth/rpcauth.py
              install -Dm644 test/config.ini $out/share/bitcoin-core/test-config.ini
              runHook postInstall
            '';
          };
        }
      );

      devShells = forEachSystem (
        { pkgs }:
        {
          default = pkgs.mkShell {
            packages = devShellPackages pkgs;
          };
        }
      );

      formatter = forEachSystem ({ pkgs }: pkgs.nixfmt-tree);

      checks = forEachSystem (
        { pkgs }:
        let
          package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        in
        {
          inherit package;
          devShell = self.devShells.${pkgs.stdenv.hostPlatform.system}.default;
          unitTests = pkgs.runCommand "bitcoin-core-unit-tests" { } ''
            ${package}/bin/test_bitcoin -l test_suite
            touch $out
          '';
          functionalTests =
            pkgs.runCommand "bitcoin-core-functional-tests"
              {
                nativeBuildInputs = functionalTestTools pkgs;
              }
              ''
            mkdir -p "$TMPDIR/build/test"
            mkdir -p "$TMPDIR/build/src/test/data"
            ln -s ${package}/bin "$TMPDIR/build/bin"
            cp -r ${self}/test/functional "$TMPDIR/build/test/"
            cp -r ${self}/src/test/data/. "$TMPDIR/build/src/test/data/"
            cp ${package}/share/bitcoin-core/test-config.ini "$TMPDIR/build/test/config.ini"
            sed -i \
              -e "s|^SRCDIR=.*|SRCDIR=${self}|" \
              -e "s|^BUILDDIR=.*|BUILDDIR=$TMPDIR/build|" \
              -e "s|^RPCAUTH=.*|RPCAUTH=${package}/share/rpcauth/rpcauth.py|" \
              "$TMPDIR/build/test/config.ini"
                ${pkgs.python3}/bin/python "$TMPDIR/build/test/functional/test_runner.py" \
                  -j "$NIX_BUILD_CORES" --tmpdir="$TMPDIR"
                touch $out
              '';
        }
      );
    };
}
