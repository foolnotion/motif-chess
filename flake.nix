{
  description = "motif-chess";

  inputs = {
    flake-parts.url = "github:hercules-ci/flake-parts";
    nixpkgs.url = "github:nixos/nixpkgs/master";
    foolnotion.url = "path:/home/bogdb/src/nur-pkg";
    foolnotion.inputs.nixpkgs.follows = "nixpkgs";
    chesslib.url = "github:foolnotion/chesslib";
    chesslib.inputs.nixpkgs.follows = "nixpkgs";
    chesslib.inputs.foolnotion.follows = "foolnotion";
    pgnlib.url = "github:foolnotion/pgnlib/f069c1b2a26a2402056d1c78e80fa0bffaaff36a";
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
          # Nix's Darwin CMake hook does not rewrite the Rust-built Slint dylib's
          # install name. Consumers otherwise retain its ephemeral build path.
          slint-fixed = pkgs.slint.overrideAttrs (old: {
            postFixup = (old.postFixup or "") + pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
              install_name_tool -id "$out/lib/libslint_cpp.dylib" "$out/lib/libslint_cpp.dylib"
            '';
          });
          slint-with-mcp = slint-fixed.overrideAttrs (old: {
            cmakeFlags = (old.cmakeFlags or [ ]) ++ [
              "-DSLINT_FEATURE_MCP=ON"
              "-DSLINT_FEATURE_SYSTEM_TESTING=ON"
            ];
          });
          ucilib-fixed = ucilib.packages.${system}.default.overrideAttrs (old: {
            postPatch = (old.postPatch or "") + ''
              substituteInPlace source/engine.cpp \
                --replace-fail '#include <fmt/core.h>' '#include <fmt/format.h>'
            '';
          });

          # nativefiledialog-extended for the file picker (Story 13.6). Linux uses the
          # xdg-desktop-portal backend over D-Bus: nixpkgs builds the GTK3 backend, which
          # drags GTK into the closure and aborts the process without its GSettings
          # schemas. Static, so the macOS bundle needs no extra library; the portal
          # build links only libdbus, which is propagated: a static library's users
          # need it on their run path (the app would not start without it).
          # The test programs need a display and are not built.
          nfd-portal =
            let
              static = pkgs.nativefiledialog-extended.override { enableShared = false; };
            in
            if pkgs.stdenv.hostPlatform.isLinux then
              static.overrideAttrs (old: {
                buildInputs = [ pkgs.dbus ];
                propagatedBuildInputs = (old.propagatedBuildInputs or [ ]) ++ [ pkgs.dbus ];
                cmakeFlags = old.cmakeFlags ++ [
                  "-DNFD_PORTAL=ON"
                  "-DNFD_BUILD_TESTS=OFF"
                  "-DNFD_BUILD_SDL2_TESTS=OFF"
                ];
              })
            else
              static;

          glaze-simd-no-ssl = pkgs.glaze.overrideAttrs (old: {
            cmakeFlags = (old.cmakeFlags or [ ]) ++ [
              "-DGLZ_DISABLE_SIMD=OFF"
              "-DGLZ_ENABLE_SSL=OFF"
            ];
            postInstall = (old.postInstall or "") + ''
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
              ExposedPorts = {
                "8080/tcp" = { };
              };
              Env = [
                "MOTIF_DB_PATH=/data/db"
                "MOTIF_HTTP_PORT=8080"
                "MOTIF_HTTP_HOST=0.0.0.0"
              ];
              Volumes = {
                "/data/db" = { };
              };
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
            ];

            buildInputs = with pkgs; [
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
              httplib
              reproc
              ucilib-fixed
              chesslib.packages.${system}.default
              pgnlib.packages.${system}.default

              # additional project deps
              cxxopts
              fast-float
              gtl
              taskflow
              xxhash
              zstd
            ];
          };

          packages.app = packages.default.overrideAttrs (old: {
            name = "motif-chess-app";
            cmakeFlags =
              old.cmakeFlags
              ++ pkgs.lib.optionals (pkgs.stdenv.hostPlatform.isLinux || pkgs.stdenv.hostPlatform.isDarwin) [
                "-Dmotif_ENABLE_SLINT_APP=ON"
              ];
            buildInputs =
              old.buildInputs
              ++ pkgs.lib.optionals (pkgs.stdenv.hostPlatform.isLinux || pkgs.stdenv.hostPlatform.isDarwin) [
                slint-fixed
                nfd-portal
              ];
          });

          packages.app-mcp = packages.default.overrideAttrs (old: {
            name = "motif-chess-app-mcp";
            cmakeFlags =
              old.cmakeFlags
              ++ pkgs.lib.optionals (pkgs.stdenv.hostPlatform.isLinux || pkgs.stdenv.hostPlatform.isDarwin) [
                "-Dmotif_ENABLE_SLINT_APP=ON"
              ];
            buildInputs =
              old.buildInputs
              ++ pkgs.lib.optionals (pkgs.stdenv.hostPlatform.isLinux || pkgs.stdenv.hostPlatform.isDarwin) [
                slint-with-mcp
                nfd-portal
              ];
          });

          devShells.default = mkShell {
            name = "motif-chess-dev";

            nativeBuildInputs =
              packages.app-mcp.nativeBuildInputs
              ++ (with pkgs; [
                # analysis and formatting
                clang-tools
                cppcheck
                include-what-you-use
                cmake-language-server

                # Slint layout diagnostics and standalone rendering
                slint-lsp
                slint-viewer

                # Required to rebuild Slint with its MCP feature.
                cargo
                rustc

                # spec-kit
                python3
                uv

                nodejs_24

                # CI local runner
                act
              ]);

            buildInputs =
              packages.app-mcp.buildInputs
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
