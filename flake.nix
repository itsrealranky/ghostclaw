{
  description = "ghostclaw - fast, ultra-lightweight AI assistant infrastructure in C++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "ghostclaw";
          version = "0.1.0";

          src = ./.;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
          ];

          buildInputs = with pkgs; [
            curl
            openssl
            sqlite
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DGHOSTCLAW_BUILD_BENCHMARKS=OFF"
          ];

          installPhase = ''
            runHook preInstall
            install -Dm755 ghostclaw $out/bin/ghostclaw
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description = "Fast, ultra-lightweight, fully autonomous AI assistant infrastructure";
            homepage = "https://github.com/anomalyco/ghostclaw";
            license = licenses.mit;
            mainProgram = "ghostclaw";
          };
        };
      });
}
