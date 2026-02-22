# Security Camera — Project Journal

This document records the full journey of the security camera project.
Use it to resume after any break and to understand every decision made.

---

## 1. What We're Building and Why

**Goal:** A motion-triggered security camera based on ESP32 hardware that:
- Detects motion via frame differencing on a downscaled grayscale feed
- Records MJPEG-in-AVI clips (max 60 seconds) to SD card
- Saves the first frame of each clip as a JPEG thumbnail
- Uploads clips + thumbnails to AWS S3 via presigned PUT URL
- Sends SES email alerts on motion detection
- Provides a web gallery UI: thumbnail grid → click → stream/download full clip

**Why this project:** Practical application of the AWS patterns learned in the telemetry "Hello World" project. Real hardware, real cloud integration, real video pipeline.

---

## 2. System Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│ ESP32-S3-EYE                                                      │
│                                                                   │
│  OV2640 ──► camera_hal ──► motion_detect (QVGA grayscale)        │
│                       └──► clip_writer (MJPEG/AVI + thumbnail)   │
│                              └──► sdcard (SDMMC)                  │
│                                                                   │
│  cloud_client (background FreeRTOS task)                          │
│    ├── GET API Gateway /  → presigned PUT URLs                    │
│    └── PUT clip + thumbnail to S3                                 │
└────────────────────────────────────────────┬─────────────────────┘
                                             │ HTTPS
                                    API Gateway HTTP API v2
                                    (Lambda Function URLs 403 on this account)
                                    GET /  → presign.py (no auth)
                                    GET /list → list.py  (JWT auth)
                                             │
                                    S3 bucket: security-cam-clips-26b14cf0
                                    ├── clips/{DEVICE_ID}_YYYYMMDD_HHMMSS.avi
                                    ├── thumbs/{DEVICE_ID}_YYYYMMDD_HHMMSS_thumb.jpg
                                    └── lifecycle: delete after 30 days
                                             │
                                    S3 event → Lambda: notify.py
                                             │
                                    SES → Gmail alert (Proton Mail silently drops SES)
                                             │
┌──────────────────────────────────────────────────────────────────┐
│ Browser                                                           │
│                                                                   │
│  CloudFront (HTTPS) ──► S3 private bucket (OAC) ──► index.html  │
│                                                                   │
│  Cognito hosted login → JWT                                       │
│  JWT → API Gateway GET /list → list.py                            │
│    Returns: [{clip_key, clip_url, thumb_url, timestamp, size_mb}] │
│    clip_url + thumb_url are 7-day presigned GET URLs              │
│                                                                   │
│  Thumbnail grid: click download → browser GETs clip from S3      │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. Hardware

### Primary: ESP32-S3-EYE (Phase 1 — implement now)

| Feature | Detail |
|---------|--------|
| SoC | ESP32-S3, Xtensa LX7 dual-core 240MHz |
| Camera | OV2640, DVP interface, hardware JPEG encoder |
| Camera driver | esp_camera (wraps DVP + OV2640 init) |
| PSRAM | 8MB OPI PSRAM (frame buffer storage) |
| Flash | 8MB |
| SD card | Micro SD via SPI (SD_MMC in 1-bit SPI mode) |
| WiFi | Built-in, direct esp_wifi |
| USB | Micro-USB for flash/debug |
| Target | esp32s3 |

**Key OV2640 characteristics:**
- Hardware JPEG at any resolution: zero CPU cost
- Grayscale at QVGA: fast readout, ideal for motion detection
- Mode switch (JPEG ↔ grayscale) takes ~300ms for sensor stabilisation
- Maximum JPEG resolution: UXGA (1600×1200)
- Recording resolution used: SVGA (800×600) or VGA (640×480) JPEG
- Motion detection resolution: QVGA (320×240) grayscale

### Future: ESP32-P4-FunctionalEVBoard (Phase 2)

| Feature | Detail |
|---------|--------|
| SoC | ESP32-P4, RISC-V dual-core HP 400MHz + LP core |
| Camera | MIPI-CSI interface, hardware ISP |
| Encoding | Hardware H.264 encoder |
| WiFi | Via companion ESP32-C6 chip over SDIO (esp_hosted) |
| Camera driver | esp_cam_ctlr_csi + esp_driver_isp |
| Target | esp32p4 |

---

## 4. Firmware Architecture

### 4.1 Multi-Target Design Philosophy

Single codebase, compile-time target selection:
```bash
idf.py set-target esp32s3   # S3-EYE path
idf.py set-target esp32p4   # P4 path (Phase 2)
```

**Rule: `main.c` contains ZERO `#ifdef IDF_TARGET` blocks.**

All hardware differences are isolated in component `esp32s3/` or `esp32p4/` subdirectories.
The `camera_hal.h` API is the hardware contract — same calls, different implementations.

### 4.2 Directory Structure

