{ pkgs, version ? "v.1.24" }:
let
  src = pkgs.fetchFromGitHub {
    owner = "F-Stack";
    repo = "f-stack";
    rev = version;

    sha256 = "sha256-bHy23pU+CeEF194VgOhB4+Q+AsT3a6lF+75SK30V+TI=";
  };

  dpdk = pkgs.stdenv.mkDerivation {
    pname = "dpdk";
    version = builtins.readFile "${src}/dpdk/VERSION";
    src = "${src}/dpdk";

    nativeBuildInputs = with pkgs; [ meson ninja pkg-config python3 ];
    buildInputs = with pkgs; [ numactl openssl pkgs.numactl ];

    dontConfigure = true;

    buildPhase = ''
      set -euxo pipefail

      meson -Denable_kmods=true --prefix=$out build
      ninja -C build
    '';

    installPhase = ''
      ninja -C build install
    '';
  };

  f-stack = pkgs.stdenv.mkDerivation {
    pname = "f-stack";
    version = version;
    src = src;

    nativeBuildInputs = with pkgs; [ pkg-config gawk gnused ];
    buildInputs = with pkgs; [ dpdk numactl openssl ];

    propagatedBuildInputs = [ dpdk pkgs.openssl pkgs.numactl ];

    dontConfigure = true;

    FF_DPDK = dpdk;
    PKG_CONFIG_LIBDIR = "${dpdk}/lib/pkgconfig";
    NIX_CFLAGS_COMPILE = pkgs.lib.optionalString pkgs.stdenv.isx86_64
      "-march=x86-64-v2 -mtune=generic";

    postPatch = ''
      sed -i 's|^PREFIX=.*|PREFIX='$out'|' lib/Makefile || true
      sed -i 's|^\(PREFIX_[A-Z]*=\)/usr/local|\1''${PREFIX}|' lib/Makefile || true

      sed -Ei 's| -include $FF_DPDK/include/rte_config.h||g' lib/Makefile || true
      sed -Ei 's/-march=native//g' lib/Makefile || true
      sed -Ei '/\ttest -f '"'"'\$\{F-STACK_CONF\}'"'"' \|\|.*/d' lib/Makefile || true
    '';

    buildPhase = ''
      set -euxo pipefail

      pushd lib/
      grep '^PREFIX' Makefile
      grep '^PREFIX_BIN=\$' Makefile

      make EXTRA_CFLAGS="$NIX_CFLAGS_COMPILE" PREFIX=$out
    '';

    installPhase = ''
      mkdir -p $out/{bin,include,lib}
      make install
      cp ../config.ini $out/f-stack.conf

      mkdir $out/lib/pkgconfig
      cat > $out/lib/pkgconfig/fstack.pc <<'EOF'
      prefix=''${pcfiledir}/../..
      exec_prefix=''${prefix}
      libdir=''${prefix}/lib
      includedir=''${prefix}/include

      Name: fstack
      Description: F-Stack user-space TCP/IP stack
      Version: ${version}
      Cflags: -I''${includedir}
      Libs: -L''${libdir} -lfstack
      Requires.private: libdpdk openssl
      EOF
    '';
  };
in
f-stack
