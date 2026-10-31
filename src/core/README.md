# Merge state and correspondence

The Qt-free merge engine owns immutable source snapshots, result revisions, passage correspondence, conflict markers, source choices, decision state and undo history. Correspondence.md and EditingCommands.md describe its range and command contracts.

Add value types and deterministic merge or editing rules here. Translate Qt positions, file paths and process results at an adapter boundary instead of adding platform dependencies to the engine.
