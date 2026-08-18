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

        # libjuice is not in nixpkgs, so we build it here. Small CMake project,
        # pinned by tag and hash -- bump both together.
        libjuice = pkgs.stdenv.mkDerivation (finalAttrs: {
          pname = "libjuice";
          version = "1.7.3";

          src = pkgs.fetchFromGitHub {
            owner = "paullouisageneau";
            repo = "libjuice";
            rev = "v${finalAttrs.version}";
            hash = "sha256-XUcutgrP96hdXGUl4JjN2iovdkwYRw9LP6ze6S4Wp+A=";
          };

          nativeBuildInputs = with pkgs; [ cmake pkg-config ];

          meta = {
            description = "UDP Interactive Connectivity Establishment (ICE) library";
            homepage = "https://github.com/paullouisageneau/libjuice";
            license = pkgs.lib.licenses.mpl20;
            platforms = pkgs.lib.platforms.unix;
          };
        });
      in
      {
        packages.libjuice = libjuice;

        devShells.default = pkgs.mkShell {
          name = "warship-dev";

          # Tools that run at build time.
          packages = with pkgs; [
            gcc
            gnumake
            pkg-config

            # LSP + formatting/linting
            clang-tools # clangd, clang-format, clang-tidy

            # Debugging
            gdb
            tcpdump # watch the ICE handshake and the game datagrams
          ];

          # Libraries we link against. These belong in buildInputs, not
          # packages: only buildInputs makes the cc wrapper add the -I and -L
          # paths, and libjuice ships no pkg-config file to fall back on.
          buildInputs = [ libjuice ];

          shellHook = ''
            echo "warship C/C++ dev shell"
            echo "  cc: $(gcc --version | head -n1)"
            echo "  make: $(make --version | head -n1)"
            echo "  libjuice: ${libjuice.version}"
          '';
        };
      }
    );
}
