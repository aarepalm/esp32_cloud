# Verify the alert email address with SES.
#
# Terraform creates the identity and SES sends a verification email.
# You MUST click the link in that email before the notify Lambda can
# send alerts — SES sandbox only allows sending to verified addresses.

resource "aws_sesv2_email_identity" "alert" {
  email_identity = var.alert_email

  tags = {
    Project = var.project_name
  }
}
