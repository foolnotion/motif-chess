# Hacking

Here is some wisdom to help you build and test this project as a developer and
potential contributor.

If you plan to contribute, please read the [CONTRIBUTING](CONTRIBUTING.md)
guide.

## Developer mode

Build system targets that are only useful for developers of this project are
hidden if the `motif_DEVELOPER_MODE` option is disabled. Enabling this
option makes tests and other developer targets and options available. Not
enabling this option means that you are a consumer of this project and thus you
have no need for these targets and options.

Developer mode is always set to on in CI workflows.

### Presets

This project makes use of [presets][1] to simplify the process of configuring
the project. As a developer, you are recommended to always have the [latest
CMake version][2] installed to make use of the latest Quality-of-Life
additions.

You have a few options to pass `motif_DEVELOPER_MODE` to the configure
command, but this project prefers to use presets.

As a developer, you should create a `CMakeUserPresets.json` file at the root of
the project:

```json
{
  "version": 2,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 14,
    "patch": 0
  },
  "configurePresets": [
    {
      "name": "dev",
      "binaryDir": "${sourceDir}/build/dev",
      "inherits": ["dev-mode", "vcpkg", "ci-<os>"],
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "dev",
      "configurePreset": "dev",
      "configuration": "Debug"
    }
  ],
  "testPresets": [
    {
      "name": "dev",
      "configurePreset": "dev",
      "configuration": "Debug",
      "output": {
        "outputOnFailure": true
      }
    }
  ]
}
```

You should replace `<os>` in your newly created presets file with the name of
the operating system you have, which may be `win64`, `linux` or `darwin`. You
can see what these correspond to in the
[`CMakePresets.json`](CMakePresets.json) file.

`CMakeUserPresets.json` is also the perfect place in which you can put all
sorts of things that you would otherwise want to pass to the configure command
in the terminal.

> **Note**
> Some editors are pretty greedy with how they open projects with presets.
> Some just randomly pick a preset and start configuring without your consent,
> which can be confusing. Make sure that your editor configures when you
> actually want it to, for example in CLion you have to make sure only the
> `dev-dev preset` has `Enable profile` ticked in
> `File > Settings... > Build, Execution, Deployment > CMake` and in Visual
> Studio you have to set the option `Never run configure step automatically`
> in `Tools > Options > CMake` **prior to opening the project**, after which
> you can manually configure using `Project > Configure Cache`.

### Dependency manager

The above preset will make use of the [vcpkg][vcpkg] dependency manager. After
installing it, make sure the `VCPKG_ROOT` environment variable is pointing at
the directory where the vcpkg executable is. On Windows, you might also want
to inherit from the `vcpkg-win64-static` preset, which will make vcpkg install
the dependencies as static libraries. This is only necessary if you don't want
to setup `PATH` to run tests.

[vcpkg]: https://github.com/microsoft/vcpkg

### Configure, build and test

If you followed the above instructions, then you can configure, build and test
the project respectively with the following commands from the project root on
any operating system with any build system:

```sh
cmake --preset=dev
cmake --build --preset=dev
ctest --preset=dev
```

If you are using a compatible editor (e.g. VSCode) or IDE (e.g. CLion, VS), you
will also be able to select the above created user presets for automatic
integration.

Please note that both the build and test commands accept a `-j` flag to specify
the number of jobs to use, which should ideally be specified to the number of
threads your CPU has. You may also want to add that to your preset using the
`jobs` property, see the [presets documentation][1] for more details.

### File dialog in the Slint app

The Browse buttons beside the database, PGN import and engine path fields open
the operating system's file dialog through [nativefiledialog-extended][nfd]
(NFD). The typed path field is always the fallback. A choice only fills the
field; nothing is submitted.

- **Packaging.** NFD comes from nixpkgs through `find_package(nfd CONFIG)`. On
  Linux `flake.nix` overrides the package to build the xdg-desktop-portal
  backend, static, so the dialog needs a running portal and session bus at
  run time and no GTK or Qt is linked. The dev shell and `nix build .#app` carry
  the override.
- **Failures.** With no session bus or portal, Browse shows the reason NFD gives
  in the error bar. To see it: `DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent
  motif_slint_app`.
- **A dialog cannot be closed from outside.** While one is open every Browse
  button is disabled and the status bar says the dialog may be behind the window.
  Closing the window waits 100 ms for the dialog and then leaves its thread
  behind. With the GTK portal backend the dialog closes with the application
  (the portal drops the requests of a client that leaves the bus).
