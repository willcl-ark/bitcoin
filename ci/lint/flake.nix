{
  description = "Bitcoin Core lint dependencies";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { nixpkgs, ... }: {
    packages = nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        default = pkgs.buildEnv {
          name = "bitcoin-lint-env";
          paths = with pkgs; [
            bash
            cacert
            cargo
            clang-tools
            clippy
            coreutils
            diffutils
            findutils
            gawk
            git
            glibcLocales
            gnugrep
            gnupg
            gnused
            markuplinkchecker
            moreutils
            perl
            (python3.withPackages (
              ps: with ps; [
                lief
                mypy
                pyzmq
              ]
            ))
            ruff
            rustc
            rustfmt
            shellcheck
            stdenv.cc
            (writeTextDir "etc/os-release" ''
              NAME="Bitcoin Core lint environment"
              ID=bitcoin-lint
            '')
          ];
          pathsToLink = [
            "/bin"
            "/etc"
            "/lib/locale"
          ];
          nativeBuildInputs = [ pkgs.makeWrapper ];
          postBuild = ''
            makeWrapper ${pkgs.coreutils}/bin/env "$out/bin/with-lint-env" \
              --set PATH "$out/bin" \
              --set LOCALE_ARCHIVE "$out/lib/locale/locale-archive" \
              --set SSL_CERT_FILE "$out/etc/ssl/certs/ca-bundle.crt" \
              --set GIT_SSL_CAINFO "$out/etc/ssl/certs/ca-bundle.crt"
          '';
          meta.mainProgram = "with-lint-env";
        };
      }
    );
  };
}
