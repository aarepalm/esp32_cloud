# Security Camera — Project Overview

Motion-triggered security camera built on the ESP32-S3-EYE development board.
Detects motion via frame differencing, records MJPEG/AVI clips to SD card, uploads
to AWS S3, sends Gmail alerts, and serves a Cognito-protected web gallery.

## Features

- Motion detection via QVGA grayscale frame differencing (no ML, zero cloud round-trip)
- MJPEG/AVI recording at 10fps SVGA to SD card (max 60 s per clip, auto-chains)
- First frame of each clip saved as JPEG thumbnail
- Background upload to S3 via presigned PUT URL (recording and upload never overlap)
- SES email alert on motion with clip name and timestamp
- Web gallery: thumbnail grid, click to stream/download, keep/delete per clip
- Cognito OAuth2 login protects the gallery; S3 bucket stays fully private
- Live LCD status screen (240×240 ST7789V) + 4-button ADC resistor ladder

## Documentation

| File | Contents |
|------|----------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | System diagram, component table, security model, data flow |
| [HARDWARE.md](HARDWARE.md) | Board details, wiring, OV2640 quirks |
| [SETUP.md](SETUP.md) | Step-by-step rebuild guide |
| [LESSONS.md](LESSONS.md) | Hard-won lessons from development and debugging |
| [JOURNAL.md](JOURNAL.md) | Full development journal (historical record) |

## Status

Phase 1 (firmware + S3 upload + email) — complete
Phase 2 (LCD UI + button control) — complete
Phase 3 (web gallery + keep/delete) — complete
Phase 4 (ESP32-P4 port) — future
