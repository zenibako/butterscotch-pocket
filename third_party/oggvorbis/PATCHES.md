# Local Ogg/Vorbis page-position patch

Origin: https://github.com/jfreymuth/oggvorbis, tag v1.0.5.
Go module archive checksum: `h1:u+Ck+R0eLSRhgq8WTmffYnrVtSztJcYrl588DM4e3kQ=`.
The upstream MIT license is retained in LICENSE. Production Go sources, module
metadata and README were copied from the pinned module cache. No upstream audio
fixtures or commercial game assets were copied.

`Reader.read` anchors its running PCM position to each completed, non-EOS audio
page's granule. Previously only initialization and EOS used page positions.
At long/short window transitions, a page's completed PCM endpoint can differ
from its cumulative returned sample count. Ignoring that position causes EOS
to retain an extra partial block. EOS discard is clamped to the current packet length: earlier samples have
already been delivered, and a whole final packet may lie beyond the EOS
position. This prevents negative read counts and matches streaming C/stb
behavior. The existing initialization path still owns
initial-page trimming; the new anchoring starts after initialization.

The prep package's synthetic `TestAudioVorbisEndUsesPreviousPagePosition`
regression repages the project-authored tone and matches the C/stb reference's
4992-frame result and its whole-final-packet discard case matches 4096 frames. `TestAudioVorbisGranuleBeyondDecodedSamples` preserves support
for complete single-page streams with a positive PCM end-position offset.
