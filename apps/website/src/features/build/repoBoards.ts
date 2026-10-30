// The real board.json files and board photos from device/firmware/boards/, read at build time
// (Vite glob: photos ship as hashed assets). See boards.ts for the shape.
import { communityBoards, type BoardJson } from "./boards";

const ROOT = "../../../../../";
const jsons = import.meta.glob<BoardJson>("../../../../../device/firmware/boards/**/board.json", {
  eager: true,
  import: "default",
});
const photos = import.meta.glob<string>("../../../../../device/firmware/boards/**/*.{jpg,jpeg,png,webp}", {
  eager: true,
  query: "?url",
  import: "default",
});

const boardsDir = (key: string) => key.slice(`${ROOT}device/firmware/boards/`.length).replace(/\/board\.json$/, "");

export const COMMUNITY_BOARDS = communityBoards(
  Object.entries(jsons).map(([key, json]) => ({ dir: boardsDir(key), json })),
  (repoPath) => photos[`${ROOT}${repoPath}`],
);
