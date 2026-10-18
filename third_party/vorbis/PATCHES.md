# Local Vorbis safety patches

Origin: https://github.com/jfreymuth/vorbis, tag v1.0.2.
Go module archive checksum: `h1:m1xH6+ZI4thH927pgKD8JOH4eaGRm18rEE9/0WKjvNE=`.
The upstream MIT license is retained in LICENSE. Source files and go.mod were
copied from the pinned Go module cache; no game assets or upstream test audio
were copied. bounds_test.go contains project-authored synthetic regressions.

The prep converter consumes potentially malformed local game assets and must
return an error without hanging or allocating from unchecked counts. Patches:

- Bit readers fail immediately on exhaustion. Public ReadHeader, Decode and
  DecodeInto recover malformed-input failures into errors. This avoids silently
  synthesizing zero bits indefinitely after truncated data.
- Huffman lookup rejects missing/out-of-bounds branches and depth over 32.
  Single-entry books allocate a minimum two-slot tree. Empty sparse books are
  rejected. Ordered codebook counts/lengths must make bounded valid progress.
- Codebooks cap dimensions at 256, entries at 65536, product at 1 Mi elements,
  and aggregate tree/value allocations at 8 Mi elements per setup header.
- Header packets cap at 1 MiB. Identification bounds channels to 1..8, rates to
  1..384000 Hz and valid 64..8192-sample block sizes. Setup requires an earlier
  identification header. Comment counts are bounded by their packet and 65536;
  framing is checked, and malformed comments return the deferred error.
- Residue parsing rejects end before begin; decoding checks before unsigned
  subtraction so a reversed range cannot request billions of classifications.
- Lookup type 1 multiplicand counts use an exact bounded integer root rather
  than floating-point Pow/Floor, whose rounding can consume the wrong number
  of setup bits and differ across Go/TinyGo builds. Perfect-power regressions
  include cube root of 8 (formerly returned 1 instead of 2).
- Two redundant self-assignments in inverse coupling were removed without
  changing arithmetic, allowing current Go vet to validate the local module.
- Floor0 rejects zero order/rate/map size and amplitude widths over 32, and its
  application loop explicitly stops at the output boundary.
- Floor0 synthesis follows Vorbis I section 6.2.3: the odd-order quarter is
  floating point, coefficient-index loops safely handle order 1, even-order
  factors use cosine rather than cosine squared, and the Bark map divides
  frequency by twice the output length. Packet decoding retains the cumulative
  last LSP coefficient across codebook vectors (section 6.2.2). Synthetic golden
  maps/curves and multi-vector packets in floor0_test.go cover these inherited
  correctness defects; floor0 remains supported.

- IMDCT products and codebook lookup products explicitly round to float32
  before additions, preventing ARM64 compiler FMA contraction. This gives
  native and wasm32 the same arithmetic; a synthetic deterministic IMDCT
  checksum regression fixes the portable result.

Ordinary valid decoding uses the same algorithm with explicit float32 rounding. The prep tests compare a synthetic
Vorbis fixture to the existing stb_vorbis/C packer; the complete pack remains
byte-identical after these patches. Malformed entropy tails are rejected rather
than concealed by implicit zero padding.
