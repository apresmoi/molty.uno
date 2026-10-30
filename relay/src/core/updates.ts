// SPDX-License-Identifier: MIT
// "Is Molty up to date?": pure functions over what the relay knows about the
// device (hello: fw, board, caps) and the latest release (core/releases.ts).
// Used by molty_updates, molty_status (relay.firmware), the one-time
// firmware_update notice, molty_update and the firmware_too_old error.

import type { JsonObject } from "./protocol.js";
import type { ReleaseInfo, ReleaseLookup, ReleaseState } from "./releases.js";
import { FEATURES, type FeatureId, compareVersions, parseVersion } from "./version.js";

/** What the relay knows about the device (live socket, or the last hello when offline). */
export interface DeviceFacts {
  fw: string | null;
  board: string | null;
  /** null = the device sent no caps (firmware before caps existed). */
  caps: string[] | null;
  online: boolean;
}

/**
 * current: runs the latest release · update_available: a newer release has an image for this board ·
 * no_build_for_board: a newer release exists but not for this board · no_release: nothing published yet ·
 * unknown: the device's version or the release check is unknown.
 */
export type FirmwareStatus = "current" | "update_available" | "no_build_for_board" | "no_release" | "unknown";

export interface NeededFor {
  /** Agent tool the feature is used through. */
  feature: string;
  what: string;
  needs: string;
  /** A published release already ships it. */
  released: boolean;
}

export interface FirmwareBlock {
  current: string | null;
  board: string | null;
  online: boolean;
  latest: string | null;
  status: FirmwareStatus;
  update_available: boolean;
  published_at: string | null;
  notes_url: string | null;
  needed_for: NeededFor[];
  /** The device advertises the `update` cap: molty_update works. */
  can_self_update: boolean;
  release_check: ReleaseState;
}

/** Where a human updates by hand (every firmware with /api/ota). */
export const CONSOLE_HINT = "open Molty's console (http://molty.local, or http://10.13.37.1 over USB) → home";

export function firmwareBlock(d: DeviceFacts, lookup: ReleaseLookup): FirmwareBlock {
  const r = lookup.release;
  const known = !!parseVersion(d.fw);
  const cmp = r && known ? compareVersions(r.version, d.fw)! : undefined;
  const asset = r && d.board ? r.boards[d.board] : undefined;
  let status: FirmwareStatus;
  if (lookup.state === "unavailable" || !known) status = "unknown";
  else if (!r) status = "no_release";
  else if (cmp! <= 0) status = "current";
  else status = asset ? "update_available" : "no_build_for_board";
  const needed_for: NeededFor[] = [];
  if (known) {
    for (const f of Object.values(FEATURES)) {
      if ((compareVersions(d.fw, f.min_fw) ?? 0) >= 0) continue;
      needed_for.push({ feature: f.tool, what: f.what, needs: f.min_fw, released: !!r && (compareVersions(r.version, f.min_fw) ?? -1) >= 0 });
    }
  }
  return {
    current: d.fw,
    board: d.board,
    online: d.online,
    latest: r?.version ?? null,
    status,
    update_available: status === "update_available",
    published_at: r?.published_at ?? null,
    notes_url: r?.notes_url ?? null,
    needed_for,
    can_self_update: !!d.caps?.includes(FEATURES.update.cap),
    release_check: lookup.state,
  };
}

/** How the human gets firmware `target` onto this device, in one sentence. */
export function howToUpdate(fw: FirmwareBlock, target: string): string {
  if (fw.can_self_update) return `call molty_update (REST POST /v1/devices/<id>/update) after your human agrees; they confirm on Molty's screen too`;
  return `this firmware can't update itself: tell your human to ${CONSOLE_HINT}, which shows firmware ${target} and installs it`;
}

/**
 * `firmware_too_old` details when `feature` is missing because the device runs older
 * firmware than the feature needs; undefined when the version does not explain it
 * (then the board lacks the hardware, or the version is unknown: `unsupported`).
 */
export function tooOld(feature: FeatureId, d: DeviceFacts, lookup: ReleaseLookup): JsonObject | undefined {
  const f = FEATURES[feature];
  const cmp = compareVersions(d.fw, f.min_fw);
  if (cmp === undefined || cmp >= 0) return undefined;
  const fw = firmwareBlock(d, lookup);
  const released = !!fw.latest && (compareVersions(fw.latest, f.min_fw) ?? -1) >= 0;
  const how = released
    ? `tell your human ${f.tool} needs Molty firmware ${f.min_fw} (it runs ${d.fw}); firmware ${fw.latest} is out: ${howToUpdate(fw, fw.latest!)}`
    : `tell your human ${f.tool} needs Molty firmware ${f.min_fw} (it runs ${d.fw}), which is not released yet; molty_updates shows when it is`;
  return { feature: f.tool, current: d.fw, needed: f.min_fw, latest: fw.latest, how };
}

/**
 * Plain-language firmware steps for the agent, in order. The agent skill is not the
 * relay's business: it updates with the molty plugin (skills/molty/references/updates.md).
 */
export function updateActions(fw: FirmwareBlock): string[] {
  const out: string[] = [];
  switch (fw.status) {
    case "update_available":
      out.push(
        fw.can_self_update
          ? `Ask your human: "Molty firmware ${fw.latest} is out (Molty runs ${fw.current}). Update now?" If yes, call molty_update; Molty asks them to confirm on its screen too, then restarts (about a minute).`
          : `Tell your human Molty firmware ${fw.latest} is out (Molty runs ${fw.current}). This firmware can't update itself: they ${CONSOLE_HINT}, which shows the update and installs it.`,
      );
      break;
    case "current":
      out.push(`Molty's firmware ${fw.current} is the latest release.`);
      break;
    case "no_build_for_board":
      out.push(`Firmware ${fw.latest} is out, but not for this board (${fw.board ?? "unknown"}): nothing to install yet.`);
      break;
    case "no_release":
      out.push(`No firmware release is published yet; Molty runs ${fw.current}.`);
      break;
    case "unknown":
      out.push(
        fw.release_check === "unavailable"
          ? "Couldn't reach the release list just now: say so, and check again later."
          : fw.current
            ? `Molty runs a development build (${fw.current}): it can't be compared with releases.`
            : "Molty hasn't connected to the relay yet, so its firmware is unknown.",
      );
      break;
  }
  const blocked = fw.needed_for.filter((n) => n.feature !== FEATURES.update.tool);
  for (const n of blocked) {
    out.push(
      n.released
        ? `${n.feature} (${n.what}) needs firmware ${n.needs}: mention that updating unlocks it.`
        : `${n.feature} (${n.what}) needs firmware ${n.needs}, not released yet: don't offer it until then.`,
    );
  }
  if (!fw.online && fw.current) out.push("Molty is offline right now: updates wait until it is back.");
  return out;
}

/** The release's OTA image for `board`, if it can be installed (has an image and a checksum). */
export function installable(r: ReleaseInfo | null, board: string | null) {
  const a = r && board ? r.boards[board] : undefined;
  return a && a.sha256 ? a : undefined;
}
