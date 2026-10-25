// SPDX-License-Identifier: MIT
// Licences: Molty's own licence + a short summary of the third-party components in
// the firmware image and this console. Full text: THIRD_PARTY_NOTICES.md in the repo.
import { h, mount } from '../ui/dom';
import type { Page } from '../ui/dom';

const ITEMS: Array<[string, string]> = [
  ['Molty (firmware, console, relay, CLI, creatures)', 'MIT — Copyright (c) 2026 Juan Cruz Fortunatti and Molty contributors'],
  ['ESP-IDF, Mbed TLS, esp_tinyusb, esp_websocket_client, mdns, Wi-Fi/PHY libraries, esp_lcd_spd2010', 'Apache-2.0 — Espressif Systems and contributors'],
  ['TinyUSB, FreeRTOS, cJSON, SPIFFS, http_parser', 'MIT'],
  ['lwIP, wpa_supplicant', 'BSD-3-Clause'],
  ['Newlib', 'BSD-style licences'],
  ['Mozilla CA certificate bundle', 'MPL-2.0'],
  ['glyphcss ports (5x7 font, ramp, speed-reader ORP, field synth)', 'MIT — © 2025 Layoutit'],
  ['three.js r170 (loaded from a CDN, not bundled)', 'MIT — three.js authors'],
  ['Vite runtime helper', 'MIT — VoidZero Inc. and Vite contributors'],
];

export const licencesPage: Page = (root) => {
  mount(root,
    h('h1', null, 'licences'),
    h('p', { class: 'dim' }, 'Full notices: THIRD_PARTY_NOTICES.md in the Molty repository.'),
    ...ITEMS.map(([what, lic]) => h('div', { class: 'card' }, h('b', null, what), h('br'), h('span', { class: 'dim' }, lic))));
};
