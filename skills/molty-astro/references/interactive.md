# The interactive chart

`astro.mjs chart --interactive` shows the chart, then keeps listening to Molty's events (the molty
skill's long-poll). A tap on a sign opens its close-up on Molty, and the script reads that sign aloud
while it shows. An empty sign reads its nature and where its ruler is.

It runs until `--ttl` (default 300 s, max 600) or Ctrl-C (which clears the screen): start it in the
background, speak its `SAY` line, and don't talk over a close-up.

`--dry-run --tap gemini` prints what one tap would send.
