{
  pkgs ? import <nixpkgs> {}
}:

with pkgs;

stdenv.mkDerivation rec {
  name = "midipush";
  src = ./.;
  buildInputs = [
    python
    alsaLib
    fluidsynth
    zlib # for crc32
  ];
  sf = "${soundfont-fluid}/share/soundfonts/FluidR3_GM2-2.sf2";
  shellHook = ''
    start_sf () {
      fluidsynth ${sf} --server
    }
  '';
}
