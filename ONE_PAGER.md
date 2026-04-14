# ESP32 Security Camera — One Pager

## What It Is

A motion-triggered security camera running on an **ESP32-S3-EYE** development board. When motion is detected, it records a video clip to an SD card, uploads it to AWS, and emails you an alert — all without any cloud latency in the detection loop.

---

## Hardware

| | |
|---|---|
| **Board** | ESP32-S3-EYE (dual-core LX7 @ 240 MHz, 8 MB PSRAM) |
| **Camera** | OV2640 via DVP; hardware JPEG encoder |
| **Storage** | Micro SD via SDMMC |
| **Display** | ST7789V 240×240 LCD, 4 Hz status updates |
| **Input** | 4-button ADC resistor ladder |

---

## What the Firmware Does

```
INIT → MOTION_WATCH → (score > threshold) → RECORDING → MOTION_WATCH
                                                   ↓
                                        Background upload queue
```

1. **Motion watch** — OV2640 streams QVGA grayscale at ~25 fps; frame-differencing scores each frame. Zero cloud round-trip.
2. **Record** — On trigger, switches to SVGA JPEG mode and writes MJPEG/AVI to SD at 10 fps (max 60 s, auto-chains). First frame saved as thumbnail.
3. **Upload** — Background FreeRTOS task fetches presigned PUT URLs from AWS, uploads clip + thumbnail in 32 KB chunks, then deletes local files.

---

## AWS Cloud Pipeline

```
Device ──HTTPS──► API Gateway ──► presign Lambda  ──► presigned PUT URL
                                                            │
                                                        S3 bucket
                                                    clips/*.avi
                                                    thumbs/*.jpg
                                                            │
                                            S3 event ──► notify Lambda
                                                            │
                                                       SES ──► Gmail alert

Browser ──HTTPS──► CloudFront ──► index.html (S3, private)
         Cognito login (OAuth2) → JWT
         JWT ──► API Gateway ──► list Lambda   → presigned GET URLs (7 day)
                              ──► manage Lambda → keep / delete clips
```

| Lambda | Auth | Role |
|--------|------|------|
| `presign` | API key (`x-api-key` header) | Returns 5-min PUT URLs for device upload |
| `notify` | S3 trigger | Tags clip `keep=false`; sends SES email |
| `list` | Cognito JWT | Returns clip list + 7-day presigned GET URLs |
| `manage` | Cognito JWT | Keep / unkeep / delete clips |

---

## Security Model

- **Device → S3:** short-lived presigned PUT URLs; API key verified in Lambda
- **Browser → API:** Cognito `id_token` (JWT, 1-hour expiry); validated by API Gateway
- **Browser → S3:** 7-day presigned GET URLs generated server-side
- **S3 buckets:** never public; webapp served via CloudFront + OAC

---

## Keep / Delete Pattern

Every uploaded clip is auto-tagged `keep=false`. S3 lifecycle deletes `keep=false` clips after 30 days. The gallery "Keep" button sets `keep=true` to exempt a clip permanently. "Unkeep" restores the tag and resuming the countdown.

---

## Project Status

| Phase | Status |
|-------|--------|
| Firmware + S3 upload + email alerts | **Complete** |
| LCD UI + button control | **Complete** |
| Web gallery (keep/delete, Cognito login) | **Complete** |
| ESP32-P4 port (MIPI-CSI, H.264) | **Planned** |

---

## Tech Stack

**Firmware:** C, ESP-IDF v5.4, FreeRTOS, esp_camera, SDMMC, HTTPS (mbedTLS)
**Cloud:** Terraform, AWS Lambda (Python), API Gateway HTTP v2, S3, SES, Cognito, CloudFront
**Region:** eu-north-1
