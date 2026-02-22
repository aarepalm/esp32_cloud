"""
list.py — Return a sorted list of all clips with presigned GET URLs.

Called by the web gallery (JWT-authenticated):
  GET /list   Authorization: Bearer <id_token>

Returns JSON array sorted newest-first:
  [
    {
      "clip_key":  "clips/CAM1_20260221_120000.avi",
      "clip_url":  "https://...",    (7-day presigned GET)
      "thumb_url": "https://...",    (7-day presigned GET, or null if missing)
      "timestamp": "2026-02-21T12:00:00+00:00",
      "size_mb":   9.4
    },
    ...
  ]

Clip filename format: <device_id>_YYYYMMDD_HHMMSS.avi
Thumb filename format: <device_id>_YYYYMMDD_HHMMSS_thumb.jpg (under thumbs/ prefix)
"""

import boto3
import json
import os
import re
from datetime import datetime, timezone
from botocore.exceptions import ClientError

s3        = boto3.client('s3')
paginator = s3.get_paginator('list_objects_v2')

BUCKET     = os.environ['CLIP_BUCKET']
GET_EXPIRY = 7 * 24 * 3600   # 7 days


def parse_timestamp(name):
    """Parse DEVICE_YYYYMMDD_HHMMSS from clip basename. Returns ISO 8601 string or None."""
    m = re.search(r'_(\d{8})_(\d{6})$', name)
    if not m:
        return None
    try:
        dt = datetime.strptime(m.group(1) + m.group(2), '%Y%m%d%H%M%S')
        return dt.replace(tzinfo=timezone.utc).isoformat()
    except ValueError:
        return None


def handler(event, context):
    clips = []

    pages = paginator.paginate(
        Bucket=BUCKET,
        Prefix='clips/',
        PaginationConfig={'PageSize': 1000},
    )
    for page in pages:
        for obj in page.get('Contents', []):
            key = obj['Key']
            if not key.endswith('.avi'):
                continue

            # Base name: strip "clips/" prefix and ".avi" suffix
            name = key[len('clips/'):-len('.avi')]

            # Presigned GET URL for the clip (7-day expiry)
            clip_url = s3.generate_presigned_url(
                'get_object',
                Params={'Bucket': BUCKET, 'Key': key},
                ExpiresIn=GET_EXPIRY,
            )

            # Check for paired thumbnail; generate presigned GET if present
            thumb_key = f'thumbs/{name}_thumb.jpg'
            thumb_url = None
            try:
                s3.head_object(Bucket=BUCKET, Key=thumb_key)
                thumb_url = s3.generate_presigned_url(
                    'get_object',
                    Params={'Bucket': BUCKET, 'Key': thumb_key},
                    ExpiresIn=GET_EXPIRY,
                )
            except ClientError:
                pass  # thumbnail not present — leave thumb_url as None

            # Parse timestamp from filename; fall back to S3 LastModified
            ts = parse_timestamp(name) or obj['LastModified'].isoformat()

            # Read keep tag (missing tag = not kept)
            kept = False
            try:
                tags = s3.get_object_tagging(Bucket=BUCKET, Key=key)
                kept = any(
                    t['Key'] == 'keep' and t['Value'] == 'true'
                    for t in tags.get('TagSet', [])
                )
            except ClientError:
                pass

            clips.append({
                'clip_key':  key,
                'clip_url':  clip_url,
                'thumb_url': thumb_url,
                'timestamp': ts,
                'size_mb':   round(obj['Size'] / 1_048_576, 2),
                'kept':      kept,
            })

    # Newest clip first (ISO 8601 strings sort lexicographically)
    clips.sort(key=lambda c: c['timestamp'], reverse=True)

    return {
        'statusCode': 200,
        'headers': {'Content-Type': 'application/json'},
        'body': json.dumps(clips),
    }
