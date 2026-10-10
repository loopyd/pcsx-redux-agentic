{
  description = "Linux PlayStation 1 emulator for agentic reverse engineering";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs = {
    self,
    nixpkgs
  }:
  let
    lib = nixpkgs.lib;
    supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
    forAllSystems = lib.genAttrs supportedSystems;
  in {
    packages = forAllSystems (system:
    let
      pkgs = nixpkgs.legacyPackages.${system};
      cross = import nixpkgs {
        inherit system;
        crossSystem = "mipsel-none-elf";
      };
    in {
      default = self.packages.${system}.pcsx-redux;

      pcsx-redux = pkgs.callPackage ./pcsx-redux.nix {
          src = self;
          platforms = supportedSystems;
          gccMips = cross.buildPackages.gccWithoutTargetLibc;
      };

      # Use this one if you want to provide your own bios and don't care for
      # openbios being bundled in.
      pcsx-redux-no-bios = pkgs.callPackage ./pcsx-redux.nix {
          src = self;
          platforms = supportedSystems;
          withOpenbios = false;
          gccMips = null;
      };
    });

    devShells = forAllSystems (system: {
        # Using this as the default dev shell because we probably don't want to
        # work on mips code, and shouldn't waste time building the mipsel
        # toolchain.
        default = self.packages.${system}.pcsx-redux-no-bios;
    });

  };
}
