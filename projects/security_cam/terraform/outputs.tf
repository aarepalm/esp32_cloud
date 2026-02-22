output "presign_url" {
  value       = aws_apigatewayv2_stage.presign.invoke_url
  description = "Paste this into sdkconfig.defaults.local as CONFIG_LAMBDA_PRESIGN_URL (no trailing slash needed)"
}

output "clips_bucket" {
  value       = aws_s3_bucket.clips.bucket
  description = "S3 bucket storing clips and thumbnails"
}

output "ses_verification_status" {
  value       = "Check email ${var.alert_email} and click the SES verification link before testing"
  description = "Reminder to verify SES email"
}

output "gallery_url" {
  value       = local.webapp_url
  description = "Open this URL to view the security camera gallery"
}

output "cognito_login_url" {
  value       = "https://${aws_cognito_user_pool_domain.gallery.domain}.auth.${var.aws_region}.amazoncognito.com/login"
  description = "Direct Cognito hosted login URL"
}
