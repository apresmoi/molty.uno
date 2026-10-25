import { useEffect, useState } from "react";
import type { CueStep, ScreenCue } from "../services/molty-stage";

/** Walk `intro` once, then loop `loop` forever. Returns the current cue. */
export function useCueSequence(intro: CueStep[], loop: CueStep[]): ScreenCue {
  const steps = [...intro, ...loop];
  const [index, setIndex] = useState(0);
  useEffect(() => {
    const step = steps[index];
    if (!step) return;
    const timer = setTimeout(() => {
      const next = index + 1;
      setIndex(next < steps.length ? next : intro.length);
    }, step.seconds * 1000);
    return () => clearTimeout(timer);
  }, [index]); // eslint-disable-line react-hooks/exhaustive-deps -- steps are static per mount
  return steps[index]?.cue ?? { kind: "blob" };
}
