{
  pkgs ? import (builtins.fetchTarball {
    # Keep the compiler and dependency-building toolchain reproducible.
    url = "https://github.com/NixOS/nixpkgs/archive/5880666fd9eb563038431edb35c2d0aa595884e6.tar.gz";
    sha256 = "sha256-OURZPknrTjQrlNyxPdqzyqmU/81Wes1CUP/Ft1Rv/YI=";
  }) { },
}:

let
  build = pkgs.writeShellScriptBin "build-static" ''
    set -euo pipefail

    jobs="''${JOBS:-$(nproc)}"
    host="''${HOST:-$(depends/config.guess)}"

    if [[ ! -x "depends/config.guess" ]]; then
      echo "run build from the Bitcoin Core source directory root" >&2
      exit 1
    fi

    case "$host" in
      x86_64*linux-gnu|aarch64*linux-gnu)
        linker_flags="-L${pkgs.glibc.static}/lib -static-pie -static-libgcc -static-libstdc++ -Wl,-O2"
        ;;
      *-*-linux-gnu)
        linker_flags="-L${pkgs.glibc.static}/lib -static-libgcc -static-libstdc++"
        ;;
      *)
        echo "unsupported static build host: $host" >&2
        exit 1
        ;;
    esac

    make -C depends --jobs="$jobs" HOST="$host" NO_QT=1

    cmake -B build -G Ninja \
      --toolchain "depends/$host/toolchain.cmake" \
      -DBUILD_BENCH=OFF \
      -DBUILD_FUZZ_BINARY=OFF \
      -DBUILD_GUI=OFF \
      -DBUILD_GUI_TESTS=OFF \
      -DBUILD_TESTS=ON \
      -DCMAKE_EXE_LINKER_FLAGS="$linker_flags" \
      -DAPPEND_LDFLAGS="-Wl,--no-dynamic-linker"

    cmake --build build --parallel "$jobs" \
      --target "''${TARGET:-all}"
  '';
in
pkgs.mkShell {

  packages = with pkgs; [
    build
    bison
    binutils
    cacert
    cmake
    curl
    gnumake
    ninja
    patch
    pkg-config
    python3
    which
    xz
  ];
}