```
projects/security_cam/firmware/
├── CMakeLists.txt                       # Explicit SDKCONFIG_DEFAULTS list
├── sdkconfig.defaults                   # Common: TLS, PSRAM, FATFS, stack sizes
├── sdkconfig.defaults.esp32s3           # S3: clock, flash size, PSRAM mode, camera pins
├── sdkconfig.defaults.esp32p4           # P4: 400MHz, SDIO for esp_hosted
├── sdkconfig.defaults.local             # Gitignored: WiFi creds, Lambda URL, device ID
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild                # App-level config symbols
│   └── main.c                          # State machine — ZERO target ifdefs
└── components/
    ├── camera_hal/                      # Hardware boundary
    │   ├── CMakeLists.txt              # Conditionally compiles s3 or p4 impl
    │   ├── idf_component.yml           # esp_camera dep with target constraint
    │   ├── include/camera_hal.h        # THE hardware contract
    │   ├── esp32s3/camera_hal_s3.c     # esp_camera-based (DVP + OV2640)
    │   └── esp32p4/camera_hal_p4.c     # esp_cam_sensor + esp_video + ISP (Phase 2)
    ├── wifi_manager/
    │   ├── CMakeLists.txt
    │   ├── include/wifi_manager.h
    │   ├── esp32s3/wifi_manager_s3.c   # Direct esp_wifi (same as telemetry wifi.c)
    │   └── esp32p4/wifi_manager_p4.c   # esp_hosted SDIO init then esp_wifi (Phase 2)
    ├── clip_writer/
    │   ├── CMakeLists.txt
    │   ├── include/clip_writer.h
    │   ├── avi_writer.c / .h           # MJPEG-in-AVI (S3-EYE path)
    │   └── h264_writer.c / .h          # H.264 stream (P4 path, stub initially)
    ├── motion_detect/
    │   ├── CMakeLists.txt
    │   ├── include/motion_detect.h
    │   └── motion_detect.c             # Pure algorithm — no target knowledge
    ├── cloud_client/
    │   ├── CMakeLists.txt
    │   ├── include/cloud_client.h
    │   └── cloud_client.c              # Presigned URL GET + S3 PUT
    ├── sdcard/
    │   ├── CMakeLists.txt
    │   ├── include/sdcard.h
    │   └── sdcard.c                    # SDMMC mount/unmount
    ├── lcd_ui/
    │   ├── CMakeLists.txt
    │   ├── include/lcd_ui.h
    │   ├── lcd_ui.c                    # ST7789V SPI driver + refresh task
    │   └── font8x16.h                  # IBM PC-8 bitmap font (256 chars × 16 bytes)
    └── button_adc/
        ├── CMakeLists.txt
        ├── include/button_adc.h
        └── button_adc.c                # ADC1 CH0 resistor ladder, debounce, short/long press
```

### 4.3 camera_hal Interface (the hardware contract)

```c
typedef enum { CAM_PIXFMT_JPEG, CAM_PIXFMT_GRAY8, CAM_PIXFMT_YUV420, CAM_PIXFMT_H264_NALU } cam_pixfmt_t;
typedef enum { CAM_MODE_MOTION, CAM_MODE_RECORD } cam_mode_t;

typedef struct {
    void *data; size_t len;
    uint32_t width, height;
    cam_pixfmt_t fmt;
    uint64_t timestamp_us;
} cam_frame_t;

typedef struct {
    bool delivers_jpeg;    // S3-EYE: OV2640 hardware JPEG
    bool delivers_h264;    // P4: hardware H.264 encoder
    uint32_t record_width, record_height;
    uint32_t motion_width, motion_height;
} cam_caps_t;

esp_err_t camera_hal_init(cam_mode_t initial_mode);
esp_err_t camera_hal_set_mode(cam_mode_t mode);      // ~300ms stabilisation, blocks
esp_err_t camera_hal_get_frame(cam_frame_t *f, uint32_t timeout_ms);
esp_err_t camera_hal_release_frame(cam_frame_t *f);
esp_err_t camera_hal_deinit(void);
const cam_caps_t *camera_hal_get_caps(void);
```

`clip_writer` uses `cam_caps_t` at runtime to select AVI or H.264 path — no ifdefs in app code.

### 4.4 Application State Machine (main.c)

```
1. sdcard_init()
2. wifi_manager_connect()
3. camera_hal_init(CAM_MODE_MOTION)      ← QVGA grayscale
4. caps = camera_hal_get_caps()
5. clip_writer_configure(caps)           ← selects AVI or H.264 internally
6. Start cloud upload task (FreeRTOS)
7. loop:
   a. camera_hal_get_frame(&frame, 100)
   b. score = motion_detect_score(frame)
   c. camera_hal_release_frame(&frame)
   d. if score > threshold:
        camera_hal_set_mode(CAM_MODE_RECORD)
        discard 1 frame (AE settling)
        clip_writer_begin(filename)
        record until no-motion timeout or max 60s:
          - FPS gate: write frame only if 100ms elapsed since last (enforces 10fps)
          - first valid JPEG frame: save as _thumb.jpg
          - every 50 frames: RECORD→MOTION check→RECORD (motion still active?)
        clip_writer_end()
        if max-duration: start new clip immediately (continue recording)
        else: camera_hal_set_mode(CAM_MODE_MOTION)
        queue_upload(filename)           ← signal upload task, non-blocking
```

**Key timing constants (main.c):**

| Constant | Value | Meaning |
|----------|-------|---------|
| `MOTION_CHECK_FRAMES` | 50 | Motion check every 50 recorded frames (~5s at 10fps) |
| `MOTION_STOP_TIMEOUT_S` | 12 | Stop recording after 12s with no motion detected |
| `CONFIG_MOTION_THRESHOLD` | 2000 | Changed pixels in QVGA frame to trigger (2.6%) |
| `pixel_threshold` | 40 | Per-pixel change required to count as changed (16%) |
| `RECORD_FRAME_INTERVAL_US` | 100000 | 10fps gate (OV2640 native output is ~25fps) |

### 4.5 Background Upload Task

A dedicated FreeRTOS task owns all WiFi/HTTP work. Recording and upload never overlap:
- Main loop signals via a FreeRTOS queue after each clip is written
- Upload task: GET presigned URL from Lambda → PUT clip to S3 → PUT thumbnail to S3
- SD/WiFi DMA contention is eliminated by design

---

## 5. AWS Infrastructure

### 5.1 Resources (Deployed — Phase 1 complete)

