# Targeted optimization checks

Choose the affected checks from `src/openfpga/README.md` and `Makefile`; building
a fixture is not running it. Use the actual produced executable and required
inputs. Shared VM/string/renderer changes need both Undertale `GAME=undertale
WADS=16` and Deltarune Chapter 1 `GAME=deltarune WADS=17` where applicable.
`WADS` is a numeric format version, not an asset directory.

| Change | Relevant checks |
| --- | --- |
| Native writer, tile shift, background lookup | `writer-draw-fixture`, `tile-shift-fixture`, `background-lookup-fixture`; Deltarune executables take the user's Chapter 1 `data.win` path |
| Borrowed strings and bounded search | `string-reads-fixture`, plus shared real builtin behavior/ownership tests |
| Silent audio tail or cache state | `snapshot-tests`: scalar/bulk output and state, cache eviction/reload, metadata allocation failure, seek/pause/gain, failed prepare and repeated restore |
| Focus mode/build identity | `tests/test_focus_build_cache.py` via `tools/test-snapshots.sh`, using an explicit immutable `SDK_ROOT` |
| Diagnostic clocks and packaging | `focus-stats-fixture` and `tests/test_focused_package.py`, plus all five semantic routes |

Use ASan/UBSan where supported and record exclusions. Native fixture comparisons
must cover typed results, owner/copy-on-write behavior, fallback and rejection,
live mutation and bounded interpreter work. A native registration that silently
declines can preserve pixels while losing the entire optimization: retain work
assertions or a negative registration control as well as behavior comparisons.
Keep setup/probe calls outside `assert`; fixture checks must remain active if
compiled with `NDEBUG`. Run the affected fixtures in that configuration when
changing their setup/assertion handling.

For exact audio comparisons, start with the deterministic decoder/mixer fixtures.
The full-game fixed-clock and normalized voice observers in the dated background
results were private test adapters, not public environment switches. Reusing
that approach requires a separately identified and validated capture adapter,
matching seed/route/configuration, and independent comparisons of complete PCM,
pair counts, images and normalized state. Wall-clock dumps are not deterministic.
Desktop throughput cannot prove Pocket speed; returned device benchmarks cannot
prove physical Memory restore.

Follow [save-state verification](save-states.md) for fresh-process and consecutive
restore chains with production policy bypass unset. Separately report normal
save → Analogue Quit → reopen, Memory restore and physical sleep/wake. Preserve
source attachments and progress saves throughout validation.
