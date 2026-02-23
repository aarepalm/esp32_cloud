# Security Camera — Lessons Learned

Hard-won lessons from building and debugging this project. Grouped by area.

---

## OV2640 / Camera Driver

### Mode switching requires full deinit + reinit

Using the sensor API (`set_pixformat()`) to switch between GRAYSCALE and JPEG mode
fails in both directions:
- GRAY→JPEG: DMA stays in grayscale mode; every frame is 76800 raw bytes, no JPEG header.
- JPEG→GRAY: OV2640 PLL ends up broken; VSYNC stops; `fb_get()` hangs forever.

**Fix:** Always use `esp_camera_deinit()` + `esp_camera_init()` for both transitions.
Takes ~250 ms but is the only reliable approach.

### FPS gate is mandatory in the recording loop

OV2640 at VGA outputs ~25 fps natively. Without a gate, the idx1 index (sized for
60 s × 10 fps = 600 frames) overflows after 24 s, silently dropping all subsequent frames.

**Fix:** Write a frame only if `esp_timer_get_time() >= next_frame_time`. Advance
`next_frame_time` by `1 000 000 / fps` µs per written frame.

### Discard 3 frames after every MOTION→RECORD reinit

The OV2640 resets its AE state on every reinit. Frame 1 has wrong exposure (~10 KB vs
normal ~14 KB), causing a visible brightness flash at the cut.

**Fix:** Discard 3 frames after every reinit. 1 is not enough; 2 reduces but does not
eliminate the artefact; 3 is clean. Cost: ~120 ms per motion check cycle.

### GPIO48 backlight on S3-EYE is active-low

GPIO48 (backlight) is active-low on the S3-EYE. Driving it HIGH switches the
backlight OFF. Init and toggle must invert polarity:

```c
gpio_set_level(LCD_BL, on ? 0 : 1);
```

This caused the "display looks frozen" bug: the refresh task was drawing at 4 Hz to
a correctly functioning panel, but the backlight was always off.

### `esp_vfs_fat_info()` can block for seconds on large SD cards

`f_getfree()` scans the entire FAT to count free clusters. On a 60 GB SD card this
blocks for several seconds. Calling it from the UI refresh loop at 4 Hz produces
an effectively frozen display.

**Fix:** Run `esp_vfs_fat_info()` in a dedicated low-priority task every 5 s.
The refresh task reads cached globals; it never touches the filesystem.

---

## S3 Upload Sizing

### 32 KB chunks, not 4 KB

A 4 KB read buffer + 4 KB `buffer_size_tx` produces 2200+ iterations of
`fread` + `esp_http_client_write` for a 9 MB clip. FAT and TCP overhead per
small call is significant.

**Fix:** 32 KB PSRAM-allocated buffer + `buffer_size_tx=32768`. Reduces to ~280
iterations (8× fewer SD reads and TCP segments). Throughput: 300–600 KB/s on home WiFi.

### RESP_BUF_LEN must be ≥ 4096

Two STS-signed presigned PUT URLs in one JSON response can reach ~1900 bytes
(large `X-Amz-Security-Token` embedded in each URL). A 2048-byte buffer is silently
truncated, causing `cJSON_Parse()` to return NULL with no error logged.

**Fix:** Set `RESP_BUF_LEN = 4096`.

### MOTION_STOP_TIMEOUT_S must exceed the check interval

`MOTION_CHECK_FRAMES=50` at 10 fps = ~5 s between motion checks. If
`MOTION_STOP_TIMEOUT_S ≤ 5`, a single failed check immediately stops recording —
the ~340 ms mode-switch round-trip consumes the entire timeout.

**Rule:** `MOTION_STOP_TIMEOUT_S > MOTION_CHECK_FRAMES / fps`. Use 12 s so two
consecutive failed checks are required before recording stops.

---

## AWS Account Constraints

### API key auth for the presign endpoint

API Gateway HTTP API v2 does not support native API keys (that is a REST API v1
feature). To protect the presign endpoint, the check lives in the Lambda itself:
the device sends an `x-api-key` header, the Lambda compares it against an `API_KEY`
environment variable, and returns HTTP 403 if they don't match.

