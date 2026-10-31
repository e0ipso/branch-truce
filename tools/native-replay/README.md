# Approved fixture and native replay

Run this tool against an already built desktop app. It creates a disposable real Git merge, clicks the native Qt controls, captures the private Wayland output and checks the observed state. The preload driver is built separately and never linked into production.

```sh
python3 tools/native-replay/replay.py fixture --output /absolute/new-fixture
python3 tools/native-replay/replay.py capture \
  --source /absolute/branch-truce-checkout \
  --build /absolute/desktop-build \
  --output /absolute/new-capture-directory
```

`--source` is the checkout containing the invoked tool. `--build` contains `src/app/branch-truce`. The tool builds its driver and image decoder under the new output directory. Source and app build trees are read only. Output directories must not exist. Run from any working directory by giving the script's absolute path. Prerequisites are Python 3, Git, CMake, a C++20 compiler, Qt 6 development packages for Core/Gui/Quick/Network/Test, sway with headless and pixman support, swaymsg, grim, Breeze Light/Dark colour schemes, and the app's runtime dependencies. Noto Sans and Hack must be installed for the approved typography. No Python packages or existing desktop session are needed.

The default run makes two clean replays per theme at 1280x720, producing eight PNGs and four decoded-pixel comparisons. Every comparison must have zero differing RGBA pixels on the same executable and environment. Cross-platform pixels are not promised. Use `--height 800` for supplemental 1280x800 checks, and `--repeat N` for more clean replays, with N at least two.

The fixture pins 24-line left, 25-line right and 30-line result files, all without a final newline. `reference/provenance.json` records their hashes and the retained originals' hashes from the 2026-10-30 preservation manifest. These copies were verified against that manifest when promoted. The expected result intentionally retains Git conflict markers; exclude that reference file from `git diff --check` and validate its exact bytes against provenance. Generation reads these pinned files, so it needs neither historical session scripts nor surviving temporary files. The PNG is valid, 64x64 RGB and exactly 18,637 bytes. The deterministic main/feature/backoff history starts with three unmerged files. Commit identities come from the generated Git history and are checked against the native pane labels.

Replay accepts the left contribution beginning at line 19, ignores the right contribution beginning at line 20, accepts and stages the left release notes, then exercises Undo and Redo. Native source text must match the pinned bytes. The result must match the exact 30-line reference, with one folded marker block on lines 11 through 17 and a visible gutter jump from 11 to 18. One of three files must be resolved, with the staged release-note bytes checked in Git. Binary Save and Mark resolved must be disabled. The historical incorrect conflict-count label is recorded as observed state, never asserted as the expected contract.

Each theme/repeat directory contains PNGs, text/binary state snapshots, `fixture.json`, app/compositor logs and `cleanup.json`. Snapshots include actual object names, accessible names, native focus, geometry, source text, decisions, folds, generated commit identities and hashes of the fixture manifest, executable, driver and capture. `verification.json` collects comparisons and capture hashes. A failure exits nonzero and retains available diagnostics. Readiness requires the actual private compositor publication, an exposed loaded window, durable file state, completed resolution and settled layout before input. Capture additionally waits for unchanged decoded native window frames, so icon and colour transitions finish even when named control geometry is already stable. Commands use a blocking private socket with deadlines. The tool does not inject model state, replace accessible labels or paint a fake UI.

A per-user nonblocking lock serializes replay. Another active invocation fails before creating output. Each run owns a short private HOME/XDG directory and starts its own headless sway and app process groups. Cleanup waits for those processes and removes that directory, including its compositor output, on success, errors and handled interruption. It never connects to the developer's compositor or removes unrelated outputs. Captures, build outputs and logs remain in the requested output directory. The small lock inode stays in `/tmp` so concurrent callers cannot bypass a waiting owner. An uncatchable process kill cannot guarantee cleanup.

Run focused integrity, readiness, serialization and cleanup checks with:

```sh
python3 tests/tools/NativeReplayTests.py
# Also registered as tooling.native-replay when CMake finds Python 3.
```

## Capture profile and reference comparison

[The calibration notes](../../docs/ui-mockup/native-calibration.md) explain the pinned
profile, actual typography/theme observations, reference raster scale and exception rules.
`capture-profile.json` pins observed style, icon theme, fonts, scale and DPR.
`reference-contract.json` hashes all four approved PNG/HTML pairs and names logical regions.
Changing an approved reference fails before capture. A different platform/profile needs
an explicit reviewed profile change rather than a silent theme exception.

Capture now retains `build-provenance.json`, measured `state.profile`, the actual colour
scheme groups, and a `*-comparison/` directory for each shot. That directory includes
byte-identical original reference/native PNGs, unscaled region close-ups, common-grid
overlays, absolute-channel differences and `comparison.json`. Approved PNGs are 1.5 raster
pixels per logical CSS pixel; native DPR is 1. The comparison grid uses exact reference
x2/native x3 cell replication, with no interpolation or masks. Region differences stay
outstanding, even where the contract lists a conditional narrow exception.

Replay's short private runtime directory is under `/var/tmp`, so it does not consume a
nearly full `/tmp`; the existing small serialization lock remains under `/tmp`.
Run the focused contract and pixel checks after building the external image tool:

```sh
python3 tests/tools/NativeCalibrationTests.py --image-tool /absolute/driver-build/native-replay-image
python3 tools/native-replay/calibration.py --verify /absolute/capture-output
```

Contract checks are also included in `NativeReplayTests.py` and its existing CTest entry.
Verification compares current source/profile/reference inputs and retained artifact hashes,
rechecks the executable and measured profile, and confirms disposable resources were removed.
The integrated gate still owns full fit/reachability and complete suite verification.
