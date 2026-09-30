# Audio decoding fixtures

Engine-authored tones for the decoder and streaming tests. Each holds 0.2 s of a
440 Hz sine at half scale, mono at 8000 Hz: 1600 frames whose sample n is
`0.5 * sin(2 * pi * 440 * n / 8000)`.

- `tone.wav` is 16-bit PCM, written with Python's standard `wave` module as
  `round(16384 * sin(2 * pi * 440 * n / 8000))`.
- `tone.flac` is `flac --best --no-padding -o tone.flac tone.wav` (flac 1.5.0).
  It decodes to exactly the samples of `tone.wav`.
- `tone.mp3` is `lame -b 16 tone.wav tone.mp3` (LAME 3.100): MPEG-2.5 Layer III
  at 16 kbit/s. It decodes to 2880 frames, which include the encoder's delay and
  padding.
- `tone.ogg` is `sndfile-convert -vorbis tone-float.wav tone.ogg` (libsndfile
  1.2.2 with libvorbis 1.3.7), where `tone-float.wav` holds the same sine as
  32-bit float samples. Converting the 16-bit file instead doubles its level.
