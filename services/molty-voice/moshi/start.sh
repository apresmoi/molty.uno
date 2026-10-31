#!/bin/bash
# Kyutai's start script, except that a Hugging Face token is optional: their models are public.
set -ex

export LD_LIBRARY_PATH=$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"))')

if [ -n "$HUGGING_FACE_HUB_TOKEN" ]; then uvx hf auth login --token "$HUGGING_FACE_HUB_TOKEN"; fi

CARGO_TARGET_DIR=/app/target cargo install --features cuda moshi-server@0.6.4

/root/.cargo/bin/moshi-server "$@"