| Resource | Name / URL | Purpose |
|----------|-----------|---------|
| S3 clips bucket | `security-cam-clips-*` | Clip + thumbnail storage |
| Lambda presign | `security-cam-presign` | Generates presigned PUT URLs |
| API Gateway | `https://469uck3f30.execute-api.eu-north-1.amazonaws.com` | Presign endpoint (replaces Function URL — see Lessons) |
| Lambda notify | `security-cam-notify` | SES email on S3 clip upload event |
| Alert email | `aare.palm@gmail.com` | Gmail works; Proton Mail silently drops SES |
| Region | `eu-north-1` | Stockholm |
| Terraform state | `projects/security_cam/terraform/` | Local .tfstate |

### 5.2 Presigned URL Flow

```
ESP32 ──GET──► API Gateway → security-cam-presign Lambda
               ?clip=<name>.avi&thumb=<name>_thumb.jpg
                     │
                     └─► Returns JSON:
                         { "clip_url": "https://s3.../...",
                           "thumb_url": "https://s3.../..." }

ESP32 ──PUT──► clip_url   (Content-Type: video/avi,   32KB chunks)
ESP32 ──PUT──► thumb_url  (Content-Type: image/jpeg,  32KB chunks)
```

**Why API Gateway, not Lambda Function URL:** Lambda Function URLs return HTTP 403
on this AWS account despite correct `authorization_type=NONE` + resource policy.
No SCPs exist; CloudWatch shows requests never reach Lambda. API Gateway HTTP API v2
works correctly. (See Lessons Learned.)

### 5.3 S3 Lifecycle

- Default: delete all objects after 24 hours
- Exception: objects tagged `keep=true` are excluded from lifecycle deletion
- Web UI has a "Keep" button per clip that calls the tag Lambda

---

## 6. Recording Behaviour

| Parameter | Value |
|-----------|-------|
| Motion detection resolution | QVGA (320×240) grayscale |
| Recording resolution | SVGA (800×600) JPEG or VGA (640×480) |
| Trigger | Frame-diff score exceeds threshold |
| Max clip length | 60 seconds |
| Motion continues at 60s | Close clip, immediately start new one |
| Thumbnail | First JPEG frame of each clip |
| Clip naming | ISO 8601 timestamp: `2026-02-21T12-00-00_clip.avi` |
| Thumb naming | Same base: `2026-02-21T12-00-00_thumb.jpg` |
| Pre-roll | None (clip starts on trigger) |

---

## 7. Multi-Target Strategy

### Why two targets

- **ESP32-S3-EYE**: Available now. DVP + OV2640. Hardware JPEG. Standard WiFi.
- **ESP32-P4**: More powerful. MIPI-CSI + ISP + hardware H.264. WiFi via C6 SDIO.

### HAL layer boundaries

| Aspect | S3-EYE (Phase 1) | P4 (Phase 2) |
|--------|-----------------|--------------|
| Camera driver | esp_camera (DVP + OV2640) | esp_cam_ctlr_csi + ISP |
| JPEG | OV2640 hardware | ISP output or H.264 |
| WiFi | Direct esp_wifi | esp_hosted SDIO → esp_wifi |
| Clip format | MJPEG/AVI | H.264 elementary stream |
| HAL files | camera_hal_s3.c, wifi_manager_s3.c | camera_hal_p4.c, wifi_manager_p4.c |

### CMake per-component selection pattern

```cmake
if(IDF_TARGET STREQUAL "esp32s3")
    set(SRCS "esp32s3/camera_hal_s3.c")
    set(REQS esp_camera)
elseif(IDF_TARGET STREQUAL "esp32p4")
    set(SRCS "esp32p4/camera_hal_p4.c")
    set(REQS esp_driver_cam esp_driver_isp esp_driver_i2c)
else()
    message(FATAL_ERROR "Unsupported target: ${IDF_TARGET}")
endif()
```

---

## 8. Key Risks and Resolutions

| Risk | Severity | Resolution |
|------|----------|------------|
| `esp_camera` compat with IDF v5.4 | High | HAL isolates it: if `esp_camera` breaks, rewrite `camera_hal_s3.c` only to use native `esp_cam_ctlr_dvp`. Zero app impact. |
| SD/WiFi DMA contention | High | Background upload task — upload never runs during recording. PSRAM frame ring buffer (~4–6 frames) absorbs SD write stalls. |
| OV2640 mode switch latency ~300ms | Medium | `camera_hal_set_mode()` discards frames until valid, then returns. App blocks briefly — imperceptible. |
| P4 `esp_hosted` boot complexity | High | Deferred by design — isolated in `wifi_manager_p4.c`, zero S3-EYE impact. |
| P4 H.264 container format | High | Start with raw `.h264` elementary stream. MP4 muxing added later if needed. |
| AVI idx1 buffer size | Low | Pre-allocate at `clip_writer_begin()`: 60s × 10fps × 8 bytes = 4.8KB in PSRAM. Free at close. |
| Power loss mid-clip | Medium | AVI header placeholder patched at close only. File unplayable if power cut. Future: OpenDML index or repair tool. |

---

## 9. Implementation Sequence

### Phase 1 (ESP32-S3-EYE — COMPLETE)

| Step | Component | Status |
|------|-----------|--------|
| 1 | Scaffold directory structure + stub files for both targets | ✅ Done |
| 2 | `sdcard` — SDMMC mount, write test file | ✅ Done |
| 3 | `wifi_manager` S3 backend — copy/adapt from telemetry wifi.c | ✅ Done |
| 4 | `camera_hal` S3 backend — esp_camera init, QVGA + JPEG modes | ✅ Done |
| 5 | `motion_detect` — frame diff, tunable threshold | ✅ Done |
| 6 | `clip_writer` AVI path — RIFF header, movi chunks, idx1 patch | ✅ Done |
| 7 | `cloud_client` — presigned URL GET, S3 PUT | ✅ Done |
| 8 | Terraform — S3 bucket, Lambda, IAM, API Gateway | ✅ Done |
| 9 | Integration test: motion → clip → S3 → email | ✅ Done |
| 9a | Post-integration tuning (see Section 13) | ✅ Done |
| 9b | LCD UI + button control (see Section 14) | ✅ Done |

