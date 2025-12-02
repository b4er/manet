{ pkgs
, useFStack ? false
}:
let
  fstack = import ./fstack.nix { inherit pkgs; };

  sbepp = import ./sbepp.nix { inherit pkgs; };
  spsc-queue = import ./spsc-queue.nix { inherit pkgs; };

  python-with-packages = pkgs.python3.withPackages (py-pkgs: [
    py-pkgs.websockets
  ]);
in
pkgs.stdenv.mkDerivation
{
  pname = "manet";
  version = "0.0.1";

  src = ./..;

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DMANET_BUILD_TESTING=OFF"
  ] ++ pkgs.lib.optionals useFStack [
    "-DMANET_USE_FSTACK=ON"
  ];

  nativeBuildInputs = with pkgs; [
    cmake
    pkg-config
  ];

  buildInputs = [
    pkgs.doctest
    pkgs.lcov
    pkgs.openssl
    python-with-packages

    sbepp
    spsc-queue
  ] ++ pkgs.lib.optionals useFStack [
    fstack
  ];
}
