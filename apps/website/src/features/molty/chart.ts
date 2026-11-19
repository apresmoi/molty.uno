// bake: the astrology package's birth chart, from the firmware's own sample commands (the ones
// its host tests play). Its planets, signs and lines are spots a tap reads.
import commands from "../../../../../device/firmware/test/host/fixtures/astrology-commands.jsonl?raw";
import { sampleScene } from "./actions";
import type { Program } from "../../services/molty-screen";

export const CHART: Program = {
  kind: "scene",
  layout: "chart",
  params: sampleScene(commands, "chart").params,
  ttl: 40,
  after: 6,
  rest: 8,
};
