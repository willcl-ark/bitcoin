final: prev: {
  capnproto = prev.capnproto.overrideAttrs (_: rec {
    version = "1.5.0";
    src = prev.fetchFromGitHub {
      owner = "capnproto";
      repo = "capnproto";
      rev = "v${version}";
      hash = "sha256-2J3FYwPAtbahHI1y1KMqU8Gn2YlKyIW8kZIJz2Ja31w=";
    };
  });

  capnproto-runtime =
    final.runCommand "capnproto-runtime-${final.capnproto.version}"
      {
        nativeBuildInputs = [ final.patchelf ];
      }
      ''
        runtime_rpath='${
          final.lib.makeLibraryPath [
            final.stdenv.cc.cc
            final.glibc
          ]
        }:$ORIGIN'

        for library in libcapnp-rpc libcapnp libkj-async libkj; do
          install -Dm755 \
            "${final.capnproto}/lib/$library.so.${final.capnproto.version}" \
            "$out/lib/$library.so.${final.capnproto.version}"
          patchelf --set-rpath "$runtime_rpath" \
            "$out/lib/$library.so.${final.capnproto.version}"
        done
      '';
}