The key is stored in `terraform.tfvars` (gitignored) as `presign_api_key` and on
the device in `sdkconfig.defaults.local` (gitignored) as `CONFIG_API_KEY`. Generate
a strong value with `openssl rand -hex 32`. This is the same pattern used in the
telemetry project.

### Lambda Function URLs return 403 on some new accounts

Lambda Function URLs with `authorization_type=NONE` return HTTP 403 on this account
even with correct resource policy and no SCPs. CloudWatch shows requests never reach
Lambda.

**Workaround:** Use API Gateway HTTP API v2. `aws_apigatewayv2_api` + integration +
route + stage + `aws_lambda_permission` (source: `apigateway.amazonaws.com`) works
correctly.

### Lambda reserved concurrency

New accounts cannot set `reserved_concurrent_executions` if total unreserved
concurrency would drop below 10. Remove the reserved concurrency Terraform resource
if you hit this error.

### SES + Proton Mail: silent drop

SES sends successfully (CloudWatch logs "Alert sent", SES shows 0 bounces), but
Proton Mail silently discards all SES mail — nothing appears in any folder.

**Fix:** Use Gmail as the alert address.

---

## Terraform Patterns

### OAC (Origin Access Control) instead of public S3 website

OAC is the newer, better pattern for serving a private S3 bucket through CloudFront.
The S3 bucket has no public access; CloudFront authenticates via IAM SigV4. Contrast
with the older telemetry project which uses a public S3 website as CloudFront origin.

### API Gateway HTTP API v2 for Lambda triggers

`aws_apigatewayv2_api` with `protocol_type=HTTP` is the right resource type for
Lambda-backed HTTP endpoints. Simpler and cheaper than REST API (v1).

Required Terraform resources: `aws_apigatewayv2_api`, `aws_apigatewayv2_integration`,
`aws_apigatewayv2_route`, `aws_apigatewayv2_stage`, `aws_lambda_permission`.

### JS template literals conflict with Terraform templatefile()

Terraform `templatefile()` uses `${...}` syntax for variable substitution — the same
syntax as JavaScript template literals. If the template HTML uses backtick strings
with `${...}`, Terraform errors out or silently drops the substitution.

**Fix:** Use plain string concatenation in the JavaScript inside `.tpl` files.
No `$${...}` escaping needed if template literals are avoided entirely.

### Cognito hosted UI requires HTTPS callback URL

Cognito will reject any non-localhost callback URL that uses HTTP. The CloudFront
distribution HTTPS URL must be set as the callback URL. This means CloudFront must
be deployed before the Cognito app client can be fully configured.

---

## Motion Detection Tuning

### Default threshold was calibrated for the wrong frame size

The original Kconfig default said "downscaled 80×60" in the help text, but the actual
motion detection frame is QVGA (320×240 = 76 800 pixels). `MOTION_THRESHOLD=500` on
76 800 pixels = 0.65% of the frame — triggers on any slight lighting change.

**Starting value:** 2000 (2.6% of QVGA). Per-pixel threshold 40 (16%) filters lamp
flicker and cloud shadows effectively for indoor/outdoor cameras.

| Parameter | Too low (problem) | Recommended | Too high (problem) |
|-----------|------------------|-------------|-------------------|
| `MOTION_THRESHOLD` | Triggers on light flicker | 2000 | Misses real motion |
| `pixel_threshold` | Triggers on global brightness shifts | 40 | Misses subtle motion |
| `MOTION_STOP_TIMEOUT_S` | Stops on AE settling after check | 12 | Keeps recording long after motion ends |

### sdkconfig.defaults changes do not apply to existing sdkconfig

`sdkconfig.defaults` is only merged into `sdkconfig` when `sdkconfig` does not exist.
Changing a default after first build has no effect until you either:
- Edit `sdkconfig` directly (change takes effect on next build), or
- Run `idf.py fullclean` (destroys build, forces regeneration from defaults)
