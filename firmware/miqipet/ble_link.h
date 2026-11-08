// Bluetooth LE input: the same NDJSON lines as the serial port, written to one GATT
// characteristic, so the board can sit on any USB charger away from the computer.
// See docs/superpowers/specs/2026-11-05-ble-transport-design.md.
#pragma once
#include <stddef.h>

#define BLE_SERVICE_UUID "ee97aa6d-6973-4f5a-9148-c26f84eb0f84"
#define BLE_RX_UUID      "f91d93a9-1bf8-4ae7-b4ac-31fcaa05cebc"  // write / write without response

void bleBegin();
// Bytes received since the last call, in order; returns how many were copied. Lines may be
// split across writes and calls: the caller reassembles them like serial input.
size_t bleRead(char* buf, size_t cap);
bool bleConnected();
