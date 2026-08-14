{
  description = "Bitcoin Core Docker image";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };

      nativeBuildInputs = with pkgs; [
        cmake
        ninja
        pkg-config
        python3
      ];

      buildInputs = with pkgs; [
        boost
        sqlite
        zeromq
      ];

      bitcoin-core = pkgs.stdenv.mkDerivation {
        pname = "bitcoin-core";
        version = "31.99.0";
        src = ./.;

        inherit nativeBuildInputs buildInputs;

        cmakeFlags = [
          "-DBUILD_BITCOIN_BIN=OFF"
          "-DBUILD_CLI=ON"
          "-DBUILD_DAEMON=ON"
          "-DBUILD_GUI=OFF"
          "-DBUILD_TESTS=OFF"
          "-DBUILD_TX=OFF"
          "-DBUILD_UTIL=OFF"
          "-DBUILD_WALLET_TOOL=OFF"
          "-DENABLE_EXTERNAL_SIGNER=OFF"
          "-DENABLE_IPC=OFF"
          "-DENABLE_WALLET=ON"
          "-DINSTALL_MAN=OFF"
          "-DWITH_CCACHE=OFF"
          "-DWITH_USDT=OFF"
          "-DWITH_ZMQ=ON"
        ];

        installPhase = ''
          runHook preInstall
          install -Dm755 bin/bitcoind $out/bin/bitcoind
          install -Dm755 bin/bitcoin-cli $out/bin/bitcoin-cli
          runHook postInstall
        '';
      };

      docker-image = pkgs.dockerTools.buildLayeredImage {
        name = "bitcoin-core";
        tag = "latest";
        architecture = "amd64";
        contents = [ bitcoin-core ];
        config = {
          Entrypoint = [ "/bin/bitcoind" ];
          Cmd = [ "-printtoconsole" ];
        };
      };
    in
    {
      packages.${system} = {
        default = docker-image;
        inherit bitcoin-core docker-image;
      };

      devShells.${system}.default = pkgs.mkShell {
        inherit nativeBuildInputs buildInputs;
      };
    };
}
