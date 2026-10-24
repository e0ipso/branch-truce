# Conversion fixtures

`ConversionCases.h` supplies literal expected bytes to the existing
`GitAdapterTests` class. Its accepted rows specify canonical source bytes, source
checkout bytes, displayed UTF-8, launch format, edited UTF-8, serialized result
bytes, canonical result bytes, result format and agreed Git mode. Defaults spell
the ASCII source `s\n`, edit `r\n` and mode `100644`. Unicode rows use `Aé🙂`,
including a UTF-16 surrogate pair, with tabs and explicit final-newline choices.
Expected bytes never come from the production serializer.

Run the local conversion entry point after preparing the desktop build:

```sh
./tools/prepare-worktree.sh desktop
cmake --build build/desktop --parallel 2
ctest --test-dir build/desktop --output-on-failure -V -R '^(git.adapter|storage.text-format)$'
```

The CTest log prints every accepted `FORMAT` and rejected source, attribute,
edit and round-trip row, followed by test and assertion counts. The adjacent
`storage.text-format` suite also checks malformed UTF-16, required/prohibited
BOMs and the codec's full encoding/newline/final-newline combinations.

## Accepted matrix

The 26 data rows cover these explicit combinations:

- Unattributed UTF-8 LF, CRLF and BOM; each preserves the launch format.
- `text eol=lf` overrides CRLF launch metadata; `text=auto eol=crlf` emits CRLF
  while staging LF; `-text` retains CRLF in the canonical blob.
- Explicit UTF-8 with BOM and CRLF retains the BOM in worktree and canonical
  bytes, with LF in the canonical blob.
- Explicit UTF-16LE emits no BOM and LF. Explicit UTF-16BE emits no BOM and
  CRLF. Generic UTF-16 emits the checkout's little-endian BOM and LF.
- `core.autocrlf=true` with explicit CRLF, `core.eol=crlf` with `text`,
  `core.autocrlf=input` with `text=auto`, and `-text` overriding autocrlf.
- Unicode UTF-8, UTF-8 BOM/CRLF, and UTF-16LE without a final newline.
- Unicode UTF-16BE/CRLF and generic UTF-16 with BOM and final LF.
- Explicit UTF-16LE or UTF-8 replaces big-endian UTF-16 launch metadata.
- Empty UTF-8, BOM-only UTF-8, and empty UTF-16LE acquire no final newline,
  even when launch metadata records one.
- Adding a final newline takes effect despite launch metadata without one.
- A CRLF source with an LF launch result retains the result's LF choice.

Every row starts with real unmerged stage-2/stage-3 entries. Preparation must
leave the index and worktree unchanged. The test writes the reviewed bytes,
stages that exact path, and checks the complete stage-zero record against a
separately hashed expected canonical blob. `VerifyPublished`, `git show` and
the worktree bytes must agree with the fixture. Executable Unicode rows use
`100755`; the other rows use `100644`. The existing mode test covers both modes
and rejects disagreement.

A further accepted `ident` case verifies displayed expansion, serialized
expansion, the staged `$Id$` canonical token and its expected object ID.

## Rejected matrix

- Ten source rows reject malformed UTF-8, NUL, binary controls, DEL, bare CR,
  mixed separators, and canonical CRLF that cannot round-trip under LF attributes.
- Six attribute rows reject `-text eol=crlf`, custom text/ident values, native
  EOL, ISO-8859-1 and UTF-32, for both source reading and result preparation.
- Ten edited-buffer rows reject malformed UTF-8, binary controls, CR, CRLF,
  mixed separators and an initial BOM in editable text.
- LF preservation under autocrlf and a big-endian UTF-16 launch that Git checks
  out in little endian reject because checkout changes the reviewed bytes.
- Editing an expanded `ident` token's document makes its embedded object ID
  stale. Preparation rejects it and leaves the previously staged token intact.

Rejected rows expose a diagnostic and no accepted snapshot. They leave the
index unchanged; edited-buffer cases also check the original worktree bytes.
All conversion fixtures explicitly remove their temporary repository and
executable directories and check that both paths are gone.

These tests use the existing conversion and helper policy documented in
[`docs/text-conversion.md`](../../../docs/text-conversion.md). Guarded filesystem
publication and the shared canonical-output budget have separate coverage.
