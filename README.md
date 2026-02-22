# esp32_cloud

Personal learning projects connecting ESP32 hardware to AWS. Built as a hands-on
introduction to cloud-connected embedded systems: starting with a simple telemetry
"Hello World", then a full motion-triggered security camera with S3 storage, email
alerts, and a Cognito-protected web gallery.

## Repo layout

| Path | Description |
|------|-------------|
| `activate.sh` | Sources ESP-IDF environment (run once per shell session) |
| `esp-idf/` | ESP-IDF v5.4 framework — **not committed** (install separately, ~1 GB) |
| `projects/security_cam/` | Motion-triggered security camera — complete |
| `docs/AWS_SETUP.md` | One-time AWS account setup required before any project |

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| ESP-IDF | v5.4 | Install to `esp-idf/` at repo root; see [Espressif docs](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/get-started/) |
| Terraform | ≥ 1.7 | Infrastructure as code for all AWS resources |
| AWS CLI | v2 | Configured with `aws configure` (profile: default, region: eu-north-1) |
| `gh` CLI | any | Optional — only needed if creating the GitHub repo from the command line |

## Quick start

```bash
# 1. Clone and install ESP-IDF
git clone https://github.com/aarepalm/esp32_cloud.git
cd esp32_cloud
git clone --recursive --branch v5.4 https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3 && cd ..

# 2. Activate IDF environment (do this in every new shell)
. ./activate.sh

# 3. Follow the project README for build + deploy steps
```

See [projects/security_cam/docs/README.md](projects/security_cam/docs/README.md)
for the full security camera setup guide.

Before deploying any project, follow [docs/AWS_SETUP.md](docs/AWS_SETUP.md) to
prepare your AWS account.

## License

MIT — see [LICENSE](LICENSE).
