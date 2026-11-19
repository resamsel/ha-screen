# ha-screen

Home Assistant display for the Waveshare ESP32-C6-Touch-LCD-1.47.
Settings (HA URL, entities, poll interval, graphs) are in `main/config.h`.

## Secrets

Wi-Fi credentials and the HA token are read from environment variables **at build time**
and compiled into the firmware. The build stops if one is missing. Keep them in `.env`
(gitignored; copy `.env.example`), which the build command passes to Docker with
`--env-file`. Format: `NAME=value`, one per line, no quotes.

| Variable    | Value                                                        |
|-------------|--------------------------------------------------------------|
| `WIFI_SSID` | Wi-Fi network name                                           |
| `WIFI_PASS` | Wi-Fi password                                               |
| `HA_TOKEN`  | HA > your profile > Security > Long-lived access tokens      |

They end up in the generated `build/esp-idf/main/secrets_env.h` (in the build directory,
never in the source tree). Anyone with the firmware binary can read them.

## Build and flash

```bash
docker run --rm -v "$PWD":/project -v ha-screen-build:/build -w /project \
  --env-file .env \
  espressif/idf:v5.3.2 sh -c 'idf.py -B /build build && cp /build/lvgl_example.bin out/'
esptool --chip esp32c6 -p /dev/cu.usbmodem1101 -b 460800 write-flash 0x10000 out/lvgl_example.bin
```