### Phase 3 (Web Gallery — COMPLETE)

| Step | Component | Status |
|------|-----------|--------|
| 9c | `list.py` Lambda — list clips, presigned GET URLs, keep status | ✅ Done |
| 9d | `index.html.tpl` — gallery UI, Cognito auth, keep + delete buttons | ✅ Done |
| 9e | Terraform — Cognito, CloudFront OAC, list + manage routes, CORS | ✅ Done |
| 9f | `manage.py` Lambda — keep/unkeep/delete; tag-based lifecycle; migration | ✅ Done |

### Phase 4 (ESP32-P4 — later)

| Step | Component | Status |
|------|-----------|--------|
| 10 | `camera_hal` P4 backend | Pending |
| 11 | `wifi_manager` P4 backend — esp_hosted | Pending |
| 12 | `clip_writer` H.264 path | Pending |
| 13 | Integration test on P4 hardware | Pending |

---

## 10. Lessons Learned

### esp_camera and IDF v5.x

The esp_camera component (Espressif's legacy camera driver) has had compatibility issues
with newer IDF versions. If `camera_hal_s3.c` build fails, the HAL design means we can
swap the implementation to use the native IDF v5 `esp_cam_ctlr_dvp` API without touching
any application code.

### sdkconfig.defaults multi-file pattern

For multi-target builds the top-level CMakeLists.txt must construct the SDKCONFIG_DEFAULTS
list explicitly:
```cmake
set(SDKCONFIG_DEFAULTS
    "sdkconfig.defaults"
    "sdkconfig.defaults.${IDF_TARGET}"
)
```
Both `sdkconfig.defaults.esp32s3` and `sdkconfig.defaults.esp32p4` must exist in the repo
or the build fails for any target (CMake errors on missing files).

### sdkconfig vs sdkconfig.defaults

`sdkconfig.defaults` is only applied when the `sdkconfig` file does not yet exist.
If you change a value in `sdkconfig.defaults` after first build, the compiled `sdkconfig`
keeps the old value. To apply: either edit `sdkconfig` directly, or `idf.py fullclean`
(destroys the build, forces sdkconfig regeneration from defaults).

### Lambda Function URLs — account-level 403 (this account)

Lambda Function URLs with `authorization_type=NONE` return HTTP 403 on this AWS account,
even with correct resource policy and no SCPs. CloudWatch shows the requests never reach
Lambda. Workaround: use API Gateway HTTP API v2 (`aws_apigatewayv2_api` + integration +
route + stage + `aws_lambda_permission` with `lambda:InvokeFunction` from
`apigateway.amazonaws.com`). API Gateway works correctly.

### SES email delivery — Proton Mail silently drops

SES verified, Lambda fires, CloudWatch logs "Alert sent", SES stats show 0 bounces —
but Proton Mail silently drops all SES emails. They never arrive in any folder.
Gmail works correctly (emails land in Promotions tab). Use Gmail for the alert address.

### OV2640 mode switch — must use full deinit+reinit both directions

Using the sensor API (`set_pixformat()`, `set_framesize()`) to switch between GRAYSCALE
and JPEG fails in both directions:

- **GRAY→JPEG**: sensor API only changes the OV2640 register. The ESP32-S3 DMA pipeline
  stays in grayscale byte-capture configuration. Every "JPEG" frame is raw grayscale bytes
  (76800 bytes for QVGA, no `FF D8` header). Confirmed by hex inspection.
- **JPEG→GRAY**: sensor API leaves the OV2640 PLL broken (`clk_2x=0, clk_div=0`),
  VSYNC stops, `fb_get()` hangs indefinitely.

**Fix:** Both transitions must use `esp_camera_deinit()` + `esp_camera_init()`.
This takes ~250ms per direction but is the only reliable approach.

### OV2640 native frame rate vs declared AVI fps

The OV2640 at VGA JPEG outputs ~25 fps natively. The AVI file declares 10fps
(`CONFIG_RECORD_FPS=10`). Without a FPS gate, the idx1 index buffer (sized for
60s × 10fps = 600 frames) overflows after just 24 seconds (600 / 25fps), and all
subsequent frames are silently dropped for the remaining 36 seconds of the clip.

**Fix:** Timestamp-based FPS gate in the recording loop — only write a frame if
`now >= next_frame_time`. `next_frame_time` advances by `1,000,000 / fps` microseconds.
If we fall behind (e.g. after a mode-switch), reset `next_frame_time` rather than burst.

### Motion detection threshold tuning for QVGA

The Kconfig default (`default 50`, help text said "downscaled 80×60") was wrong.
The actual frame is QVGA (320×240 = 76,800 pixels). `MOTION_THRESHOLD=500` = 0.65% of
frame — triggers on any lighting change. Correct starting value: 2000 (2.6%).
Per-pixel threshold also matters: `pixel_threshold=30` (12% per pixel) catches global
brightness shifts. Raise to 40 (16%) to filter lamp flicker and cloud shadows.

### Motion-stop timeout must exceed check interval

`MOTION_CHECK_FRAMES=50` at 10fps = ~5s check interval.
If `MOTION_STOP_TIMEOUT_S ≤ 5`, a single failed motion check immediately stops recording:
by the time we return from the ~340ms mode-switch round trip, 5+ seconds have elapsed
since the last confirmed motion, exceeding the timeout. Recording stops after one bad
check even during active movement.

**Rule:** `MOTION_STOP_TIMEOUT_S` must be > `MOTION_CHECK_FRAMES / fps` (the check
interval). Set to 12s so two consecutive failed checks are required before stopping.

### OV2640 AE settling after reinit causes flicker frames

After every `esp_camera_deinit()` + `esp_camera_init()`, the OV2640 resets its
autoexposure. The first JPEG frame captured after reinit has the wrong exposure compared
to steady-state (~10 KB vs normal ~14 KB). This shows as a visible brightness flicker
in the recorded video, occurring at every periodic motion check (every 50 frames).

**Fix:** Discard **3 frames** after every MOTION→RECORD reinit. 1 frame was not enough
(still visible). 2 frames were marginally better but flicker remained. 3 frames eliminates
it. Each discard costs ~40ms (one frame at 25fps native rate = ~120ms total overhead).
Apply both at start-of-recording and after each periodic motion check:
```c
{ cam_frame_t f0; if (camera_hal_get_frame(&f0, 200) == ESP_OK) camera_hal_release_frame(&f0); }
{ cam_frame_t f0; if (camera_hal_get_frame(&f0, 200) == ESP_OK) camera_hal_release_frame(&f0); }
{ cam_frame_t f0; if (camera_hal_get_frame(&f0, 200) == ESP_OK) camera_hal_release_frame(&f0); }
```

### Upload performance — chunk size matters

Default 4KB read buffer + 4KB TCP send buffer (`buffer_size_tx`) produced 2200+
`fread`/`esp_http_client_write` iterations for a 9MB clip. FAT filesystem and TCP
overhead on each small call is significant.

**Fix:** 32KB PSRAM-allocated read buffer + `buffer_size_tx=32768`. Reduces iterations
to ~280 (8× fewer SD reads and TCP segments). The KB/s throughput log shows actual speed.
Typical results: 300–600 KB/s on home WiFi, limited by WiFi upload bandwidth.

### RESP_BUF_LEN for presigned URL responses

Two AWS STS-session presigned URLs in a JSON response can reach ~1900 bytes
(large `X-Amz-Security-Token` embedded in each URL). The initial buffer of 2048 bytes
was silently truncated, causing `cJSON_Parse()` to return NULL. Minimum safe size: 4096.

### Python env mismatch on flash

The `.bashrc` activates the `esp-idf-tools` Python env, but builds use the
`.espressif` env. This causes `idf.py` to not be found after sourcing. Always source
IDF explicitly: `source ~/esp32_cloud/esp-idf/export.sh`.

---

## 11. How to Resume After a Break

```bash
# 1. Read this journal (especially Sections 10, 13, 14, 15)

# 2. Source IDF — always do this explicitly, .bashrc activates wrong env
source /home/aare/esp32_cloud/esp-idf/export.sh

# 3. Build
cd /home/aare/esp32_cloud/projects/security_cam/firmware
idf.py build

# 4. Flash + monitor (device port is /dev/ttyACM0 on S3-EYE — built-in USB JTAG)
#    Kill any stale monitor first: kill $(fuser /dev/ttyACM0)
idf.py -p /dev/ttyACM0 flash monitor

# 5. Key log markers to watch:
#    W main: >>> RECORD START  — motion triggered, recording started
#    W main: >>> RECORD STOP   — recording ended (reason + frame count)
#    W main: >>> UPLOAD START  — upload task starting
#    W main: >>> UPLOAD OK/FAIL — upload result
#    I main: Motion check: score=NNN  — periodic check (every ~5s during recording)
#    I cloud_client: PUT stream: NNN bytes in NNN ms → NNN KB/s — upload throughput

# 6. Terraform (if AWS changes needed)
cd /home/aare/esp32_cloud/projects/security_cam/terraform
terraform plan
terraform apply

# 7. Key terraform outputs
terraform output presign_url   # paste into sdkconfig.defaults.local
terraform output gallery_url   # open in browser: https://dj2mjryixkzca.cloudfront.net

# 8. Credentials in sdkconfig.defaults.local (gitignored):
#    CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD, CONFIG_LAMBDA_PRESIGN_URL, CONFIG_DEVICE_ID

# 9. Gallery login
#    URL:      https://dj2mjryixkzca.cloudfront.net
#    Username: aarepalm
#    Password: same as telemetry dashboard
```

**Important:** If you change `sdkconfig.defaults`, it does NOT update a pre-existing
`sdkconfig`. Edit `sdkconfig` directly or run `idf.py fullclean` first.

---

## 12. Deployed Resources

| Resource | Name / URL |
|----------|-----------|
| S3 clips bucket | `security-cam-clips-26b14cf0` |
| Presign API URL | `https://469uck3f30.execute-api.eu-north-1.amazonaws.com` |
| Alert email | `aare.palm@gmail.com` |
| **Gallery URL** | **`https://dj2mjryixkzca.cloudfront.net`** |
| **Cognito login** | **`https://security-cam-3bda06ce.auth.eu-north-1.amazoncognito.com/login`** |
| S3 webapp bucket | `security-cam-webapp-13647a28` |
| Region | `eu-north-1` (Stockholm) |
| Terraform state | `projects/security_cam/terraform/terraform.tfstate` (local) |
| Firmware dir | `projects/security_cam/firmware/` |
| Device port | `/dev/ttyACM0` (ESP32-S3-EYE built-in USB Serial/JTAG) |

---

## 13. Phase 1 Tuning Log

All bugs found and fixed during integration testing after initial end-to-end worked.

### Bug 1: All AVI frames were raw grayscale (not JPEG)

**Symptom:** ffplay could not open clips. `ffprobe` showed correct structure but
"No JPEG data found". Python hex inspection: all frames were 76800-byte raw grayscale
blobs (`5d 5e 5e...`), no `FF D8` JPEG header.

**Root cause:** `camera_hal_set_mode()` used sensor API `set_pixformat(JPEG)` which only
changed the OV2640 register. The ESP32-S3 DMA pipeline stayed in grayscale byte-capture
mode. Full reinit required (see Lessons Learned).

**Fix:** Rewrote `camera_hal_set_mode()` to call `esp_camera_deinit()` +
`esp_camera_init()` for both directions.

### Bug 2: idx1 overflow after 24 seconds

**Symptom:** `W avi_writer: Frame count exceeds pre-allocated idx1 capacity (600)`
repeating every 40ms. Clips recorded for 60s wall-clock but contained only the first
24s of content; frames 601+ silently dropped.

**Root cause:** OV2640 outputs ~25fps natively. idx1 sized for 60s × 10fps = 600 frames.
600 frames / 25fps = 24s until overflow. `stop_max` fires on wall-clock (60s), not frame
count, so recording continued with all frames dropped.

**Fix:** Timestamp-based FPS gate in recording loop. One frame written per 100ms
(1,000,000 / CONFIG_RECORD_FPS µs).

### Bug 3: Recording stopped immediately after first motion check

**Symptom:** `RECORD STOP motion gone (5s idle)` after only ~10s of recording, even
with active movement.

**Root cause:** `MOTION_STOP_TIMEOUT_S=5` equalled the check interval (~5s). After the
first motion check returned "quiet" (due to AE settling after reinit), `no_motion_us`
was already ≥ 5s. Recording stopped on the very next frame after returning from the check.

**Fix:** Raised `MOTION_STOP_TIMEOUT_S` to 12s (> 2× check interval). Now requires two
consecutive failed checks before stopping.

### Bug 4: Periodic flicker in AVI playback

**Symptom:** Brief brightness flash every ~5 seconds in recorded video. Python frame-size
analysis confirmed one ~10KB outlier frame exactly every 50 frames (matching
`MOTION_CHECK_FRAMES`).

**Root cause:** First JPEG frame after each RECORD mode reinit captured the OV2640
during AE settling. That frame has wrong exposure (10KB vs normal 14KB).

**Fix:** Discard 3 frames after every MOTION→RECORD reinit (both at recording start
and after each periodic check). 1 frame was not enough (still visible). 2 frames reduced
it but flicker remained. 3 frames eliminates it. Cost: ~120ms per motion check cycle.

### Bug 5: Motion detection too sensitive

**Symptom:** Camera triggering on slight lighting changes (clouds, lamp flicker).

**Root cause:** `MOTION_THRESHOLD=500` on a 76,800-pixel QVGA frame = 0.65% of pixels.
`pixel_threshold=30` (12% per pixel) counted global brightness shifts.

**Fix:** `MOTION_THRESHOLD` → 2000 (2.6%). `pixel_threshold` → 40 (16%).

### Bug 6: "RECORD START" missing from log for continued clips

**Symptom:** Log showed `RECORD STOP (max duration)` followed immediately by another
`RECORD STOP` with no matching `RECORD START`.

**Root cause:** When `stop_max` fires, the code starts a new clip immediately but only
logged `ESP_LOGI "Started new clip"` (INFO level, no `>>>` marker).

**Fix:** Changed to `ESP_LOGW ">>> RECORD START (continued after max duration)"`.

### Bug 7: Upload slow — 4KB chunk size

**Symptom:** Large clips (9MB) uploading slowly. 4KB read/write loop = 2230+ iterations
of `fread` + `esp_http_client_write`.

**Fix:** 32KB PSRAM buffer + `buffer_size_tx=32768`. ~8× fewer iterations. Throughput
now logged: `PUT stream: N bytes in N ms → N KB/s`.

---

## 14. Phase 2 — LCD UI + Button Control

### Overview

Added a live status screen and button controls to the running camera.

**New components:**
- `components/lcd_ui/` — ST7789V 240×240 SPI LCD driver + FreeRTOS refresh task
- `components/button_adc/` — resistor-ladder button driver on ADC1 CH0

**Changes to main.c:**
- Delete-after-upload: `.avi` and `_thumb.jpg` removed from SD after successful upload
- `UPLOAD_QUEUE_DEPTH` raised from 4 → 20 (for bulk upload of all pending clips)
- `upload_all_pending()` helper: scans `/sdcard/` for `*.avi`, posts each to upload queue
- LCD + button init after SD/WiFi/camera init
- Button event drain in main loop (non-blocking `xQueueReceive`)
- MENU short press → screen on/off toggle
- PLAY long press → queue all pending `.avi` clips for upload

### Hardware

| Signal | GPIO |
|--------|------|
| LCD SCK | 21 |
| LCD MOSI | 47 |
| LCD CS | 44 |
| LCD DC | 43 |
| LCD BL | 48 |
| LCD RST | 3 |
| Buttons ADC | GPIO1 (ADC1_CH0) |

Buttons are a resistor ladder on a single ADC pin. Thresholds: UP <600mV, DOWN 600–1400mV, PLAY 1400–2400mV, MENU 2400–3100mV, None >3100mV.

### Screen Layout

```
y= 20  STATE LINE  2× font — "WATCHING" (yellow) / "REC 0:23" (red)
y= 80  UPLOAD LINE 1× font — "Uploading..." (cyan, only when active)
y=120  "Free:    4.2 GB"   (white, updated every 5s)
y=145  "Pending: 3"        (white, updated every 5s)
y=170  "Done:    7"        (white, session counter)
```

### Bugs Found and Fixed During Bring-up

#### Bug 1: `esp_log` in component REQUIRES

`esp_log` is a built-in IDF component — it cannot appear in a component's REQUIRES
list. CMake error: `Failed to resolve component 'esp_log'`. Removed from both
`lcd_ui/CMakeLists.txt` and `button_adc/CMakeLists.txt`.

#### Bug 2: `sys/statvfs.h` not available in ESP-IDF newlib

`statvfs()` / `<sys/statvfs.h>` is not provided by the IDF toolchain's newlib.
Replaced with `esp_vfs_fat_info("/sdcard", &total, &free)` from `esp_vfs_fat.h`
(part of the `fatfs` component, which was already a REQUIRES dependency).
`esp_vfs_fat` is also not a standalone component name — it lives inside `fatfs`.

#### Bug 3: `fill_rect` DMA overrun

`fill_rect(x, y, w, h)` allocated one row on the stack (`uint16_t row[240]` = 480 bytes)
but called `esp_lcd_panel_draw_bitmap(panel, x, y, x+w, y+h, row)` with the full
W×H rectangle. The DMA transfer length is computed internally from the rectangle
dimensions — it would read `240×240×2 = 115,200 bytes` starting from a 480-byte stack
array, overrunning into adjacent stack frames and writing garbage to the display.

**Fix:** Loop `draw_bitmap` one row at a time, identical to how `draw_char` already
works correctly.

#### Bug 4: SPI bus race between main loop and refresh task

`lcd_ui_set_screen_on(false)` called `fill_rect()` directly from the main-loop task
to clear the screen. Concurrently, `refresh_task` was issuing its own `draw_bitmap`
SPI transactions. Both tasks queuing SPI transactions simultaneously caused them to
interleave, producing garbage on screen when toggling.

**Fix:** `lcd_ui_set_screen_on()` now only toggles the backlight GPIO and sets a
`g_needs_clear` flag. The refresh task checks the flag on its next tick, performs
the full clear, then redraws. Only `refresh_task` ever touches the SPI bus.

#### Bug 5: `esp_vfs_fat_info()` blocking the refresh loop

`esp_vfs_fat_info()` calls FatFS `f_getfree()`, which scans the entire File Allocation
Table to count free clusters. On a 60 GB SD card this blocks for **several seconds**.
Calling it every 250ms stalled the refresh loop, making state/upload line updates appear
frozen (effectively 0.2 Hz instead of 4 Hz). Uploading text appeared and disappeared
unpredictably because the task was asleep inside FAT scanning when state changed.

**Fix (first attempt):** Cache SD stats (free space + pending count) and refresh them
only every `SD_REFRESH_TICKS × 250ms = 5s`. The state line and upload line still
update every 250ms as intended.

**Fix (final):** Moved all SD filesystem access into a dedicated `sd_stats_task`
(priority 2) that runs `esp_vfs_fat_info()` and `sd_pending_count()` independently,
every 5 s, and stores results in `volatile float g_sd_free_gb` / `volatile int
g_sd_pending`. The `refresh_task` (priority 3) reads these cached globals — it never
touches the filesystem at all.

#### Bug 6: GPIO48 backlight active-low polarity inversion (root cause of "frozen display")

**Symptom:** State text (WATCHING / REC / Uploading) appeared to update only after the
user toggled the screen off and back on. The serial log messages `lcd_ui: Screen ON`
and `lcd_ui: Screen OFF` printed with inverted meaning. Users reported that after
boot the display appeared dark, while pressing MENU to "turn off" actually made it
light up.

**Root cause:** GPIO48 (backlight control) on the ESP32-S3-EYE is **active-low** —
driving it HIGH switches the backlight OFF; driving it LOW switches it ON. The original
code set `gpio_set_level(LCD_BL, on ? 1 : 0)`, which had the polarity backwards:

- At boot: `gpio_set_level(LCD_BL, 1)` → backlight OFF. `refresh_task` updated the
  screen correctly every 250ms, but the screen was dark so the user saw nothing.
- MENU "off" pressed: `g_screen_on = false` → GPIO goes low → backlight physically
  ON. Refresh task stopped drawing. User sees the last-drawn frame — it looks "live"
  because it is from 250ms ago, but no further updates arrive.
- MENU "on" pressed: `g_screen_on = true` → GPIO goes high → backlight physically
  OFF again. `g_needs_clear = true` triggers one immediate redraw, which the user
  briefly glimpses before the backlight cuts out.

The display was working perfectly the entire time. Only the backlight was controlled
with inverted polarity.

**Debugging clue:** User observed "if data appears right after off/on, it must be
available non-blocking." This was the key insight: the data WAS always available and
non-blocking — the `refresh_task` was drawing it every 250ms to a dark screen.

**Fix:**
```c
gpio_set_level(LCD_BL, on ? 0 : 1);  /* GPIO48 is active-low */
```
Init line changed to `gpio_set_level(LCD_BL, 0)` so backlight is ON from boot.

---

## 15. Phase 3 — Web Gallery (CloudFront + Cognito)

### What was built

Adds a CloudFront-hosted web gallery showing all recorded clips with
thumbnails, timestamps and download links. S3 clips bucket stays private;
all media is served via 7-day presigned GET URLs.

**Architecture:**
```
Browser → CloudFront → S3 webapp bucket (index.html)
Browser → Cognito hosted login → JWT
Browser + JWT → API Gateway GET /list (JWT Authorizer) → list Lambda → S3
Browser → S3 presigned GET URL → download clip or thumbnail
```

### New AWS resources

| Resource | Name |
|----------|------|
| Cognito User Pool | `security-cam-gallery` |
| Cognito hosted domain | `security-cam-3bda06ce.auth.eu-north-1.amazoncognito.com` |
| CloudFront distribution | `dj2mjryixkzca.cloudfront.net` |
| S3 webapp bucket | `security-cam-webapp-13647a28` (private, OAC) |
| Lambda | `security-cam-list` |
| API Gateway route | `GET /list` on existing `469uck3f30` API (JWT auth) |

### New files

| File | Purpose |
|------|---------|
| `terraform/lambda_src/list.py` | Lists `clips/*.avi`, pairs with `thumbs/*_thumb.jpg`, generates 7-day presigned GETs, sorts newest-first |
| `terraform/webapp_src/index.html.tpl` | Gallery HTML (dark theme, no CDN deps); 4 Terraform template vars injected at apply |
| `terraform/cognito.tf` | Cognito pool + hosted domain + app client + admin user |
| `terraform/gallery.tf` | list Lambda, JWT authorizer, /list route, S3 webapp, OAC, CloudFront, index.html upload |

### Modified files

| File | Change |
|------|--------|
| `terraform/variables.tf` | Added `cognito_username`, `cognito_password` |
| `terraform/terraform.tfvars` | Added cognito credential entries |
| `terraform/iam.tf` | Added `s3:ListBucket` on clips bucket ARN (list Lambda needs it) |
| `terraform/lambda.tf` | Added `cors_configuration` to existing API Gateway (allows CloudFront origin) |
| `terraform/outputs.tf` | Added `gallery_url`, `cognito_login_url` |
| `terraform/main.tf` | Added `null` provider (needed for `null_resource` permanent password) |

### Key design decisions

**OAC instead of public S3 website:**
The telemetry project uses public S3 website hosting + CloudFront custom origin.
The gallery uses the newer OAC (Origin Access Control) pattern: S3 bucket stays
private, CloudFront authenticates via IAM SigV4. Better security, no public bucket.

**Reusing existing API Gateway:**
The gallery's `GET /list` route is added to the existing `presign` API Gateway
(same `469uck3f30` endpoint). Only the new route has JWT auth; `GET /` (device
presign) remains unauthenticated. CORS is added to the API to allow the browser
at the CloudFront origin to call `/list`.

**Template file for index.html:**
The gallery `index.html` is stored as `webapp_src/index.html.tpl` with 4 Terraform
template variables (`${cognito_domain}`, `${client_id}`, `${redirect_uri}`,
`${api_url}`). `templatefile()` renders it at `terraform apply` time and
`aws_s3_object` uploads the rendered result. No manual HTML editing needed.

**No JS template literals in the template file:**
Using JS backtick template literals (`${...}`) would conflict with Terraform's
template syntax. The gallery JS uses plain string concatenation throughout, so
no `$${...}` escaping was needed anywhere.

---

## 16. Phase 3b — Keep and Delete Features

### What was built

Two gallery actions per clip:
- **Keep** — protects a clip from the 30-day auto-delete lifecycle (toggle)
- **Delete** — immediately removes clip + thumbnail from S3

### How Keep works

S3 lifecycle rules can only filter on what IS present, not on what is absent
(there is no "objects without tag X" filter). So the approach is:

1. **Tag every new clip** with `keep=false` (done by `notify.py` after the email)
2. **Lifecycle rule** for `clips/` prefix filters on tag `keep=false` → deletes after 30 days
3. **Keep action** changes the tag to `keep=true` → object is exempt from lifecycle
4. **Unkeep action** changes it back to `keep=false` → lifecycle resumes

Thumbnails are NOT tagged. They use a separate lifecycle rule (`thumbs/` prefix,
no tag filter) that always deletes after 30 days. A kept clip's thumbnail will
therefore disappear after 30 days, showing "no thumbnail" in the gallery — the
clip download link still works.

### Race condition: clip upload vs thumbnail upload

The ESP32 uploads the clip first (large, can take 10–30s), then the thumbnail.
The S3 event fires when the clip upload completes — at that moment the thumbnail
has not been PUT yet. So `notify.py` only tags the clip, not the thumbnail.
The thumbnail is handled by the separate time-based lifecycle.

### Why not tag at PUT time (presigned URL)?

Embedding `Tagging: keep=false` in the presigned PUT URL is possible but requires
the firmware to send the `x-amz-tagging` header, which it currently does not.
That would be a firmware change. Using the notify Lambda avoids touching the firmware.

### Migration for existing clips

Before this change, clips had no `keep` tag. The new tag-filtered lifecycle would
NOT delete them (no tag → filter doesn't match). To restore correct behaviour:
a `null_resource.tag_existing_clips` runs once at `terraform apply` time and
iterates all existing `clips/*.avi` objects, tagging any that lack the `keep` tag
with `keep=false`. Result: 20+ existing clips were tagged correctly.

### New / modified files

| File | Change |
|------|--------|
| `lambda_src/manage.py` | New — handles keep/unkeep/delete via `POST /manage` |
| `lambda_src/notify.py` | Tags new clip with `keep=false` after sending email |
| `lambda_src/list.py` | Reads `keep` tag per clip via `get_object_tagging`; returns `kept` bool |
| `terraform/s3.tf` | Replaced single lifecycle rule with two tag-filtered rules + migration `null_resource` |
| `terraform/gallery.tf` | manage Lambda + CloudWatch log group + API Gateway integration + route + permission |
| `terraform/iam.tf` | Added `s3:DeleteObject`, `s3:GetObjectTagging`, `s3:PutObjectTagging` |
| `terraform/lambda.tf` | CORS: added `POST` method and `content-type` header |
| `webapp_src/index.html.tpl` | Per-card keep toggle + delete button; kept clips get green border + lock icon |

### Gallery UX

- **keep** button: toggles `keep=true/false` tag without page reload. Kept clips get a green border and a 🔒 icon. Button text changes to "unkeep".
- **delete** button: shows `confirm()` dialog, then calls `DELETE` action and removes the card from DOM (no page reload).
- Both buttons dim the card (`opacity: 0.5`, `pointer-events: none`) while the API call is in flight.
