"""
presign.py — Generate presigned S3 PUT URLs for the ESP32 device.

Called by the device:
  GET <function_url>?clip=X.avi&thumb=X_thumb.jpg

Returns:
  { "clip_url": "https://...", "thumb_url": "https://..." }

The device then PUTs the AVI clip and JPEG thumbnail directly to S3
using these URLs. No AWS credentials needed on the device.
"""

import boto3
import json
import os

s3 = boto3.client('s3')

BUCKET     = os.environ['CLIP_BUCKET']
API_KEY    = os.environ['API_KEY']
PUT_EXPIRY = 300   # 5 minutes — plenty of time for the device to upload


def handler(event, context):
    headers = event.get('headers') or {}
    if headers.get('x-api-key') != API_KEY:
        return {
            'statusCode': 403,
            'headers': {'Content-Type': 'application/json'},
            'body': json.dumps({'error': 'Forbidden'}),
        }

    params = event.get('queryStringParameters') or {}
    clip   = params.get('clip')
    thumb  = params.get('thumb')

    if not clip or not thumb:
        return {
            'statusCode': 400,
            'headers': {'Content-Type': 'application/json'},
            'body': json.dumps({'error': 'Missing clip or thumb parameter'}),
        }

    clip_url = s3.generate_presigned_url(
        'put_object',
        Params={'Bucket': BUCKET, 'Key': f'clips/{clip}'},
        ExpiresIn=PUT_EXPIRY,
    )
    thumb_url = s3.generate_presigned_url(
        'put_object',
        Params={'Bucket': BUCKET, 'Key': f'thumbs/{thumb}'},
        ExpiresIn=PUT_EXPIRY,
    )

    print(f'Presigned URLs generated for clip={clip} thumb={thumb}')

    return {
        'statusCode': 200,
        'headers': {'Content-Type': 'application/json'},
        'body': json.dumps({'clip_url': clip_url, 'thumb_url': thumb_url}),
    }
