# macOS pre-release DMG

Build the Slint application on macOS, then package its native CMake bundle:

```sh
cmake --preset=dev -Dmotif_ENABLE_SLINT_APP=ON
cmake --build build/dev --target motif_slint_app
./packaging/macos/create_dmg.sh \
  build/dev/bin/motif_slint_app.app \
  dist/Motif-Chess.dmg
```

The output is deliberately **unsigned and not notarized**. Gatekeeper can warn
when the copied application is first opened. A user who trusts this pre-release
build must explicitly approve the Gatekeeper override. Do not represent this
artifact as signed or Gatekeeper-trusted.

To inspect the result, mount it, copy `motif_slint_app.app` to `/Applications`
or another application directory, unmount the image, and launch the copied
application. The application bundle declares `.pgn` as
`application/x-chess-pgn`; test Finder opening separately on a real macOS host.
