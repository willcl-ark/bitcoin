{
  description = "Bitcoin Core development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    nixpkgs,
    flake-utils,
    ...
  }:
    flake-utils.lib.eachDefaultSystem (
      system: let
        pkgs = import nixpkgs {
          inherit system;
        };
      in {
        formatter = pkgs.alejandra;

        devShells.default = pkgs.mkShell.override {
          stdenv = pkgs.gcc15Stdenv;
        } {
          buildInputs = with pkgs; [
            ccache
            cmake
            ninja
            pkg-config

            boost
            capnproto
            libevent
            libsystemtap
            linuxPackages.bcc
            linuxPackages.bpftrace
            sqlite.dev
            zeromq
          ];

          shellHook = ''
            export CCACHE_COMPRESS=1
            export CCACHE_SLOPPINESS=random_seed
            export CCACHE_UMASK=007
            export CMAKE_GENERATOR=Ninja

            echo "CCACHE_DIR: $CCACHE_DIR"
          '';
        };
      }
    );
}

