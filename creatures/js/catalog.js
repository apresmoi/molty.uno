// Creature roster: the cute-kit creatures (js/cute) + two procedural ones (js/proc.js).
// tris are measured at load time and shown next to the catalog number.
// `yaw` turns the model so its face points at the camera (+Z).

import { CUTE_CATALOG } from './cute/index.js';

export const CATALOG = [
  // ---- Route 3: cute kit (js/cute/*, one file per creature pack) ----
  ...CUTE_CATALOG,

  // ---- Route 2: procedural, built in code (js/proc.js) ----
  { id: 'mochi', name: 'Mochi', archetype: 'Blobby', route: 'Route 2', kind: 'proc', variant: 'mochi',
    source: 'built in code (this viewer)', license: 'project', tris: null, roster: 'Starter #1 (generator template)' },
  { id: 'burr', name: 'Burr', archetype: 'Spiky', route: 'Route 2', kind: 'proc', variant: 'burr',
    source: 'built in code (this viewer)', license: 'project', tris: null, roster: 'Starter #2' },
];

export const byId = Object.fromEntries(CATALOG.map((c) => [c.id, c]));
