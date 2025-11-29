{ pkgs
, src ? {
    rev = "1.7.0";
    sha256 = "sha256-wRsnb5TzepMM6WtApBJ+95Zj2HdZIelDEsf8HV8sNlY=";
  }
,
}:
pkgs.stdenv.mkDerivation {
  pname = "sbepp";
  version = src.rev;

  src = pkgs.fetchFromGitHub {
    owner = "OleksandrKvl";
    repo = "sbepp";
    inherit (src) rev sha256;
  };

  nativeBuildInputs = with pkgs; [ cmake pkg-config ];

  buildInputs = with pkgs; [ fmt pugixml ];
}
