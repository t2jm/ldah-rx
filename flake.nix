{
  description = "ldah-rx";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    nixpkgs-esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";
    treefmt-nix.url = "github:numtide/treefmt-nix";
  };

  outputs =
    { self, nixpkgs, ... }@inputs:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      treefmtEval = forAllSystems (
        system: inputs.treefmt-nix.lib.evalModule nixpkgs.legacyPackages.${system} ./treefmt.nix
      );
    in
    {
      formatter = forAllSystems (system: treefmtEval.${system}.config.build.wrapper);
      checks = forAllSystems (system: {
        formatting = treefmtEval.${system}.config.build.check self;
      });
      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default =
            let
              inherit (pkgs)
                codespell
                mkShell
                prek
                ;
            in
            mkShell {
              inputsFrom = [
                inputs.nixpkgs-esp-dev.devShells.${system}.esp-idf-full
              ];
              packages = [
                codespell
                prek
                self.formatter.${system}
              ];
            };
        }
      );
    };
}
