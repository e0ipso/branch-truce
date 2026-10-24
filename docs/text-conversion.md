# Source conversion and result format

`git::TextConversion` exposes asynchronous source reading, result preparation and
publication verification. Every outcome carries the request's session generation
and input revision. A failed or cancelled conversion exposes no accepted text.
The controller must fence those identities and retain editable buffers on failure.

## Supported envelope

- UTF-8, with or without an initial BOM. Unattributed text must be valid UTF-8.
- Explicit `working-tree-encoding=UTF-8`, `UTF-16LE`, `UTF-16BE` or `UTF-16`.
  Encoding names are case insensitive and permit omission of hyphens.
  Git requires a BOM for `UTF-16` and prohibits it for explicit LE/BE encodings.
  `UTF-16` retains a captured result's byte order, defaulting to little endian
  when an explicit attribute changes a UTF-8 result to UTF-16.
- Uniform LF or CRLF separators, including empty files and files without a final
  newline. Editable buffers contain UTF-8 with LF separators. The buffer's final
  newline is serialized exactly; metadata never appends one implicitly.
- Git's built-in `text`, `text=auto`, `-text`, `eol=lf`, `eol=crlf`, `ident` and
  working-tree-encoding conversions, subject to exact clean/checkout validation.
  Effective `core.autocrlf`, `core.eol`, `core.safecrlf`,
  `core.checkRoundtripEncoding` and `core.filemode` values are captured with
  the relevant attributes. The attributes-file setting and filter-driver
  clean/smudge/process/required settings are retained to detect policy changes.
  Explicit EOL attributes take precedence over the
  launch result's newline format. An explicit encoding selects that encoding;
  UTF-8 preserves an existing UTF-8 BOM. Otherwise preserve the launch result's
  format. A source side's encoding or newline format never chooses the result's.
- Agreed regular-file modes `100644` and `100755`. Both stages 2 and 3 must exist
  at the exact byte path and agree on mode. Stage 1 is optional and must be a
  regular file when present. Capture its own mode separately from the result's.

Reject invalid UTF-8, truncated or unpaired UTF-16, unsupported encodings,
NUL/other binary control characters, bare CR, mixed LF/CRLF, contradictory
attributes and byte-changing round trips with an explicit diagnostic. Tabs are
supported. An initial encoding BOM is metadata, not editable text. Ambiguous BOMs
that cannot reproduce exactly are rejected.

Git's actual checkout policy must reproduce the serialized bytes exactly.
For example, a preserved LF result under `core.autocrlf=true` can require an
explicit EOL attribute. Expanded `ident` tokens must match the resulting object
ID. An edit that makes an expanded token stale is rejected because checkout
would change the reviewed text. Refresh and review that representation before
attempting publication. Textconv is never an editing input. Custom external
filters, hooks affecting staging, partial/promisor repositories and other denied
helpers remain subject to the adapter's existing policy.

## Conversion and verification contract

For each source, supply the exact path, stage number, object ID and captured
unmerged entries. The pipeline retains raw canonical bytes and identity,
materializes Git checkout bytes under the helper policy, decodes strictly and
proves that clean conversion returns the captured canonical object and bytes.
It also proves that checkout and codec serialization reproduce the source bytes.

For a result, supply the launch result's `storage::TextFormat`, edited UTF-8 and
the captured stages. Preparation serializes the selected format and asks Git to
clean it, read the canonical blob and check it out again. These operations use
an app-owned temporary object store with the repository's validated local object
store as an alternate. No worktree, index or repository object is written by
preparation. Private objects are removed before completion, including on failure
or cancellation. Existing adapter time and input limits apply. Canonical blob and
checkout reads each have a separate 8 MiB output budget shared by stdout and
stderr. Earlier preflight output does not reduce either read's allowance.

The prepared snapshot binds the worktree root, exact byte path, displayed text,
serialized bytes, canonical bytes/object ID, policy and agreed executable mode.
Relevant configuration and attributes are compared across requests and checked
again before accepting the result. The publication coordinator must revalidate
operation, index and destination baselines, retain the reviewed revision, write
with the prepared mode and bytes, and arrange app-owned or parent-owned staging.
Those publication actions remain separate from conversion.

`VerifyPublished` checks the exact stage-zero path, canonical object ID and mode,
reads its canonical bytes, checks its checkout form against the prepared bytes,
then reinspects policy and index. Only that successful verification is evidence
that Git holds the prepared result. The low-level `GitAdapter` read/stage methods
alone do not establish text support or successful publication.
