# WaterFlow Firmware

Firmware for the ESP32-S3 water flow monitor built by team **ครูบาช่วยด้วย**.
The board measures water flow with a YF-S201 sensor, shows it on a 0.96" OLED, sends telemetry to ThingsBoard Cloud over MQTT, and raises a LINE alert through the Dormitory Maintenance Reporting System when water keeps flowing for too long.

Related repositories:
- Dashboard and alert service: https://github.com/Toey2706/WaterFlow-Dashboard
- Dormitory Maintenance Reporting System (app, LINE bot, backend): https://github.com/Toey2706/Dorm-care

## Hardware

| Part | ESP32-S3 pin |
|---|---|
| ESP32-S3-WROOM-1-N16R8 | - |
| YF-S201 flow sensor (connector P1, level shifting with R6, R14, Q1) | GPIO7 |
| OLED 0.96" I2C (U7) | SDA GPIO16, SCL GPIO15 |
| RGB LED 5050 (LED1, common anode to 3.3V) | GPIO4, GPIO5, GPIO6 |
| BOOT button | GPIO0 |

LED1 does not light on the first assembled board. The firmware drives it, the fault is in the assembly and is still open.

## What it does

- Counts sensor pulses on GPIO7 with an interrupt, ignoring pulses closer than 2 ms (the YF-S201 cannot go that fast, so they are noise).
- Every second: flow rate = pulses / 7.5 (L/min), total = pulses / 450 (L).
- Shows flow, total and state (FLOWING, idle, LEAK?) on the OLED, plus connection state in the corner.
- Saves the total to flash every 10 seconds when it changes. Hold BOOT for 3 seconds to reset it.
- Publishes `flow_lpm`, `total_l`, `flowing`, `rssi`, `flow_seconds`, `leak` to `v1/devices/me/telemetry` every 5 seconds, and immediately when flow starts or stops.
- Accepts ThingsBoard RPC `resetTotal` and `testAlert`.
- When water flows continuously for `LEAK_SECONDS` (default 600), it calls the `water-alert` function once. That function sends a LINE card to residents bound to `DEVICE_ROOM` and an Android push to dorm admins. It does not create a repair ticket.

## Build

1. Install VS Code with the PlatformIO extension.
2. Copy `include/secrets.example.h` to `include/secrets.h` and fill in Wi-Fi (2.4 GHz only), the ThingsBoard device access token and the device key that matches the `WATER_DEVICE_KEY` secret in Supabase. `secrets.h` is ignored by git.
3. Set the room and leak threshold in `include/config.h`.
4. Keep the project in a folder whose path has no Thai characters, the ESP32 toolchain cannot read it.
5. Upload. If the port is busy, close the serial monitor, or hold BOOT, press RESET, release BOOT, then upload.

## Serial output

```
พัลส์/วินาที=0  อัตราไหล=0.00 L/min  รวม=0.000 L  wifi=3 mqtt=0
```

`wifi=3 mqtt=0` means connected to ThingsBoard. `mqtt=5` is a wrong token, `mqtt=-2` means the server cannot be reached, `wifi=1` means the network was not found (often a 5 GHz network).

## Known limitations

- TLS to the alert service skips certificate checking (`setInsecure`). Fine for the prototype, not for production.
- Flow accuracy has not been calibrated against a known volume of water yet.
