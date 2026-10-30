"""Forced alignment (torchaudio MMS_FA, CPU) — runs ON THE BOX, standalone (sent over ssh stdin).

    python - <in.npz>  <  mms_remote.py      ->  JSON on stdout: {"<id>": [[start_s, end_s] | null, ...]}

in.npz: `ids` (JSON list), and per id `pcm_<i>` (int16 @16 kHz) + `words_<i>` (JSON list of
already-normalised words, a-z and '). Same aligner as the box's sync-lab/align.py, which
validated Kyutai's TTS word timing.
"""

import json
import sys

import numpy as np
import torch
import torchaudio


def main(path):
    z = np.load(path)
    ids = json.loads(str(z["ids"]))
    bundle = torchaudio.pipelines.MMS_FA
    model = bundle.get_model(with_star=False).eval()
    tok = bundle.get_tokenizer()
    aligner = bundle.get_aligner()
    out = {}
    for i, key in enumerate(ids):
        words = json.loads(str(z[f"words_{i}"]))
        keep = [k for k, w in enumerate(words) if w]
        res = [None] * len(words)
        x = torch.tensor(z[f"pcm_{i}"].astype(np.float32) / 32768.0).unsqueeze(0)
        if keep and x.shape[1] > 1600:
            with torch.inference_mode():
                em, _ = model(x)
                spans = aligner(em[0], tok([words[k] for k in keep]))
            ratio = x.shape[1] / em.shape[1] / bundle.sample_rate
            for j, k in enumerate(keep):
                res[k] = [round(spans[j][0].start * ratio, 3), round(spans[j][-1].end * ratio, 3)]
        out[key] = res
    json.dump(out, sys.stdout)


if __name__ == "__main__":
    main(sys.argv[1])
