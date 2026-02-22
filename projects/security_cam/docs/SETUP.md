# Security Camera — Setup Guide

Step-by-step guide to build and deploy the security camera from scratch.

---

## 1. Prerequisites

### Hardware

- ESP32-S3-EYE development board
- USB cable (micro-USB)
- Micro SD card (any size; FAT32 formatted)
- Internet-connected WiFi network

### Software

| Tool | Version | Install |
|------|---------|---------|
| ESP-IDF | v5.4 | See step 2 |
| Terraform | ≥ 1.7 | `apt install terraform` or [terraform.io](https://developer.hashicorp.com/terraform/install) |
| AWS CLI | v2 | [aws.amazon.com/cli](https://aws.amazon.com/cli/) |
| Python | 3.x | Bundled with ESP-IDF |

### AWS account

Follow [../../docs/AWS_SETUP.md](../../docs/AWS_SETUP.md) before proceeding:
- IAM admin user + AWS CLI profile configured
- SES: recipient Gmail address verified
- Optional: budget alert + kill-switch

---

## 2. Clone and install ESP-IDF

```bash
git clone https://github.com/aarepalm/esp32_cloud.git
cd esp32_cloud

# Install ESP-IDF v5.4 into the expected location
git clone --recursive --branch v5.4 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
cd ..
```

Activate the environment (do this in every new shell session):

```bash
. ./activate.sh
```

Verify: `idf.py --version` should print `ESP-IDF v5.4`.

---

## 3. Firmware

### 3a. Create your local credentials file

```bash
cd projects/security_cam/firmware
cp sdkconfig.defaults.local.example sdkconfig.defaults.local
```

Edit `sdkconfig.defaults.local` with your values:

```
CONFIG_WIFI_SSID="your-wifi-ssid"
CONFIG_WIFI_PASSWORD="your-wifi-password"
CONFIG_LAMBDA_PRESIGN_URL="https://xxx.execute-api.eu-north-1.amazonaws.com"
CONFIG_DEVICE_ID="cam01"
```

The presign URL comes from Terraform output (step 4). You can build without it first
and flash again after deploying the infrastructure.

### 3b. Set the target and build

```bash
cd projects/security_cam/firmware
idf.py set-target esp32s3
idf.py build
```

### 3c. Flash and monitor

Insert the SD card. Connect the ESP32-S3-EYE via USB.

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Port is `/dev/ttyACM0` — the S3-EYE uses built-in USB Serial/JTAG, not an external
CH340 chip. If the port is busy: `kill $(fuser /dev/ttyACM0)`.

Expected boot output:
```
I sdcard: SD card mounted at /sdcard
I wifi_manager: Connected, IP: 192.168.x.x
I camera_hal: Camera init OK (MOTION mode)
I cloud_client: Presign URL: https://...
W main: WATCHING
```

---

## 4. Terraform (AWS infrastructure)

### 4a. Create your tfvars file

```bash
cd projects/security_cam/terraform
cp terraform.tfvars.example terraform.tfvars
```

Edit `terraform.tfvars`:

```hcl
alert_email      = "your-gmail@gmail.com"
cognito_username = "admin"
cognito_password = "YourPassword123!"  # min 8 chars, upper+lower+digit+symbol
```

### 4b. Deploy

```bash
terraform init
terraform plan   # review what will be created
terraform apply
```

Approximate resource creation time: 3–5 minutes (CloudFront takes longest).

### 4c. Get the outputs

```bash
terraform output presign_url   # paste into sdkconfig.defaults.local
terraform output gallery_url   # open in browser
```

Update `sdkconfig.defaults.local` with the presign URL, then rebuild and reflash:

```bash
cd ../firmware
idf.py build flash -p /dev/ttyACM0
```

---

## 5. Verify end-to-end

1. **Trigger motion** — walk in front of the camera. Watch the serial monitor for:
   ```
   W main: >>> RECORD START
   W main: >>> RECORD STOP motion gone (12s idle) — 120 frames
   W main: >>> UPLOAD START
   W main: >>> UPLOAD OK
   ```

2. **Check S3** — `aws s3 ls s3://security-cam-clips-*/clips/` should show a new `.avi`.

3. **Check email** — Gmail should receive an alert within ~30 seconds of the upload.

4. **Open the gallery** — navigate to the `gallery_url` from terraform output.
   Log in with the `cognito_username` / `cognito_password` from `terraform.tfvars`.
   The new clip should appear as a thumbnail.

---

## 6. Motion detection tuning

Default values in `sdkconfig.defaults` (also settable in `sdkconfig.defaults.local`):

| Parameter | Default | Notes |
|-----------|---------|-------|
| `CONFIG_MOTION_THRESHOLD` | 2000 | Changed pixels in QVGA (76800 px) to trigger; 2000 ≈ 2.6% |
| `pixel_threshold` | 40 | Per-pixel change (0–255) counted as changed; 40 ≈ 16% |
| `MOTION_STOP_TIMEOUT_S` | 12 | Stop recording after 12 s with no motion detected |
| `CONFIG_RECORD_FPS` | 10 | Recording frame rate (FPS gate applied in firmware) |

If the camera triggers on ambient light changes (clouds, lamps), raise
`CONFIG_MOTION_THRESHOLD` to 3000–5000 or raise `pixel_threshold` to 50–60.

If the camera misses real motion, lower `CONFIG_MOTION_THRESHOLD`.

**Rule:** `MOTION_STOP_TIMEOUT_S` must be greater than
`MOTION_CHECK_FRAMES / CONFIG_RECORD_FPS` (the check interval, ~5 s at defaults).
If it equals or is less than the check interval, recording stops after the first
failed motion check even during active movement.

---

## 7. Teardown

To destroy all AWS resources:

```bash
cd projects/security_cam/terraform
terraform destroy
```

This removes all Lambda functions, S3 buckets (and their contents), API Gateway,
Cognito user pool, CloudFront distribution, and IAM roles created by Terraform.
