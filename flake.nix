{
  description = "iGPU Shim development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/24.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            cmake
            ninja
            clang_17
            llvm
            lld
            python311
            python311Packages.pip
            cargo
            rustc
            rustfmt
            clippy
            curl
            jq
            git
          ];
          shellHook = ''
            echo "iGPU Shim dev shell ready"
            echo "Linux/Clang 17 build: cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-17 -DCMAKE_CXX_COMPILER=clang++-17"
          '';
        };
      }
    );
}
