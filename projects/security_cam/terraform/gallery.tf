# ---------------------------------------------------------------------------
# Gallery infrastructure
#
# 1. list Lambda       — lists clips, generates 7-day presigned GET URLs
# 2. API Gateway       — JWT-protected GET /list route on existing presign API
# 3. S3 webapp bucket  — private; serves index.html via CloudFront OAC
# 4. CloudFront        — HTTPS frontend for the gallery
# 5. index.html        — rendered from template at apply time, uploaded to S3
# ---------------------------------------------------------------------------

# ── list Lambda ──────────────────────────────────────────────────────────────

data "archive_file" "list" {
  type        = "zip"
  source_file = "${path.module}/lambda_src/list.py"
  output_path = "${path.module}/lambda_src/list.zip"
}

resource "aws_lambda_function" "list" {
  function_name    = "${var.project_name}-list"
  filename         = data.archive_file.list.output_path
  source_code_hash = data.archive_file.list.output_base64sha256
  role             = aws_iam_role.lambda.arn
  handler          = "list.handler"
  runtime          = "python3.12"
  timeout          = 30

  environment {
    variables = {
      CLIP_BUCKET = aws_s3_bucket.clips.bucket
    }
  }

  tags = {
    Project = var.project_name
  }
}

resource "aws_cloudwatch_log_group" "list" {
  name              = "/aws/lambda/${aws_lambda_function.list.function_name}"
  retention_in_days = 7
}

# ── API Gateway — JWT authorizer + /list route ────────────────────────────────

resource "aws_apigatewayv2_authorizer" "cognito" {
  api_id           = aws_apigatewayv2_api.presign.id
  authorizer_type  = "JWT"
  identity_sources = ["$request.header.Authorization"]
  name             = "cognito"

  jwt_configuration {
    audience = [aws_cognito_user_pool_client.webapp.id]
    issuer   = "https://cognito-idp.${var.aws_region}.amazonaws.com/${aws_cognito_user_pool.gallery.id}"
  }
}

resource "aws_apigatewayv2_integration" "list" {
  api_id                 = aws_apigatewayv2_api.presign.id
  integration_type       = "AWS_PROXY"
  integration_uri        = aws_lambda_function.list.invoke_arn
  payload_format_version = "2.0"
}

resource "aws_apigatewayv2_route" "list" {
  api_id             = aws_apigatewayv2_api.presign.id
  route_key          = "GET /list"
  target             = "integrations/${aws_apigatewayv2_integration.list.id}"
  authorization_type = "JWT"
  authorizer_id      = aws_apigatewayv2_authorizer.cognito.id
}

resource "aws_lambda_permission" "apigw_invoke_list" {
  statement_id  = "AllowAPIGatewayInvokeList"
  action        = "lambda:InvokeFunction"
  function_name = aws_lambda_function.list.function_name
  principal     = "apigateway.amazonaws.com"
  source_arn    = "${aws_apigatewayv2_api.presign.execution_arn}/*/*"
}

# ── manage Lambda (keep / unkeep / delete) ────────────────────────────────────

data "archive_file" "manage" {
  type        = "zip"
  source_file = "${path.module}/lambda_src/manage.py"
  output_path = "${path.module}/lambda_src/manage.zip"
}

resource "aws_lambda_function" "manage" {
  function_name    = "${var.project_name}-manage"
  filename         = data.archive_file.manage.output_path
  source_code_hash = data.archive_file.manage.output_base64sha256
  role             = aws_iam_role.lambda.arn
  handler          = "manage.handler"
  runtime          = "python3.12"
  timeout          = 15

  environment {
    variables = {
      CLIP_BUCKET = aws_s3_bucket.clips.bucket
    }
  }

  tags = {
    Project = var.project_name
  }
}

resource "aws_cloudwatch_log_group" "manage" {
  name              = "/aws/lambda/${aws_lambda_function.manage.function_name}"
  retention_in_days = 7
}

resource "aws_apigatewayv2_integration" "manage" {
  api_id                 = aws_apigatewayv2_api.presign.id
  integration_type       = "AWS_PROXY"
  integration_uri        = aws_lambda_function.manage.invoke_arn
  payload_format_version = "2.0"
}

