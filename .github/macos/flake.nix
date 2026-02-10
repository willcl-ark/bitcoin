{
  description = "Bitcoin Core macOS CI dependencies";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [
            (final: prev: {
              capnproto = prev.capnproto.overrideAttrs (oldAttrs: rec {
                version = "1.3.0";
                src = prev.fetchFromGitHub {
                  owner = "capnproto";
                  repo = "capnproto";
                  rev = "v${version}";
                  hash = "sha256-fvZzNDBZr73U+xbj1LhVj1qWZyNmblKluh7lhacV+6I=";
                };
                patches = [ ];
              });
            })
          ];
        };

        pythonEnv = pkgs.python313.withPackages (
          ps: with ps; [
            pyzmq
            pycapnp
          ]
        );
      in
      {
        formatter = pkgs.nixfmt-tree;
        devShells.default = pkgs.mkShellNoCC {
          nativeBuildInputs = [
            pkgs.ccache
            pkgs.cmake
            pkgs.coreutils
            pkgs.ninja
            pkgs.pkg-config
            pkgs.qt6.wrapQtAppsHook
          ];

          buildInputs = [
            pkgs.boost
            pkgs.capnproto
            pkgs.libevent
            pkgs.qrencode
            pkgs.qt6.qtbase
            pkgs.qt6.qttools
            pkgs.zeromq
          ];

          packages = [
            pythonEnv
          ];

          shellHook = ''
            export SDKROOT=$(/usr/bin/xcrun --show-sdk-path)
            export CMAKE_PREFIX_PATH="$NIXPKGS_CMAKE_PREFIX_PATH"
          '';
        };
      }
    );
}
