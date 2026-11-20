# ha-screen

Home Assistant display for the Waveshare ESP32-C6-Touch-LCD-1.47.
Settings (HA URL, settings entities, poll interval, graph style) are in `main/config.h`.

## Pages from a dashboard

The main-screen pages come from the HA dashboard view `HA_DASHBOARD`/`HA_VIEW`
(`esp32-c6-dev/main`). The board re-reads it every minute and rebuilds when it changed:

- each section = one page; its heading card = the page title
- `tile` cards = value rows (up to 4), in dashboard order; name = the card's name or the
  entity's friendly name
- a `sensor` card with `graph: line` = a graph page (span = `hours_to_show`, default 24)
- a `picture-entity` card = a picture fitted into its block, name along the bottom
  (`show_name: false` hides it). Source: `camera_image`, else `image` (URL, absolute or relative
  to HA; the HA token is only sent to HA), else the card's own `camera.*`/`image.*` entity.
  Cameras refresh every 60 s while the page is shown (`HA_PICTURE_INTERVAL_MS`), image entities
  when their state changes, URLs on each page visit. JPEG (baseline) and PNG (non-interlaced)
- a `weather-forecast` card = current weather of a `weather.*` entity: icon, condition, name,
  temperature and humidity (the forecast is not shown)
- icons on tiles, sensor graphs and buttons, as in HA: the card's `icon`, else the entity's,
  else HA's icon for its integration, device class or domain (`show_icon: false` hides it).
  All Material Design Icons are built in (`tools/make_icons.py` -> `main/mdi_icons.bin`)
- up to 8 pages; anything else (other cards, missing or non-numeric entities) is skipped

The last good layout is kept in flash, so the board starts with it when HA isn't reachable.

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

## Screenshots

With the board on USB, `python3 tools/screenshot.py [port] [out.png]` saves what is on screen
(needs pyserial and Pillow; the board redraws once and streams it over the cable).

## Build and flash

```bash
docker run --rm -v "$PWD":/project -v ha-screen-build:/build -w /project \
  --env-file .env \
  espressif/idf:v5.3.2 sh -c 'idf.py -B /build build && cp /build/lvgl_example.bin out/'
esptool --chip esp32c6 -p /dev/cu.usbmodem1101 -b 460800 write-flash 0x10000 out/lvgl_example.bin
```
