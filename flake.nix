{
  description = "warship — C/C++ development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        devShells.default = pkgs.mkShell {
          name = "warship-dev";

          packages = with pkgs; [
            # Toolchain
            gcc
            gnumake
            pkg-config

            # LSP + formatting/linting
            clang-tools # clangd, clang-format, clang-tidy

            # Debugging
            gdb
          ];

          shellHook = ''
            echo "warship C/C++ dev shell"
            echo "  cc: $(gcc --version | head -n1)"
            echo "  make: $(make --version | head -n1)"
          '';
        };
      }
    );
}
