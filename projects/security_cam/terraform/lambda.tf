# ---------------------------------------------------------------------------
# Lambda #1 — presign
#
# Called directly by the ESP32 device via HTTP GET:
#   GET <function_url>?clip=X.avi&thumb=X_thumb.jpg
#   → { "clip_url": "https://...", "thumb_url": "https://..." }
#
# Returns presigned PUT URLs the device uses to upload directly to S3.
# Exposed via Lambda Function URL (no API Gateway needed, no auth —
# the presigned URLs themselves expire after 5 minutes).
# ---------------------------------------------------------------------------

data "archive_file" "presign" {
  type        = "zip"
  source_file = "${path.module}/lambda_src/presign.py"
  output_path = "${path.module}/lambda_src/presign.zip"
}

resource "aws_lambda_function" "presign" {
  function_name    = "${var.project_name}-presign"
  filename         = data.archive_file.presign.output_path
  source_code_hash = data.archive_file.presign.output_base64sha256
  role             = aws_iam_role.lambda.arn
  handler          = "presign.handler"
  runtime          = "python3.12"
  timeout          = 10

  environment {
    variables = {
      CLIP_BUCKET = aws_s3_bucket.clips.bucket
      API_KEY     = var.presign_api_key
    }
  }

  tags = {
    Project = var.project_name
  }
}

# API Gateway HTTP API v2 — gives the device a stable HTTPS endpoint.
# Lambda Function URLs returned persistent 403 AccessDeniedException on this
# account despite correct policy; API Gateway is the reliable alternative.
resource "aws_apigatewayv2_api" "presign" {
  name          = "${var.project_name}-presign"
  protocol_type = "HTTP"

  # Allow the gallery CloudFront origin to call GET /list from the browser.
  # Device calls to GET / are unaffected (no Origin header sent by ESP32).
  cors_configuration {
    allow_origins = [local.webapp_url]
    allow_methods = ["GET", "POST"]
    allow_headers = ["authorization", "content-type"]
  }
}

resource "aws_apigatewayv2_integration" "presign" {
  api_id                 = aws_apigatewayv2_api.presign.id
  integration_type       = "AWS_PROXY"
  integration_uri        = aws_lambda_function.presign.invoke_arn
  payload_format_version = "2.0"
}

resource "aws_apigatewayv2_route" "presign" {
  api_id    = aws_apigatewayv2_api.presign.id
  route_key = "GET /"
  target    = "integrations/${aws_apigatewayv2_integration.presign.id}"
}

resource "aws_apigatewayv2_stage" "presign" {
  api_id      = aws_apigatewayv2_api.presign.id
  name        = "$default"
  auto_deploy = true
}

resource "aws_lambda_permission" "apigw_invoke_presign" {
  statement_id  = "AllowAPIGatewayInvoke"
  action        = "lambda:InvokeFunction"
  function_name = aws_lambda_function.presign.function_name
  principal     = "apigateway.amazonaws.com"
  source_arn    = "${aws_apigatewayv2_api.presign.execution_arn}/*/*"
}

resource "aws_cloudwatch_log_group" "presign" {
  name              = "/aws/lambda/${aws_lambda_function.presign.function_name}"
  retention_in_days = 7
}

# ---------------------------------------------------------------------------
# Lambda #2 — notify
#
# Triggered by S3 when a new .avi clip lands in clips/.
# Generates a presigned GET URL (7-day expiry) and sends an SES email.
# ---------------------------------------------------------------------------

data "archive_file" "notify" {
  type        = "zip"
  source_file = "${path.module}/lambda_src/notify.py"
  output_path = "${path.module}/lambda_src/notify.zip"
}

resource "aws_lambda_function" "notify" {
  function_name    = "${var.project_name}-notify"
  filename         = data.archive_file.notify.output_path
  source_code_hash = data.archive_file.notify.output_base64sha256
  role             = aws_iam_role.lambda.arn
  handler          = "notify.handler"
  runtime          = "python3.12"
  timeout          = 30

  environment {
    variables = {
      CLIP_BUCKET = aws_s3_bucket.clips.bucket
      ALERT_EMAIL = var.alert_email
    }
  }

  tags = {
    Project = var.project_name
  }
}

# Allow S3 to invoke the notify Lambda
resource "aws_lambda_permission" "s3_invoke_notify" {
  statement_id  = "AllowS3Invoke"
  action        = "lambda:InvokeFunction"
  function_name = aws_lambda_function.notify.function_name
  principal     = "s3.amazonaws.com"
  source_arn    = aws_s3_bucket.clips.arn
}

resource "aws_cloudwatch_log_group" "notify" {
  name              = "/aws/lambda/${aws_lambda_function.notify.function_name}"
  retention_in_days = 7
}
