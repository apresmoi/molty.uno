#!/bin/sh
# STT backend for `molty listen --stt-cmd`: WAV on stdin -> text on stdout.
# Needs whisper-cpp (`brew install whisper-cpp`) and a ggml model; override
# the model with MOLTY_WHISPER_MODEL.
set -e
model="${MOLTY_WHISPER_MODEL:-$HOME/.cache/whisper/ggml-base.en.bin}"
tmp="$(mktemp -t molty-stt).wav"
trap 'rm -f "$tmp"' EXIT
cat > "$tmp"
whisper-cli -m "$model" -f "$tmp" -nt -np 2>/dev/null | sed 's/^ *//'