- **Scripted dialogs for headless runs.** `MOTIF_FILE_PICKER` replaces NFD for
  the process, so a UI check needs no desktop portal:

  | Value | Answer |
  | --- | --- |
  | `choose:<path>` | the dialog returns `<path>` |
  | `cancel` | the user cancels |
  | `fail:<message>` | the dialog cannot open, with `<message>` as the reason |
  | `hang` | the dialog never returns and ignores close, as a real one does |

  Any other value is reported on stderr and ignored. It only fills a text field
  the user still has to submit.
- **Tests.** The host tests use the scripted picker. The NFD adapter's failure
  path runs in `motif_nfd_picker_test`, Linux only, against a session bus that
  does not exist. A real dialog needs a portal on a session bus and is checked by hand (a private
  bus with `xdg-desktop-portal` and its GTK backend works on a headless Sway).
- **Closing the dialog.** The GTK portal backend reports the Cancel button as a
  cancel and Escape as "response code 2" (observed); the adapter treats both as a
  cancel and shows no error. The window's close button is expected to match
  Escape and was not tried.
- **Not verified here.** macOS (NFD runs `NSOpenPanel` on the event-loop thread,
  which blocks the loop while the panel is open) and Windows (no build exists).

[nfd]: https://github.com/btzy/nativefiledialog-extended

### Benchmark corpora

The import and search performance tests honor `MOTIF_IMPORT_PERF_PGN`.

If you prefer a repo-local corpus instead of a machine-local path, use:

```sh
scripts/download_twic_pgns.sh --1m
```

That writes `bench/data/twic-1m.pgn`.

`--1m` now targets approximately 1,000,000 games by counting games in the TWIC
archives it selects; it is no longer based on a fixed number of recent issues.
You can override the target with `--target-games <n>` if you want a larger or
smaller repo-local corpus.

For a faster local guardrail, derive a smaller benchmark subset from the 1M corpus:

```sh
scripts/extract_pgn_subset.sh
```

That writes `bench/data/twic-100k.pgn`.

If you want the full TWIC corpus instead:

```sh
scripts/download_twic_pgns.sh --all
```

When `MOTIF_IMPORT_PERF_PGN` is unset, the performance tests look for
`bench/data/twic-bench.pgn`, then `bench/data/twic-1m.pgn`, before falling back
to the legacy `/data/chess/1m_games.pgn` path.

### Performance workflow

For a quick regression guard after a meaningful change, run:

```sh
scripts/run_perf_benchmarks.sh
```

That uses the 100k corpus and runs:
- the production import fast-path perf test
- the public `position_search` perf test
- the public `opening_stats` perf test

For the heavier 1M benchmark pass, run:

```sh
scripts/run_perf_benchmarks.sh --mode full
```

For all current performance-tagged tests on the 1M corpus, run:

```sh
scripts/run_perf_benchmarks.sh --mode exhaustive
```

Recommended cadence:
- `quick` after local feature work or large refactors
- `full` after major feature completion and before updating `BENCHMARKS.md`
- `exhaustive` when doing dedicated performance work

### Developer mode targets

These are targets you may invoke using the build command from above, with an
additional `-t <target>` flag:

#### `coverage`

Available if `ENABLE_COVERAGE` is enabled. This target processes the output of
the previously run tests when built with coverage configuration. The commands
this target runs can be found in the `COVERAGE_TRACE_COMMAND` and
`COVERAGE_HTML_COMMAND` cache variables. The trace command produces an info
file by default, which can be submitted to services with CI integration. The
HTML command uses the trace command's output to generate an HTML document to
`<binary-dir>/coverage_html` by default.

#### `docs`

Available if `BUILD_MCSS_DOCS` is enabled. Builds to documentation using
Doxygen and m.css. The output will go to `<binary-dir>/docs` by default
(customizable using `DOXYGEN_OUTPUT_DIRECTORY`).

#### `format-check` and `format-fix`

These targets run the clang-format tool on the codebase to check errors and to
fix them respectively. Customization available using the `FORMAT_PATTERNS` and
`FORMAT_COMMAND` cache variables.

#### `run-exe`

Runs the executable target `motif_exe`.

#### `spell-check` and `spell-fix`

These targets run the codespell tool on the codebase to check errors and to fix
them respectively. Customization available using the `SPELL_COMMAND` cache
variable.

[1]: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
[2]: https://cmake.org/download/
