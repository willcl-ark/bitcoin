# Copyright 0xB10C
{ pkgs ? import (fetchTarball "https://github.com/nixos/nixpkgs/archive/nixos-unstable.tar.gz") {},
  spareCores ? 0,
  withClang ? false,
  withDebug ? false,
}:
let
  inherit (pkgs.lib) optionals strings;

  # Add mlc binary fetching
  mlcBinary = pkgs.fetchurl {
    url = "https://github.com/becheran/mlc/releases/download/v0.18.0/mlc-x86_64-linux";
    sha256 = "sha256-jbdp+UlFybBE+o567L398hbcWHsG8aQGqYYf5h9JRkw=";
  };

  # Hyper-wrapper
  # This doesn't install to PATH
  hyper-wrapper = pkgs.rustPlatform.buildRustPackage rec {
    pname = "hyper-wrapper";
    version = "0.1.3";
    src = pkgs.fetchCrate {
      inherit pname version;
      sha256 = "sha256-uX2GFw+WuQGV9ZOK6/18Gz7EfudYvzGo6py2cfUQBSc=";
    };
    cargoHash = "sha256-KBHuP+ZwhThFjf06oKDOLdylk9DdXFLDJci6kG8jCYM=";
    meta = with pkgs.lib; {
      description = "Hyperfine wrapper";
      homepage = "https://github.com/bitcoin-dev-tools/hyper-wrapper";
      license = licenses.mit;
    };
  };

  # Create a derivation for mlc
  mlc = pkgs.runCommand "mlc" {} ''
    mkdir -p $out/bin
    cp ${mlcBinary} $out/bin/mlc
    chmod +x $out/bin/mlc
  '';

  binDirs =
    [ "\$PWD/build/src" ];
  configureFlags =
    [ "--with-boost-libdir=$NIX_BOOST_LIB_DIR" ]
    ++ optionals withClang [ "CXX=clang++" "CC=clang" ]
    ++ optionals withDebug [ "--enable-debug" ];
  jobs =
    if (strings.hasSuffix "linux" builtins.currentSystem) then "$(($(nproc)-${toString spareCores}))"
    else if (strings.hasSuffix "darwin" builtins.currentSystem) then "$(($(sysctl -n hw.physicalcpu)-${toString spareCores}))"
    else "6";
in pkgs.mkShell {
    nativeBuildInputs = with pkgs; [
      autoconf
      automake
      libtool
      pkg-config
      boost
      libevent
      zeromq
      sqlite
      clang_18

      # tests
      hexdump

      # compiler output caching per
      # https://github.com/bitcoin/bitcoin/blob/master/doc/productivity.md#cache-compilations-with-ccache
      ccache

      # for newer cmake building
      cmake

      # depends
      byacc

      # debugging
      gdb

      # tracing
      libsystemtap
      linuxPackages.bpftrace
      linuxPackages.bcc

    ];
    buildInputs = with pkgs; [
      just
      bash

      # lint requirements
      cargo
      git
      mlc
      ruff
      rustc
      rustup
      shellcheck
      python310
      uv

      # Benchmarking
      flamegraph
      hyper-wrapper
      hyperfine
      jq
      linuxKernel.packages.linux_6_6.perf
      perf-tools
    ];

    # Modifies the Nix clang++ wrapper to avoid warning:
    # "_FORTIFY_SOURCE requires compiling with optimization (-O)"
    hardeningDisable = if withDebug then [ "all" ] else [ ];

    shellHook = ''
      echo "Bitcoin Core build nix-shell"
      echo ""
      echo "Setting up python venv"

      uv venv --python 3.10
      source .venv/bin/activate
      uv pip install -r pyproject.toml

      BCC_EGG=${pkgs.linuxPackages.bcc}/${pkgs.python3.sitePackages}/bcc-${pkgs.linuxPackages.bcc.version}-py3.${pkgs.python3.sourceVersion.minor}.egg

      echo "adding bcc egg to PYTHONPATH: $BCC_EGG"
      if [ -f $BCC_EGG ]; then
        export PYTHONPATH="$PYTHONPATH:$BCC_EGG"
        echo ""
      else
        echo "The bcc egg $BCC_EGG does not exist. Maybe the python or bcc version is different?"
      fi

      echo "adding ${builtins.concatStringsSep ":" binDirs} to \$PATH to make running built binaries more natural"
      export PATH=$PATH:${builtins.concatStringsSep ":" binDirs};

      rustup default stable
      rustup component add rustfmt

    '';
}
