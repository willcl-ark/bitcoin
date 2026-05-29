{
  description = "2140 Bitcoin Core devShell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));

      mkDevShell =
        pkgs:
        let
          inherit (pkgs) lib;
          inherit (pkgs.stdenv) isLinux isDarwin;

          nativeBuildInputs = [
            pkgs.ccache
            pkgs.cmakeCurses # cmakeCurses includes the ccmake tool
            pkgs.ninja
            pkgs.pkg-config
            pkgs.python313
          ]
          ++ lib.optionals isLinux [
            pkgs.libsystemtap
            pkgs.linuxPackages.bcc
            pkgs.linuxPackages.bpftrace
          ];
          buildInputs = [
            pkgs.boost
            pkgs.capnproto
          ];
        in
        pkgs.mkShell {
          inherit nativeBuildInputs buildInputs;
          hardeningDisable = lib.optionals isDarwin [ "stackclashprotection" ];
          CMAKE_GENERATOR = "Ninja";
          CMAKE_EXPORT_COMPILE_COMMANDS = 1;
          LD_LIBRARY_PATH = lib.makeLibraryPath [ pkgs.capnproto ];
          LOCALE_ARCHIVE = lib.optionalString isLinux "${pkgs.glibcLocales}/lib/locale/locale-archive";
        };
    in
    {
      devShells = forAllSystems (pkgs: {
        default = mkDevShell pkgs;
      });
      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);
    };
}
