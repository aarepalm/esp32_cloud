# Single execution role shared by both Lambda functions.
# Permissions are minimal — only what each Lambda actually needs:
#   - CloudWatch Logs: write log output (basic Lambda requirement)
#   - S3: PutObject (presign generates upload URLs) + GetObject (notify reads clip for email)
#   - SES: SendEmail (notify sends motion alert)

resource "aws_iam_role" "lambda" {
  name = "${var.project_name}-lambda-role"

  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Action    = "sts:AssumeRole"
      Effect    = "Allow"
      Principal = { Service = "lambda.amazonaws.com" }
    }]
  })

  tags = {
    Project = var.project_name
  }
}

resource "aws_iam_role_policy_attachment" "lambda_basic_logging" {
  role       = aws_iam_role.lambda.name
  policy_arn = "arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole"
}

resource "aws_iam_role_policy" "lambda_s3_ses" {
  name = "s3-ses-access"
  role = aws_iam_role.lambda.id

  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Effect = "Allow"
        Action = [
          "s3:PutObject",
          "s3:GetObject",
          "s3:DeleteObject",       # manage Lambda: delete clip + thumbnail
          "s3:GetObjectTagging",   # list Lambda: read keep tag per clip
          "s3:PutObjectTagging",   # notify + manage Lambda: set keep tag
        ]
        Resource = "${aws_s3_bucket.clips.arn}/*"
      },
      {
        # ListBucket required by list Lambda to paginate clips/
        Effect   = "Allow"
        Action   = "s3:ListBucket"
        Resource = aws_s3_bucket.clips.arn
      },
      {
        # SES SendEmail — scoped to the verified identity only
        Effect   = "Allow"
        Action   = "ses:SendEmail"
        Resource = "*"
      }
    ]
  })
}
