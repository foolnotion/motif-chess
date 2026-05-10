{
  description = "motif-chess";

  inputs = {
    flake-parts.url = "github:hercules-ci/flake-parts";
    nixpkgs.url = "github:nixos/nixpkgs/master";
    foolnotion.url = "github:foolnotion/nur-pkg/master";
    foolnotion.inputs.nixpkgs.follows = "nixpkgs";
    chesslib.url = "github:foolnotion/chesslib";
    chesslib.inputs.nixpkgs.follows = "nixpkgs";
    chesslib.inputs.foolnotion.follows = "foolnotion";
    pgnlib.url = "github:foolnotion/pgnlib";
    pgnlib.inputs.nixpkgs.follows = "nixpkgs";
    pgnlib.inputs.foolnotion.follows = "foolnotion";
    ucilib.url = "github:foolnotion/ucilib";
    ucilib.inputs.nixpkgs.follows = "nixpkgs";
    ucilib.inputs.foolnotion.follows = "foolnotion";
  };

  outputs =
    inputs@{
      self,
      flake-parts,
      nixpkgs,
      foolnotion,
      chesslib,
      pgnlib,
      ucilib,
    }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "x86_64-darwin"
        "aarch64-linux"
        "aarch64-darwin"
      ];

      perSystem =
        { system, ... }:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [ foolnotion.overlay ];
          };
          inherit (pkgs.llvmPackages_21) stdenv;
          mkShell = pkgs.mkShell.override { inherit stdenv; };
          kddockwidgets-qtquick = pkgs.kddockwidgets.overrideAttrs (old: {
            cmakeFlags = (old.cmakeFlags or [ ]) ++ [
              "-DKDDockWidgets_FRONTENDS=qtquick"
            ];
            buildInputs = (old.buildInputs or [ ]) ++ [ pkgs.qt6.qtdeclarative ];
          });
          glaze-simd-no-ssl = pkgs.glaze.overrideAttrs (old: {
            cmakeFlags = (old.cmakeFlags or [ ]) ++ [
              "-DGLZ_DISABLE_SIMD=OFF"
              "-DGLZ_ENABLE_SSL=OFF"
            ];
            postInstall =
              (old.postInstall or "")
              + ''
                substituteInPlace "$out/share/glaze/glazeConfig.cmake" \
                  --replace 'set(_glaze_ENABLE_SSL "TRUE")' 'set(_glaze_ENABLE_SSL "FALSE")'

                substituteInPlace "$out/share/glaze/glazeTargets.cmake" \
                  --replace 'INTERFACE_COMPILE_DEFINITIONS "GLZ_DISABLE_SIMD;GLZ_ENABLE_SSL"' 'INTERFACE_COMPILE_DEFINITIONS ""' \
                  --replace 'INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto"' 'INTERFACE_LINK_LIBRARIES ""'
              '';
          });
        in
        rec {
          packages.docker = pkgs.dockerTools.buildLayeredImage {
            name = "motif-chess";
            tag = "latest";
            contents = [
              packages.default
              pkgs.dockerTools.usrBinEnv
              pkgs.dockerTools.binSh
              pkgs.coreutils
            ];
            config = {
              Entrypoint = [ "${packages.serve}/bin/motif-serve" ];
              ExposedPorts = { "8080/tcp" = { }; };
              Env = [
                "MOTIF_DB_PATH=/data/db"
                "MOTIF_HTTP_PORT=8080"
                "MOTIF_HTTP_HOST=0.0.0.0"
              ];
              Volumes = { "/data/db" = { }; };
            };
          };

          packages.serve = pkgs.writeShellApplication {
            name = "motif-serve";
            runtimeInputs = [ packages.default ];
            text = ''
              # Resolve database directory: MOTIF_DB_PATH > XDG_DATA_HOME > ~/.local/share
              export MOTIF_DB_PATH="''${MOTIF_DB_PATH:-''${XDG_DATA_HOME:-$HOME/.local/share}/motif-chess/db}"
              mkdir -p "$MOTIF_DB_PATH"

              # Port and host fall through to the binary's own env-var handling
              # (MOTIF_HTTP_PORT, MOTIF_HTTP_HOST, MOTIF_HTTP_CORS_ORIGINS).
              # Defaults: port 8080, host localhost.
              export MOTIF_HTTP_PORT="''${MOTIF_HTTP_PORT:-8080}"
              export MOTIF_HTTP_HOST="''${MOTIF_HTTP_HOST:-localhost}"

              exec motif_http_server "$@"
            '';
          };

          packages.default = stdenv.mkDerivation {
            name = "motif-chess";
            src = self;

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DCMAKE_CXX_FLAGS=${if pkgs.stdenv.hostPlatform.isx86_64 then "-march=x86-64" else ""}"
            ];

            nativeBuildInputs = with pkgs; [
              cmake
              pkg-config
              qt6.wrapQtAppsHook
            ];

            buildInputs = with pkgs; [
              # Qt 6 — Qt Quick / QML stack (headers confined to motif_app)
              qt6.qtbase
              qt6.qtdeclarative
              kddockwidgets-qtquick
              # core deps (design doc)
              cpptrace
              fmt
              glaze-simd-no-ssl
              libassert
              libdwarf
              magic-enum
              mdspan
              microsoft-gsl
              tl-expected
              spdlog
              sqlite
              duckdb
              httplib
              reproc
              lexy
              chesslib.packages.${system}.default
              pgnlib.packages.${system}.default
              ucilib.packages.${system}.default

              # additional project deps
              cxxopts
              fast-float
              gtl
              taskflow
              xxHash
              zstd
            ];
          };

          devShells.default = mkShell {
            name = "motif-chess-dev";

            nativeBuildInputs =
              packages.default.nativeBuildInputs
              ++ (with pkgs; [
                # analysis and formatting
                clang-tools
                cppcheck
                include-what-you-use
                cmake-language-server

                # spec-kit
                python3
                uv

                nodejs_24

                # CI local runner
                act
              ]);

            buildInputs =
              packages.default.buildInputs
              ++ (with pkgs; [
                # testing
                catch2_3

                # HTTP server (needed for CMake find_package in fresh configures)
                httplib

                # debugging / profiling
                cpptrace
                libassert
                libdwarf
                nanobench
              ]);
          };
        };
    };
}
