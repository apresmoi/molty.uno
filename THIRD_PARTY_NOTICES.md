# Third-party notices

Molty is MIT-licensed (see LICENSE). The firmware image and the device console
include the third-party components below, under their own licences. This file
must accompany every distributed firmware binary.

## Firmware binary (ESP32-S3, built with ESP-IDF v5.5)

### Apache License 2.0 — full text: https://www.apache.org/licenses/LICENSE-2.0.txt
- ESP-IDF v5.5 — © 2015-2025 Espressif Systems (Shanghai) Co., Ltd. https://github.com/espressif/esp-idf
- Espressif Wi-Fi / PHY / coexistence binary libraries (esp_wifi, esp_phy, esp_coex) — © Espressif Systems. Provided in object form only, under Apache-2.0.
- Mbed TLS 3.6.3 — © The Mbed TLS Contributors / ARM Limited.
- esp_tinyusb 1.7.6, esp_websocket_client 1.8.0, mdns 1.11.1 — © Espressif Systems.
- esp_lcd_spd2010 driver (esp_lcd_spd2010.c/.h) — © 2023 Espressif Systems, unmodified.

### MIT License
- TinyUSB 0.21.0 — © 2012-2026 Ha Thach (tinyusb.org) and contributors
- lrndis-derived rndis_reports.c — © 2015 Sergey Fetisov
- FreeRTOS Kernel — © Amazon.com, Inc. or its affiliates
- cJSON 1.7.18 — © 2009-2017 Dave Gamble and cJSON contributors
- SPIFFS — © 2013-2017 Peter Andersson
- http_parser — © Joyent, Inc. and other Node contributors (based on NGINX, © Igor Sysoev)
- Xtensa HAL headers — © 2003-2013 Tensilica Inc.
- glyphcss (ports of its 5x7 font, shading ramp, speed-reader ORP and field synth) — © 2025 Layoutit

    Permission is hereby granted, free of charge, to any person obtaining a copy of this
    software and associated documentation files (the "Software"), to deal in the Software
    without restriction, including without limitation the rights to use, copy, modify, merge,
    publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
    to whom the Software is furnished to do so, subject to the following conditions:
    The above copyright notice and this permission notice shall be included in all copies or
    substantial portions of the Software.
    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
    INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
    PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
    FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
    OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

### BSD 3-Clause License
- lwIP — Copyright (c) 2001, 2002 Swedish Institute of Computer Science. All rights reserved.
- wpa_supplicant — Copyright (c) 2003-2022 Jouni Malinen <j@w1.fi> and contributors.
- FreeBSD net80211 — Copyright (c) 2004-2008 Sam Leffler, Errno Consulting.
- STM32 DWC2 register definitions (TinyUSB dwc2_type.h) — Copyright (c) 2019 STMicroelectronics.

    Redistribution and use in source and binary forms, with or without modification, are
    permitted provided that the following conditions are met:
    1. Redistributions of source code must retain the above copyright notice, this list of
       conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice, this list of
       conditions and the following disclaimer in the documentation and/or other materials
       provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its contributors may be used
       to endorse or promote products derived from this software without specific prior
       written permission.
    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
    OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
    EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
    TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### Newlib C library
BSD-style licences, copyright of various parties — full list:
https://github.com/espressif/esp-idf/blob/v5.5/components/newlib/COPYING.NEWLIB

### CC0 1.0
- Fast PBKDF2 (wpa_supplicant crypto glue) — Joseph Birr-Pixton
- Portions of the ESP-IDF tusb_ncm example (Unlicense OR CC0-1.0) — Espressif Systems

### Mozilla CA certificate bundle (MPL-2.0)
The firmware embeds, unmodified, the Mozilla root CA list as packaged by ESP-IDF
(components/mbedtls/esp_crt_bundle/cacrt_all.pem, from Mozilla NSS certdata.txt).
Licence: https://www.mozilla.org/MPL/2.0/

### Hardware driver credits
Touch (SPD2010), IMU (QMI8658) and I/O-expander (TCA9554) drivers are
re-implementations whose register sequences were derived from Waveshare's
ESP32-S3-Touch-LCD-1.46 demo code and the respective vendor datasheets.

## Device console (single-file HTML, also stored in the firmware SPIFFS image)
- Vite runtime helper — © 2019-present VoidZero Inc. and Vite contributors, MIT.
- three.js r170 — © 2010-2024 three.js authors, MIT. Loaded at runtime from
  https://cdn.jsdelivr.net/npm/three@0.170.0/ (not bundled).
- glyphcss field synth (port) — © 2025 Layoutit, MIT (text above).

## molty CLI (Go binary)
- github.com/coder/websocket v1.8.15 — Copyright (c) 2025 Coder. ISC License:
    Permission to use, copy, modify, and distribute this software for any purpose with or
    without fee is hereby granted, provided that the above copyright notice and this
    permission notice appear in all copies.
    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO
    THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT
    SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR
    ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
    CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
    OR PERFORMANCE OF THIS SOFTWARE.

## Hosted services (no code licence; terms of use apply)
The optional cloud relay calls Cloudflare Workers AI models @cf/deepgram/aura-2-en
(TTS) and @cf/openai/whisper-large-v3-turbo (STT), governed by Cloudflare's and the
model providers' terms.
