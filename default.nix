{
  pkgs ? import <nixpkgs> {}
}:

with pkgs;

stdenv.mkDerivation {
  name = "midipush";
  src = ./.;
  buildInputs = [ rtmidi ];
}
