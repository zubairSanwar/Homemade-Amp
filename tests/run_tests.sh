#!/usr/bin/env bash
# Offline DSP tests - no audio device needed, runs in a couple of seconds.
#
# main.cpp is one file with its own main(), so we can't just #include it. Instead we
# slice out everything above audio_process (which is all the effect structs) and
# compile that against the harness. Nothing in src/ has to change.
#
# Built with AddressSanitizer + UBSan so buffer overruns and bad float math get caught.
set -e
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

sed -n '1,/^void audio_process/p' src/main.cpp | sed '$d' \
  | grep -v "MINIAUDIO_IMPLEMENTATION\|miniaudio.h" > "$TMP/structs.inc"

g++ -O1 -g -std=c++17 -fsanitize=address,undefined -I"$TMP" \
    tests/test_dsp.cpp -o "$TMP/test_dsp"
"$TMP/test_dsp"
