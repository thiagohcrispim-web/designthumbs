# Bundled Inkscape

Vizu distributes an unmodified portable Inkscape 1.4.2 Windows x64 package as a separate executable engine. Vizu invokes it through the command line.

Original distribution: https://media.inkscape.org/dl/resources/file/inkscape-1.4.2_2025-05-13_f4327f4-x64.7z

SHA-256: `757e0358512630e65505a4cca89885369087f2908469439bc41b5a346604cbd4`

Upstream project and corresponding sources: https://inkscape.org/release/inkscape-1.4.2/source/ and https://gitlab.com/inkscape/inkscape/-/tree/INKSCAPE_1_4_2

Inkscape and its dependencies retain their own licenses. The full original package is preserved, including `inkscape/share/inkscape/doc/COPYING`, `LICENSE`, GPL/LGPL/MPL/OFL texts and other upstream notices. Vizu's MIT license does not replace these licenses.

The build downloads the archive and verifies its SHA-256 before embedding it. End-user installation is offline; the installed engine is private to Vizu and is removed with Vizu.
