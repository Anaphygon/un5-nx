# The PS2Recomp path (archived)

Phases 1 and 2 were built on [PS2Recomp](https://github.com/ran-j/PS2Recomp).
They took the game from 51,127 recompile errors to its title screen and the
first overlay load, and stopped there: PS2Recomp translates the executable
once, and UN5 runs most of its code from overlays loaded over one address.
Phase 3 moved to [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp),
which captures and translates those overlays (see [`docs/PHASE3.md`](../../docs/PHASE3.md)).

The tag `ps2recomp-final` marks the last commit on this path. What is kept:

- `patches/`: the 23 fixes to PS2Recomp, each with a test, written to be sent
  upstream. They apply to PS2Recomp `e563698`.
- `tools/setup_ps2recomp.sh`: clones PS2Recomp, applies the patches, builds
  the recompiler and analyzer.
- `tools/fixmap.py`, `tools/build_mac.sh`: turn the Ghidra function map into
  PS2Recomp's TOML and build the runtime around the generated code.
- `tools/debug-traces.diff`: the temporary traces used while debugging.

The steps are in [`docs/PHASE1.md`](../../docs/PHASE1.md) and
[`docs/PHASE2.md`](../../docs/PHASE2.md).
