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
