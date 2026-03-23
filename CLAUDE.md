# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

ESP32-to-AWS learning repo. The primary project is `projects/security_cam/`: a motion-triggered security camera running on ESP32-S3-EYE that records MJPEG/AVI clips to SD, uploads to S3 via presigned URLs, sends Gmail alerts via SES, and serves a Cognito-protected web gallery.

## Environment Setup

ESP-IDF must be activated in every shell session before any `idf.py` commands:

```bash
. ./activate.sh   # from repo root
```

This sources the ESP-IDF v5.4 environment. `esp-idf/` (~1 GB) is installed separately and not committed.

## Firmware Commands

All firmware commands run from `projects/security_cam/firmware/`:

```bash
idf.py set-target esp32s3          # configure target (run once or after clean)
idf.py build                       # compile
idf.py -p <port> flash             # flash (S3-EYE uses USB Serial/JTAG, not CH340)
idf.py -p <port> monitor           # serial monitor
idf.py -p <port> flash monitor     # flash + monitor in one step
idf.py fullclean                   # clean build artifacts
```

**Port:** Linux: `/dev/ttyACM0` · macOS: `/dev/cu.usbmodem<id>` (run `ls /dev/cu.usb*` to find it).

**Credentials:** Copy `sdkconfig.defaults.local.example` to `sdkconfig.defaults.local` and fill in WiFi SSID/password, API key, device ID, and presign URL. This file is gitignored.

## AWS Infrastructure Commands

All Terraform commands run from `projects/security_cam/terraform/`:

```bash
terraform init
terraform plan
terraform apply
terraform output presign_url    # copy into sdkconfig.defaults.local
terraform destroy
```

**Credentials:** Copy `terraform.tfvars.example` to `terraform.tfvars` and fill in `alert_email`, `presign_api_key`, and Cognito credentials. Gitignored.

AWS region defaults to `eu-north-1`.

## Architecture

### Firmware (ESP32-S3)

`main.c` implements a state machine with **zero target ifdefs** — all hardware differences are behind HAL components:

```
INIT → MOTION_WATCH → (score > threshold) → RECORDING → MOTION_WATCH
                                                  ↓
                                          cloud_client upload queue (BG task)
```

**Component structure** — each component has `include/<name>.h` (public API, zero ifdefs) plus target-specific implementations in `esp32s3/` and `esp32p4/` subdirectories. CMake selects the right source file based on `IDF_TARGET`:

| Component | Purpose |
|---|---|
| `camera_hal` | OV2640 (S3 DVP) / MIPI-CSI (P4); two modes: `CAM_MODE_MOTION` (QVGA grayscale) and `CAM_MODE_RECORD` (SVGA JPEG) |
| `motion_detect` | Frame-differencing on grayscale frames; returns pixel-count score |
| `clip_writer` | Writes MJPEG/AVI to SD card; auto-patches RIFF headers and idx1 index on finalize |
| `cloud_client` | GETs presigned PUT URLs from Lambda, uploads clip + thumbnail in 32 KB chunks |
| `wifi_manager` | Connects to WiFi; blocks until connected |
| `sdcard` | SDMMC mount and file I/O |
| `lcd_ui` | ST7789V 240×240 display at 4 Hz |
| `button_adc` | 4-button ADC resistor ladder on GPIO1 |
| `boot_console` | Serial diagnostics at startup |

**sdkconfig layering:**
1. `sdkconfig.defaults` — common defaults (committed)
2. `sdkconfig.defaults.esp32s3` or `.esp32p4` — target-specific (committed)
3. `sdkconfig.defaults.local` — user credentials (gitignored)

### AWS Cloud

- **S3 clips bucket** — `clips/` (AVI, tagged `keep=false`), `thumbs/` (JPEG); lifecycle deletes `keep=false` clips after 30 days
- **4 Lambda functions** (Python, in `terraform/lambda_src/`):
  - `presign` — API key auth in handler; returns presigned PUT URLs for device upload
  - `notify` — triggered by S3 ObjectCreated; tags clip `keep=false`, sends SES email
  - `list` — Cognito JWT auth; returns clip list with presigned GET URLs (7-day)
  - `manage` — Cognito JWT auth; keep/unkeep/delete actions on clips
- **API Gateway HTTP v2** — `/` (no auth), `/list` and `/manage` (JWT Cognito authorizer)
- **Cognito** — OAuth2 Authorization Code flow; hosted UI; `id_token` in `Authorization` header
- **CloudFront + S3 webapp** — serves `index.html` rendered from `webapp_src/index.html.tpl`

### Security Model

- Device → presign Lambda: API key in `x-api-key` header (checked in Lambda, not API Gateway)
- Device → S3: 5-minute presigned PUT URLs
- Browser → Lambda: Cognito JWT `id_token`
- Browser → S3: 7-day presigned GET URLs from `list` Lambda
- S3 bucket is never public

## Key Lessons (from LESSONS.md)

- **OV2640 mode switching** requires full `esp_camera_deinit()` + `esp_camera_init()` — partial reconfiguration doesn't work
- **FPS gate is mandatory** — OV2640 runs ~25 fps natively; without gating, AVI index overflows
- **Discard 3 frames** after every camera reinit for AE settling
- **GPIO48** (LCD backlight on S3-EYE) is active-low
- **SD free-space queries** can block for seconds on large cards — run in a low-priority background task
- **Upload chunk size** should be 32 KB (not 4 KB) — 8× fewer SD reads and TCP segments
- **Presigned URL response buffer** must be ≥ 4096 bytes — STS tokens can reach ~1900 bytes each
- **`MOTION_STOP_TIMEOUT`** must exceed the motion-check interval or recording stops prematurely
- **Motion-stop during recording** is detected via JPEG size differencing (not mode switching, which causes 370 ms gaps)

## Project Status

- **Phase 1–3 complete:** Firmware + S3 upload + email alerts + LCD UI + web gallery
- **Phase 4 planned:** ESP32-P4 port (MIPI-CSI + ISP, H.264 recording) — stub code exists in `esp32p4/` component variants
