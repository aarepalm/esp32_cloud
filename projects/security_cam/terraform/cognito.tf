# Cognito User Pool — manages gallery web login
#
# Flow:
#   1. Browser visits Cognito hosted login page
#   2. User enters username + password
#   3. Cognito redirects back to gallery with an auth code
#   4. Gallery JS exchanges code for JWT tokens
#   5. JWT included in Authorization header on GET /list
#   6. API Gateway validates JWT — no extra Lambda needed

resource "aws_cognito_user_pool" "gallery" {
  name = "${var.project_name}-gallery"

  password_policy {
    minimum_length    = 12
    require_lowercase = true
    require_uppercase = true
    require_numbers   = true
    require_symbols   = false
  }

  # No email verification required (single personal user, password set manually)
  auto_verified_attributes = []

  tags = {
    Project = var.project_name
  }
}

# Unique domain suffix for the hosted login page URL
resource "random_id" "cognito_domain" {
  byte_length = 4
}

# Hosted UI: https://<domain>.auth.eu-north-1.amazoncognito.com/login
resource "aws_cognito_user_pool_domain" "gallery" {
  domain       = "${var.project_name}-${random_id.cognito_domain.hex}"
  user_pool_id = aws_cognito_user_pool.gallery.id
}

# App client: represents the gallery web app
# generate_secret = false: browser JS apps cannot keep a secret
resource "aws_cognito_user_pool_client" "webapp" {
  name         = "${var.project_name}-webapp"
  user_pool_id = aws_cognito_user_pool.gallery.id

  generate_secret = false

  allowed_oauth_flows                  = ["code"]
  allowed_oauth_scopes                 = ["openid", "email"]
  allowed_oauth_flows_user_pool_client = true

  # CloudFront HTTPS URL is the only valid callback/logout destination
  callback_urls = [local.webapp_url]
  logout_urls   = [local.webapp_url]

  supported_identity_providers = ["COGNITO"]

  token_validity_units {
    access_token  = "hours"
    id_token      = "hours"
    refresh_token = "days"
  }

  access_token_validity  = 1  # JWT expires after 1 hour
  id_token_validity      = 1
  refresh_token_validity = 7  # silent re-login for 7 days
}

# Create the admin user
resource "aws_cognito_user" "admin" {
  user_pool_id = aws_cognito_user_pool.gallery.id
  username     = var.cognito_username

  attributes = {
    email          = var.alert_email
    email_verified = "true"
  }

  # SUPPRESS: don't send a welcome email
  message_action = "SUPPRESS"

  temporary_password = var.cognito_password
}

# Set password as permanent so no forced-reset on first login
resource "null_resource" "cognito_permanent_password" {
  depends_on = [aws_cognito_user.admin]

  provisioner "local-exec" {
    command = <<-EOT
      aws cognito-idp admin-set-user-password \
        --user-pool-id ${aws_cognito_user_pool.gallery.id} \
        --username ${var.cognito_username} \
        --password '${var.cognito_password}' \
        --permanent \
        --region ${var.aws_region}
    EOT
  }
}
