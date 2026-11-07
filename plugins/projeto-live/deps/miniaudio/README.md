# miniaudio 0.11.22

`miniaudio.inc` is `miniaudio.h` copied unchanged from https://github.com/mackron/miniaudio
(tag 0.11.22, commit 350784a9467a79d0fa65802132668e5afbcf3777). License: public domain
(Unlicense) or MIT No Attribution, at your choice (see LICENSE).

It has a different extension only so OBS's clang-format check skips it: clang-format cannot
finish on a 4 MB file with this many preprocessor branches, and third-party code should stay as
it came.

Projeto Live uses only the device input/output part (see intercom-audio.c) to play and record the
director's intercom headset outside of OBS's audio mix.
