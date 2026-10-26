/*
 * SPDX-License-Identifier: MIT
 *
 * improv_serial.h — Wi-Fi setup over the USB cable for boards whose USB-C port is a
 * USB-UART bridge (MOLTY_CAP_SERIAL, e.g. the Waveshare 1.28): the Improv Wi-Fi Serial
 * protocol on the console UART, next to the log lines. A browser (molty.uno/setup,
 * ESP Web Tools) sends the credentials over the cable; they never touch a server.
 */
#pragma once

/** Start the Improv task when the board has a serial console (a no-op otherwise, and
 *  on builds without a UART console). Call early: a client gives up after ~1.5 s. */
void improv_serial_start(void);
