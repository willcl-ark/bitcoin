{
  hostPkgs, # Native platform
  targetPkgs, # Target platform
  customStdEnv,
  version,
  lib,
}:
let
  # Native packages built for the host/build platform
  nativePackages = {
    nativeCapnp = hostPkgs.callPackage ./packages/native_capnp.nix { };
    nativeLibmultiprocess = hostPkgs.callPackage ./packages/native_libmultiprocess.nix {
      native_capnp = hostPkgs.callPackage ./packages/native_capnp.nix { };
      inherit version;
    };
  };

  # Target packages built for the target platform
  targetPackagesRaw = {
    boost = targetPkgs.callPackage ./packages/boost.nix { };
    capnp = targetPkgs.callPackage ./packages/capnp.nix { };
    libevent = targetPkgs.callPackage ./packages/libevent.nix { };
    sqlite = targetPkgs.callPackage ./packages/sqlite.nix { };
    zeromq = targetPkgs.callPackage ./packages/zeromq.nix { };
  };

  # Override stdenv for all target packages
  targetPackages = lib.mapAttrs (
    name: pkg: pkg.override { stdenv = customStdEnv; }
  ) targetPackagesRaw;
in
{
  target = targetPackages;
  native = nativePackages;
}
