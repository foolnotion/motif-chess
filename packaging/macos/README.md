# macOS pre-release DMG

Build the Slint application on macOS, then package its native CMake bundle:

```sh
cmake --preset=dev-macos
cmake --build --preset=dev-macos
./packaging/macos/create_dmg.sh \
  build/dev-macos/bin/Debug/motif_slint_app.app \
  dist/Motif-Chess.dmg
```

Before distributing the DMG, inspect its executable's linked libraries:

```sh
otool -L build/dev-macos/bin/Debug/motif_slint_app.app/Contents/MacOS/motif_slint_app
```

Every non-system dependency must resolve after copying the app outside the build
environment. Dependency embedding has not yet been implemented or verified;
until that audit passes on a real Mac, this is a build-environment pre-release
artifact rather than a portable distribution.

The output is deliberately **unsigned and not notarized**. Gatekeeper can warn
when the copied application is first opened. A user who trusts this pre-release
build must explicitly approve the Gatekeeper override. Do not represent this
artifact as signed or Gatekeeper-trusted.

To inspect the result, mount it, copy `motif_slint_app.app` to `/Applications`
or another application directory, unmount the image, and run the copied
application only after the dependency audit above passes. The application bundle
declares `.pgn` as `application/x-chess-pgn`; test Finder opening separately on
a real macOS host.
