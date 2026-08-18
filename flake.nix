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
        # Port mapping: NAT-PMP, PCP and UPnP-IGD behind one API. Same author as
        # libjuice and meant to pair with it. Also absent from nixpkgs.
        libplum = pkgs.stdenv.mkDerivation (finalAttrs: {
          pname = "libplum";
          version = "0.6.0";

          src = pkgs.fetchFromGitHub {
            owner = "paullouisageneau";
            repo = "libplum";
            rev = "v${finalAttrs.version}";
            hash = "sha256-WFxdYBLrqaExjl0JR2KxXYfdNaxiyfI1NxCTjixCwJQ=";
          };

          nativeBuildInputs = with pkgs; [ cmake pkg-config ];

          meta = {
            description = "Multi-protocol port mapping client library";
            homepage = "https://github.com/paullouisageneau/libplum";
            license = pkgs.lib.licenses.mpl20;
            platforms = pkgs.lib.platforms.unix;
          };
        });
      in
      {
        packages.libjuice = libjuice;
        packages.libplum = libplum;

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
          buildInputs = [ libjuice libplum ];

          shellHook = ''
            echo "warship C/C++ dev shell"
            echo "  cc: $(gcc --version | head -n1)"
            echo "  make: $(make --version | head -n1)"
            echo "  libjuice: ${libjuice.version}  libplum: ${libplum.version}"
          '';
        };
      }
    );
}
