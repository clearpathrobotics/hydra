with import ./config.nix;
{
  empty_dir =
    mkDerivation {
      name = "empty-dir";
      builder = ./empty-dir-builder.sh;
      meta.maintainers = [ "alice@invalid.org" ];
      meta.outPath = "${placeholder "out"}";
    };

  fails =
    mkDerivation {
      name = "fails";
      builder = ./fail.sh;
    };

  succeed_with_failed =
    mkDerivation {
      name = "succeed-with-failed";
      builder = ./succeed-with-failed.sh;
    };
}
