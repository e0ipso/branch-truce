---
type: practice
title: Interpret native reference comparisons on the calibrated grid
description: >-
  Keep raster scale, comparison-cell units and capture provenance explicit when
  judging native screenshots against approved references.
tags:
  - testing
  - ui
  - capture
  - calibration
  - provenance
kk_schema_version: 3
kk_id: practice-interpret-native-reference-comparisons-on-the-calibrated-grid
kk_derived_from: []
kk_relates_to:
  - practice-wait-for-rendered-frames-before-capturing-native-scenes
kk_depends_on: []
kk_confidence: high
---
The [reference contract](../../../tools/native-replay/reference-contract.json) records approved rasters at 1.5 pixels per logical pixel. With native DPR 1 in the [capture profile](../../../tools/native-replay/capture-profile.json), compare logical regions on the exact common grid: replicate reference pixels twice and native pixels three times, without interpolation. Difference counts are common-grid cells at three cells per logical pixel, not original native pixels. [Calibration notes](../../../docs/ui-mockup/native-calibration.md#L11) explain these units.

Preserve odd logical origins and extents when aligning regions. [The comparison code](../../../tools/native-replay/calibration.py#L155) trims the replicated reference crop to the logical region; outward-rounded physical crops alone manufacture edge differences. Retain actual unmatched extents rather than stretching a capture to the reference. Keep byte-identical originals alongside diagnostic derivatives so registration errors can be distinguished from application/reference differences.

Retain the recorded profile, actual environment observations and executable provenance with each capture, as required by the [capture documentation](../../../tools/native-replay/README.md#L32). The executable hash identifies the artifact; an observed checkout identity does not establish its compiled revision. Requested colour schemes do not establish the observed palette. Repeat-run pixel equality establishes repeatable rendering on that executable and environment, not agreement with the approved reference. Calibration leaves unexplained visual differences outstanding and does not establish real Plasma or AT-SPI compatibility.

<!-- kk:related:start -->
# Related

- Related: [practice-wait-for-rendered-frames-before-capturing-native-scenes](practice-wait-for-rendered-frames-before-capturing-native-scenes.md)
<!-- kk:related:end -->
