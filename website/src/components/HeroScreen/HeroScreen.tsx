import { useCueSequence } from "../../hooks/useCueSequence";
import { heroIntro, heroLoop } from "../../features/home/content";
import { MoltyScreen } from "../MoltyScreen";

export function HeroScreen({ fallbackImage }: { fallbackImage?: string }) {
  const cue = useCueSequence(heroIntro, heroLoop);
  return (
    <MoltyScreen
      slot="hero"
      size="hero"
      cue={cue}
      fallbackImage={fallbackImage}
      label="A live Molty screen: a fluid of braille characters where small creatures emerge and dance"
    />
  );
}
