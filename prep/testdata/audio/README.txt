sine.ogg is a synthetic 440 Hz stereo tone generated for this project using:
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=44100:duration=0.1 -ac 2 -c:a vorbis -strict experimental -q:a 4 sine.ogg
It contains no commercial game assets. The project contributors dedicate this
synthetic test signal to the public domain (CC0-1.0).
