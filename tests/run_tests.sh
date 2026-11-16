#!/usr/bin/env bash
# Offline tests - no audio device needed, runs in a few seconds.
#
# Two suites:
#   test_dsp.cpp       the effects themselves. Links src/amp + src/effects only.
#   test_channels.cpp  the audio callback's channel handling. Needs src/main.cpp, so
#                      that is compiled with -Dmain=amp_main to keep its main() out
#                      of the way (and MINIAUDIO_IMPLEMENTATION comes along with it).
#
# Both built with AddressSanitizer + UBSan so buffer overruns and bad float math get caught.
set -e
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

SAN="-O1 -g -std=c++17 -fsanitize=address,undefined"

# glob the sources the same way the Makefile does, so a new effect file is picked
# up automatically instead of silently failing to link
echo "--- DSP tests ---"
g++ $SAN \
    tests/test_dsp.cpp \
    src/amp/*.cpp src/effects/*.cpp \
    -o "$TMP/test_dsp"
"$TMP/test_dsp"

echo
echo "--- channel-layout tests ---"
g++ $SAN -Dmain=amp_main -c src/main.cpp -o "$TMP/ampmain.o"
g++ $SAN \
    tests/test_channels.cpp "$TMP/ampmain.o" \
    src/amp/*.cpp src/effects/*.cpp \
    -o "$TMP/test_channels" -lpthread -ldl -lm
"$TMP/test_channels"
