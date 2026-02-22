# Random suffix so the bucket name is globally unique
resource "random_id" "bucket_suffix" {
  byte_length = 4
}

resource "aws_s3_bucket" "clips" {
  bucket = "${var.project_name}-clips-${random_id.bucket_suffix.hex}"

  tags = {
    Project = var.project_name
  }
}

# Block all public access — clips are served via presigned GET URLs only
resource "aws_s3_bucket_public_access_block" "clips" {
  bucket = aws_s3_bucket.clips.id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

# Lifecycle rules:
#
#  expire-clips  — delete clips tagged keep=false after 30 days.
#                  Clips tagged keep=true are exempt (preserved indefinitely).
#                  New clips are tagged keep=false by the notify Lambda at upload.
#
#  expire-thumbs — delete all thumbnails after 30 days (no tag filter).
#                  Thumbnails are not tagged; they always expire on schedule.
#                  Thumbnails of kept clips will show as "no thumbnail" after
#                  30 days, but the clip download link continues to work.
resource "aws_s3_bucket_lifecycle_configuration" "clips" {
  bucket = aws_s3_bucket.clips.id

  rule {
    id     = "expire-clips"
    status = "Enabled"

    filter {
      and {
        prefix = "clips/"
        tags = {
          keep = "false"
        }
      }
    }

    expiration {
      days = 30
    }
  }

  rule {
    id     = "expire-thumbs"
    status = "Enabled"

    filter {
      prefix = "thumbs/"
    }

    expiration {
      days = 30
    }
  }
}

# One-time migration: tag any clips that existed before keep support was added.
# Without this, old untagged clips would be invisible to the tag-filtered lifecycle
# rule and would accumulate without ever being deleted.
resource "null_resource" "tag_existing_clips" {
  provisioner "local-exec" {
    command = <<-EOT
      echo "Tagging existing clips with keep=false..."
      aws s3api list-objects-v2 \
        --bucket ${aws_s3_bucket.clips.bucket} \
        --prefix clips/ \
        --query 'Contents[?ends_with(Key, `.avi`)].Key' \
        --output text \
        --region ${var.aws_region} | \
      tr '\t' '\n' | \
      while read key; do
        [ -z "$key" ] && continue
        # Only tag if no keep tag is set yet
        tag=$(aws s3api get-object-tagging \
          --bucket ${aws_s3_bucket.clips.bucket} \
          --key "$key" \
          --region ${var.aws_region} \
          --query "TagSet[?Key=='keep'].Value" \
          --output text 2>/dev/null)
        if [ -z "$tag" ]; then
          aws s3api put-object-tagging \
            --bucket ${aws_s3_bucket.clips.bucket} \
            --key "$key" \
            --tagging 'TagSet=[{Key=keep,Value=false}]' \
            --region ${var.aws_region}
          echo "  Tagged: $key"
        else
          echo "  Already tagged ($tag): $key"
        fi
      done
      echo "Migration done."
    EOT
  }

  depends_on = [aws_s3_bucket_lifecycle_configuration.clips]
}

# S3 → Lambda notification: fire notify Lambda when a new .avi is uploaded
resource "aws_s3_bucket_notification" "clips" {
  bucket = aws_s3_bucket.clips.id

  lambda_function {
    lambda_function_arn = aws_lambda_function.notify.arn
    events              = ["s3:ObjectCreated:*"]
    filter_prefix       = "clips/"
    filter_suffix       = ".avi"
  }

  depends_on = [aws_lambda_permission.s3_invoke_notify]
}
