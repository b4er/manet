{ pkgs
, src ? {
    rev = "1053918dbd251fbff69b24ef27fa5d51c29ec2af";
    sha256 = "sha256-4ER3vmIgpNDL5m1I6dUGNB6yhJu9GU//u6v3HraUMXI=";
  }
,
}:
pkgs.stdenv.mkDerivation {
  pname = "SPSCQueue";
  version = src.rev;

  src = pkgs.fetchFromGitHub {
    owner = "rigtorp";
    repo = "SPSCQueue";
    inherit (src) rev sha256;
  };

  nativeBuildInputs = with pkgs; [ cmake ];

  # cmakeFlags = [ "-DCMAKE_INSTALL_PREFIX=${placeholder "out"}" ];
  # patchPhase = "patchShebangs .";
  # configurePhase = "./configure.py --mode=release --prefix=$out";
  # buildPhase = "ninja -C build/release";
  # installPhase = "cmake --install build/release";
}
