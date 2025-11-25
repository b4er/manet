{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/5954d3359cc7178623da6c7fd23dc7f7504d7187";
    git-hooks.url = "github:cachix/git-hooks.nix";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, git-hooks, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        f-stack = import ./nix/f-stack.nix { inherit pkgs; };

        manet = pkgs.stdenv.mkDerivation {
          pname = "manet";
          version = "0.0.1";
          src = ./.;

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DUSE_FSTACK=ON"
          ];

          doCheck = true;

          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
          ];

          buildInputs = [
            pkgs.doctest
            pkgs.lcov
            pkgs.openssl

            f-stack
          ];
        };
      in
      {
        packages = {
          default = manet;
          inherit f-stack;
        };

        checks = {
          pre-commit = git-hooks.lib.${system}.run {
            src = ./.;

            hooks = {
              clang-format.enable = true;
              cmake-format.enable = true;
              end-of-file-fixer.enable = true;
              nixpkgs-fmt.enable = true;
              trim-trailing-whitespace.enable = true;
            };
          };
        };

        devShells = {
          default = pkgs.mkShell {
            buildInputs = with pkgs; [
              f-stack
              jq
              lcov
              self.checks.${system}.pre-commit.enabledPackages
              valgrind
            ];

            inputsFrom = [ manet ];

            NIX_ENFORCE_NO_NATIVE = 0;

            shellHook = ''
              ROOT="$(${pkgs.git}/bin/git rev-parse --show-toplevel)"

              ${self.checks.${system}.pre-commit.shellHook}
            '';
          };
        };
      });
}
