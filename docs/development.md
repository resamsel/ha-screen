# Development notes

Things that aren't obvious from the code.

## Host tests

The parser (`main/layout.c`), the calendar event parser (`main/cal_parse.c`), name shortening
(`main/names.c`) and the time-zone table (`main/tz.c`) build and run on the host without the board:

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.2 sh test/run.sh
```

Add a case to `test/layout_test.c` or `test/names_test.c` for every parser or naming fix.

## Flashing without a global esptool

The firmware only needs `esptool` on the host. Either `brew install esptool`, or use a
throwaway virtualenv:

```bash
python3 -m venv /tmp/esp && /tmp/esp/bin/pip install esptool
/tmp/esp/bin/esptool.py --chip esp32c6 -p $(ls /dev/cu.usbmodem* | head -1) -b 460800 write_flash 0x10000 out/lvgl_example.bin
```

esptool 4.x spells it `write_flash`, 5.x `write-flash`.

## Generated assets

- `tools/make_bg.py` downscales the dark/light background images to the screen size.
- `tools/make_fonts.py` generates `main/fonts/*.c` from LVGL's bundled Montserrat
  (needs Pillow; run from the repo root after a build has fetched `managed_components/`).
  - `lv_font_latin_12/14/16/22`: LVGL's built-in fonts are ASCII only, so umlauts, ß, µ, °
    in HA names and units would not show. These add Latin-1 and fall back to the built-in
    font for the `LV_SYMBOL_*` icons. Line height/baseline copy the built-in fonts, so
    layouts don't shift.
  - `lv_font_clock_24…72`: digits, space and `:` only, for the clock card. The clock picks
    40/56/72 px for small/medium/large and steps down proportionally when the block is too
    small, so small stays smaller than medium.
  - `npm lv_font_conv` is not used on purpose; the Pillow script needs no extra download.

## Layout cache in NVS

The parsed `ha_layout_t` is stored as one blob in NVS namespace `layout`, under a versioned
key (currently `v6` in `main/layout_store.c`). **Bump the key whenever `ha_layout_t` or
`layout_item_t` changes**, and add the old key to the erase list: an old blob with a
different size is otherwise read as garbage, and stale blobs fill the NVS partition so the
save fails.

`layout_sanitize` must not change fields that go into the hash, or the cached layout never
matches the fetched one and the UI rebuilds every minute.

## LVGL pitfalls

- `LV_LABEL_LONG_DOT` only shortens with "…" when the label has a fixed height of one
  line; with auto height it wraps into the value below.
- Charts: when `point_cnt >= width`, LVGL 8 switches to "crowded mode", which draws without
  antialiasing and sends no draw-part events (so no fill under the line). Charts use
  width/2 points and resample the history to that.
- The fill under the line is drawn in `LV_EVENT_DRAW_PART_BEGIN` with `lv_draw_polygon`.

## UI code (`main/main.c`)

- Blocks are addressed with `GI(pg, i)`; `graph_block()` gives their geometry (stacked in
  portrait, side by side in landscape).
- Helpers must not change the shared `s_gi` index; index directly. A helper that did put
  labels into the wrong block.
- Rebuilds keep the current page when it still exists.

## Testing dashboard changes

To try a card type on the real dashboard (`esp32-c6-dev/main`), back up the view's config
first, edit, check the serial log (115200 baud) and the screen, then restore and compare
the restored config with the backup. The board picks up changes within 60 s, or at once
with the refresh button.
