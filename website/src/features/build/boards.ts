// Community builds on /build/: every firmware/boards/**/board.json (folders starting with
// "_" are not boards), with its optional `buy` links and `photos`. Pure, so the unit test
// runs it on the real files; repoBoards.ts feeds it at build time.
import { shopLink } from "../../config/affiliate.mjs";

type ShopLink = ReturnType<typeof shopLink>;

export interface BoardJson {
  name: string;
  title: string;
  url?: string;
  display: { width: number; height: number; shape: string; controller?: string };
  caps?: string[];
  status: string;
  maintainer?: string;
  buy?: Array<{ label: string; url: string }>;
  /** repo-relative paths, e.g. firmware/boards/<vendor>/<model>/photo.jpg */
  photos?: string[];
}

export interface CommunityBoard {
  name: string;
  title: string;
  url?: string;
  display: string;
  extras: string;
  status: string;
  maintainer?: string;
  buy: Array<ShopLink & { label: string }>;
  photos: Array<{ src: string; alt: string }>;
}

const STATUS_ORDER = ["reference", "supported", "experimental"];
const rank = (s: string) => (STATUS_ORDER.includes(s) ? STATUS_ORDER.indexOf(s) : STATUS_ORDER.length);
const EXTRAS: Record<string, string> = {
  touch: "touch",
  imu: "IMU",
  speaker: "speaker",
  mic: "mic",
  battery: "battery",
};

/** `dir` is the board folder under firmware/boards/ ("waveshare/esp32-s3-touch-lcd-1.46");
 *  `photoUrl` turns a repo-relative photo path into a served URL (undefined: not found). */
export function communityBoards(
  entries: Array<{ dir: string; json: BoardJson }>,
  photoUrl: (repoPath: string) => string | undefined,
): CommunityBoard[] {
  return entries
    .filter((e) => !e.dir.split("/").some((part) => part.startsWith("_")))
    .map(({ json: b }) => ({
      name: b.name,
      title: b.title,
      url: b.url,
      display: `${b.display.width}×${b.display.height} ${b.display.shape}${b.display.controller ? ` · ${b.display.controller}` : ""}`,
      extras: (b.caps ?? [])
        .filter((c) => c in EXTRAS)
        .map((c) => EXTRAS[c])
        .join(" · "),
      status: b.status,
      maintainer: b.maintainer,
      buy: (b.buy ?? []).map((l) => ({ label: l.label, ...shopLink(l.url) })),
      photos: (b.photos ?? []).flatMap((p, i) => {
        const src = photoUrl(p);
        return src ? [{ src, alt: `${b.title}, photo ${i + 1}` }] : [];
      }),
    }))
    .sort((a, b) => rank(a.status) - rank(b.status) || a.title.localeCompare(b.title));
}
