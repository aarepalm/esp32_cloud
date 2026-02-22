"""
manage.py — Keep and delete operations for gallery clips.

Called by the web gallery (JWT-authenticated):
  POST /manage
  Body: {"action": "keep"|"unkeep"|"delete", "clip_key": "clips/XXX.avi"}

Actions:
  keep   — tag clip with keep=true  (exempt from 30-day lifecycle deletion)
  unkeep — tag clip with keep=false (allow lifecycle to delete after 30 days)
  delete — immediately delete clip + thumbnail from S3

The thumbnail key is derived from the clip key:
  clips/DEVICE_YYYYMMDD_HHMMSS.avi  →  thumbs/DEVICE_YYYYMMDD_HHMMSS_thumb.jpg
"""

import boto3
import json
import os
from botocore.exceptions import ClientError

s3     = boto3.client('s3')
BUCKET = os.environ['CLIP_BUCKET']


def thumb_key(clip_key):
    """Derive thumbnail key from clip key."""
    name = clip_key[len('clips/'):-len('.avi')]
    return f'thumbs/{name}_thumb.jpg'


def set_keep_tag(clip_key, value):
    """Apply keep=<value> tag to clip. Returns True on success."""
    tag_set = [{'Key': 'keep', 'Value': value}]
    s3.put_object_tagging(
        Bucket=BUCKET,
        Key=clip_key,
        Tagging={'TagSet': tag_set},
    )


def delete_clip(clip_key):
    """Delete clip and its thumbnail from S3."""
    objects = [{'Key': clip_key}]
    tk = thumb_key(clip_key)
    try:
        s3.head_object(Bucket=BUCKET, Key=tk)
        objects.append({'Key': tk})
    except ClientError:
        pass  # thumbnail already gone — delete clip only

    s3.delete_objects(
        Bucket=BUCKET,
        Delete={'Objects': objects, 'Quiet': True},
    )


def handler(event, context):
    try:
        body = json.loads(event.get('body') or '{}')
        action   = body.get('action')
        clip_key = body.get('clip_key')
    except (json.JSONDecodeError, TypeError):
        return {'statusCode': 400, 'body': json.dumps({'error': 'Invalid JSON body'})}

    if not action or not clip_key:
        return {'statusCode': 400, 'body': json.dumps({'error': 'Missing action or clip_key'})}

    if not clip_key.startswith('clips/') or not clip_key.endswith('.avi'):
        return {'statusCode': 400, 'body': json.dumps({'error': 'clip_key must be clips/*.avi'})}

    try:
        if action == 'keep':
            set_keep_tag(clip_key, 'true')
            print(f'Marked keep=true: {clip_key}')
        elif action == 'unkeep':
            set_keep_tag(clip_key, 'false')
            print(f'Marked keep=false: {clip_key}')
        elif action == 'delete':
            delete_clip(clip_key)
            print(f'Deleted: {clip_key}')
        else:
            return {'statusCode': 400, 'body': json.dumps({'error': f'Unknown action: {action}'})}
    except ClientError as e:
        print(f'S3 error for {action} on {clip_key}: {e}')
        return {'statusCode': 500, 'body': json.dumps({'error': str(e)})}

    return {
        'statusCode': 200,
        'headers': {'Content-Type': 'application/json'},
        'body': json.dumps({'ok': True}),
    }
