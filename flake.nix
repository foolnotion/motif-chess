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