resource "aws_apigatewayv2_route" "manage" {
  api_id             = aws_apigatewayv2_api.presign.id
  route_key          = "POST /manage"
  target             = "integrations/${aws_apigatewayv2_integration.manage.id}"
  authorization_type = "JWT"
  authorizer_id      = aws_apigatewayv2_authorizer.cognito.id
}

resource "aws_lambda_permission" "apigw_invoke_manage" {
  statement_id  = "AllowAPIGatewayInvokeManage"
  action        = "lambda:InvokeFunction"
  function_name = aws_lambda_function.manage.function_name
  principal     = "apigateway.amazonaws.com"
  source_arn    = "${aws_apigatewayv2_api.presign.execution_arn}/*/*"
}

# ── Webapp S3 bucket (private, CloudFront OAC access only) ───────────────────

resource "random_id" "webapp_suffix" {
  byte_length = 4
}

resource "aws_s3_bucket" "webapp" {
  bucket = "${var.project_name}-webapp-${random_id.webapp_suffix.hex}"

  tags = {
    Project = var.project_name
  }
}

resource "aws_s3_bucket_public_access_block" "webapp" {
  bucket = aws_s3_bucket.webapp.id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

# ── CloudFront Origin Access Control ─────────────────────────────────────────

resource "aws_cloudfront_origin_access_control" "webapp" {
  name                              = "${var.project_name}-webapp-oac"
  description                       = "OAC for security cam gallery"
  origin_access_control_origin_type = "s3"
  signing_behavior                  = "always"
  signing_protocol                  = "sigv4"
}

# ── CloudFront distribution ────────────────────────────────────────────────────

resource "aws_cloudfront_distribution" "webapp" {
  enabled             = true
  default_root_object = "index.html"

  origin {
    domain_name              = aws_s3_bucket.webapp.bucket_regional_domain_name
    origin_id                = "s3-oac"
    origin_access_control_id = aws_cloudfront_origin_access_control.webapp.id
  }

  default_cache_behavior {
    allowed_methods        = ["GET", "HEAD"]
    cached_methods         = ["GET", "HEAD"]
    target_origin_id       = "s3-oac"
    viewer_protocol_policy = "redirect-to-https"

    forwarded_values {
      query_string = false
      cookies { forward = "none" }
    }

    min_ttl     = 0
    default_ttl = 0  # no caching — always fetch latest index.html from S3
    max_ttl     = 0
  }

  restrictions {
    geo_restriction {
      restriction_type = "none"
    }
  }

  # Free *.cloudfront.net certificate — no custom domain needed
  viewer_certificate {
    cloudfront_default_certificate = true
  }

  tags = {
    Project = var.project_name
  }
}

# S3 bucket policy: allow CloudFront OAC to read objects
# Must be created after the distribution so we have its ARN
resource "aws_s3_bucket_policy" "webapp" {
  bucket     = aws_s3_bucket.webapp.id
  depends_on = [aws_s3_bucket_public_access_block.webapp]

  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Sid       = "AllowCloudFrontOAC"
      Effect    = "Allow"
      Principal = { Service = "cloudfront.amazonaws.com" }
      Action    = "s3:GetObject"
      Resource  = "${aws_s3_bucket.webapp.arn}/*"
      Condition = {
        StringEquals = {
          "AWS:SourceArn" = aws_cloudfront_distribution.webapp.arn
        }
      }
    }]
  })
}

# ── Local for CloudFront HTTPS URL (used in cognito.tf and outputs.tf) ────────

locals {
  webapp_url = "https://${aws_cloudfront_distribution.webapp.domain_name}"
}

# ── index.html — rendered from template, uploaded to S3 ──────────────────────

resource "aws_s3_object" "index_html" {
  bucket       = aws_s3_bucket.webapp.id
  key          = "index.html"
  content_type = "text/html"

  # templatefile() substitutes the 4 config values at apply time.
  # Terraform detects changes to content and re-uploads automatically.
  content = templatefile("${path.module}/webapp_src/index.html.tpl", {
    cognito_domain = "${aws_cognito_user_pool_domain.gallery.domain}.auth.${var.aws_region}.amazoncognito.com"
    client_id      = aws_cognito_user_pool_client.webapp.id
    redirect_uri   = local.webapp_url
    api_url        = aws_apigatewayv2_stage.presign.invoke_url
  })
}
