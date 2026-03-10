{
  description = "noctalia-qs - flexible QtQuick based desktop shell toolkit for Noctalia";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default-linux";
  };

  outputs = {
    self,
    nixpkgs,
    systems,
    ...
  }: let
    eachSystem = fn:
      nixpkgs.lib.genAttrs
      (import systems)
      (system: fn nixpkgs.legacyPackages.${system});

    mkDate = longDate:
      nixpkgs.lib.concatStringsSep "-" [
        (builtins.substring 0 4 longDate)
        (builtins.substring 4 2 longDate)
        (builtins.substring 6 2 longDate)
      ];

    version = mkDate (self.lastModifiedDate or "19700101") + "_" + (self.shortRev or "dirty");
    gitRev = self.rev or self.dirtyRev or "dirty";
  in {
    overlays.default = final: prev: {
      quickshell = final.callPackage ./nix/package.nix {inherit version gitRev;};
    };

    packages = eachSystem (pkgs: {
      quickshell = pkgs.callPackage ./nix/package.nix {inherit version gitRev;};
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.quickshell;
    });

    devShells = eachSystem (pkgs: {
      default = pkgs.callPackage ./nix/shell.nix {
        quickshell = self.packages.${pkgs.stdenv.hostPlatform.system}.quickshell.override {
          stdenv = pkgs.clangStdenv;
        };
      };
    });
  };
}
