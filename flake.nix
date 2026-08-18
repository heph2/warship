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
        warship = pkgs.stdenv.mkDerivation {
          pname = "warship";
          version = "0.1.0";
          src = ./.;

          buildInputs = with pkgs; [
            libjuice
            libplum
            libwebsockets
            openssl
          ];

          # The Makefile has no install target: this is a two-binary project
          # and a rule to copy one file is not worth carrying.
          installPhase = ''
            runHook preInstall
            install -Dm755 warship $out/bin/warship
            runHook postInstall
          '';

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            make test
            runHook postCheck
          '';

          meta = {
            description = "Terminal Battleship played peer to peer over ICE";
            homepage = "https://github.com/heph2/warship";
            mainProgram = "warship";
            platforms = pkgs.lib.platforms.linux;
          };
        };

        signal-server = pkgs.stdenv.mkDerivation {
          pname = "warship-signal-server";
          version = "0.1.0";
          src = ./.;

          buildInputs = [
            pkgs.libwebsockets
            pkgs.openssl
          ];

          buildPhase = ''
            runHook preBuild
            make signal-server
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            install -Dm755 signal-server $out/bin/warship-signal-server
            runHook postInstall
          '';

          meta = {
            description = "Room-code signaling service for warship";
            mainProgram = "warship-signal-server";
            platforms = pkgs.lib.platforms.linux;
          };
        };
      in
      {
        packages.default = warship;
        packages.warship = warship;
        packages.signal-server = signal-server;
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
          # libwebsockets is built against OpenSSL and its public header includes
          # <openssl/ssl.h>, so OpenSSL has to be here too even though we never
          # call it directly. LWS gives us wss:// on the client; the signaling
          # server itself stays plaintext behind a reverse proxy.
          buildInputs = with pkgs; [
            libjuice
            libplum
            libwebsockets
            openssl
          ];

          shellHook = ''
            echo "warship C/C++ dev shell"
            echo "  cc: $(gcc --version | head -n1)"
            echo "  make: $(make --version | head -n1)"
            echo "  libjuice: ${libjuice.version}  libplum: ${libplum.version}"
            echo "  libwebsockets: ${pkgs.libwebsockets.version}"
          '';
        };
      }
    );
}
