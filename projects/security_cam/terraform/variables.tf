variable "aws_region" {
  description = "AWS region for all resources"
  default     = "eu-north-1"
}

variable "project_name" {
  description = "Prefix for all resource names"
  default     = "security-cam"
}

variable "alert_email" {
  description = "Email address for motion alert notifications (SES send + receive)"
  type        = string
}

variable "cognito_username" {
  description = "Username for the gallery web login"
  type        = string
  default     = "admin"
}

variable "cognito_password" {
  description = "Password for the gallery web login (min 12 chars, upper+lower+number)"
  type        = string
  sensitive   = true
}
