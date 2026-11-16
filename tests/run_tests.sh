#!/usr/bin/env bash
# Offline DSP tests - no audio device needed, runs in a couple of seconds.
#
# The effects now live in their own translation units, so the harness just includes
# src/amp/amp.h and links the same .cpp files the amp does. main.cpp is left out
# because it holds main() and the miniaudio implementation.
#
# Built with AddressSanitizer + UBSan so buffer overruns and bad float math get caught.
set -e
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

g++ -O1 -g -std=c++17 -fsanitize=address,undefined \
    tests/test_dsp.cpp \
    src/amp/amp.cpp src/effects/drive.cpp src/effects/tone.cpp src/effects/memory.cpp \
    -o "$TMP/test_dsp"
"$TMP/test_dsp"
