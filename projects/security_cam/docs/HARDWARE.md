# Security Camera — Hardware

## Board: ESP32-S3-EYE

| Feature | Detail |
|---------|--------|
| SoC | ESP32-S3, Xtensa LX7 dual-core 240 MHz |
| Camera | OV2640, DVP interface, hardware JPEG encoder |
| PSRAM | 8 MB OPI PSRAM (frame buffer storage) |
| Flash | 8 MB |
| SD card | Micro SD via SDMMC (1-bit SPI mode) |
| WiFi | Built-in 802.11b/g/n, direct `esp_wifi` |
| Display | 240×240 ST7789V SPI LCD |
| Buttons | 4-button ADC resistor ladder on GPIO1 (ADC1_CH0) |
| USB | Built-in USB Serial/JTAG (no external chip) |
| IDF target | `esp32s3` |

## Connection

**Port: `/dev/ttyACM0`** — the S3-EYE uses the ESP32-S3's built-in USB Serial/JTAG
peripheral. This enumerates as `/dev/ttyACM0` on Linux, not `/dev/ttyUSB0`.

`/dev/ttyUSB0` is the port used by boards with an external USB-UART chip (e.g. CH340
on the ESP32-S3-DevKitC-1). Do not confuse the two.

If the port is busy: `kill $(fuser /dev/ttyACM0)` to release a stale monitor.

## GPIO Pinout

### LCD (ST7789V SPI)

| Signal | GPIO |
|--------|------|
| SCK | 21 |
| MOSI | 47 |
| CS | 44 |
| DC | 43 |
| Backlight | 48 (active-low — see OV2640 quirks) |
| RST | 3 |

### Button ADC

| Button | ADC voltage range |
|--------|-------------------|
| UP | < 600 mV |
| DOWN | 600 – 1400 mV |
| PLAY | 1400 – 2400 mV |
| MENU | 2400 – 3100 mV |
| None | > 3100 mV |

All four buttons share GPIO1 (ADC1_CH0) via a resistor ladder.

## OV2640 Quirks

### Mode switching requires full deinit + reinit (both directions)

Using the sensor API (`set_pixformat()`, `set_framesize()`) to switch between
`GRAYSCALE` and `JPEG` mode fails:

- **GRAY → JPEG:** The sensor register changes, but the ESP32-S3 DMA pipeline stays
  in grayscale byte-capture mode. Every captured "JPEG" frame is 76800 bytes of raw
  grayscale data with no `FF D8` SOI header.

- **JPEG → GRAY:** Sensor API leaves the OV2640 PLL in a broken state (`clk_2x=0,
  clk_div=0`). VSYNC stops. `esp_camera_fb_get()` hangs indefinitely.

**Fix:** Both transitions call `esp_camera_deinit()` + `esp_camera_init()`. Takes
~250 ms per direction but is the only reliable method. This is implemented in
`camera_hal_set_mode()`.

### Native frame rate exceeds declared AVI fps

OV2640 at VGA JPEG outputs ~25 fps natively. The AVI file is declared at 10 fps
(`CONFIG_RECORD_FPS=10`). Without a gate, the idx1 index buffer (sized for
60 s × 10 fps = 600 frames) overflows after 24 seconds (600 / 25 fps), and all
subsequent frames are silently dropped for the rest of the clip.

**Fix:** Timestamp-based FPS gate in the recording loop — only write a frame if
at least `1 000 000 / fps` microseconds have elapsed since the last written frame.

### Autoexposure settling after reinit causes flicker frames

After every `deinit + init`, the OV2640 resets its AE state. The first JPEG frame
has wrong exposure (~10 KB vs normal ~14 KB), visible as a brightness flash in playback.
This occurs at recording start and at every periodic motion check (every 50 frames, ~5 s).

**Fix:** Discard 3 frames after every MOTION→RECORD reinit. 1 frame is not enough.
2 frames reduces the artefact but does not eliminate it. 3 frames is clean.
Cost: ~120 ms per motion check cycle.

### GPIO48 backlight is active-low

On the ESP32-S3-EYE, GPIO48 (LCD backlight control) is **active-low**: driving it
`HIGH` switches the backlight **off**. The correct init is:

```c
gpio_set_level(LCD_BL, 0);          // boot with backlight ON
gpio_set_level(LCD_BL, on ? 0 : 1); // active-low polarity
```

## Memory Notes

### PSRAM upload buffer

A 32 KB PSRAM-allocated read buffer + `buffer_size_tx=32768` in `esp_http_client_config_t`
is required for acceptable upload throughput. A 4 KB buffer produces 2200+ `fread` /
`esp_http_client_write` iterations for a 9 MB clip. 32 KB reduces this to ~280 iterations
(8× fewer SD reads and TCP segments). Typical throughput: 300–600 KB/s on home WiFi.

### RESP_BUF_LEN

The presign Lambda response contains two STS-signed presigned URLs. Each URL embeds a
large `X-Amz-Security-Token`. The total JSON response can reach ~1900 bytes.
A 2048-byte response buffer is silently truncated, causing `cJSON_Parse()` to return NULL.
Minimum safe size: **4096 bytes**.
