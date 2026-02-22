"""
notify.py — Send a motion alert email when a new clip lands in S3.

Triggered by S3 ObjectCreated event on clips/*.avi.
Generates a presigned GET URL (7-day expiry) so the recipient can
download or play the clip directly from the email link.
"""

import boto3
import json
import os
import urllib.parse

s3  = boto3.client('s3')
ses = boto3.client('ses')

BUCKET      = os.environ['CLIP_BUCKET']
ALERT_EMAIL = os.environ['ALERT_EMAIL']
GET_EXPIRY  = 7 * 24 * 3600   # 7 days


def handler(event, context):
    for record in event.get('Records', []):
        key = urllib.parse.unquote_plus(record['s3']['object']['key'])

        if not key.endswith('.avi'):
            continue

        clip_name = key.split('/')[-1]                        # esp32-eye-01_19700101_005549.avi
        device_id = clip_name.split('_')[0]                   # esp32-eye-01

        # Presigned GET URL — browser can download/play directly
        view_url = s3.generate_presigned_url(
            'get_object',
            Params={'Bucket': BUCKET, 'Key': key},
            ExpiresIn=GET_EXPIRY,
        )

        subject = f'Motion detected — {device_id}'
        body = (
            f'Motion detected on {device_id}\n\n'
            f'Clip: {clip_name}\n\n'
            f'Download / play (link expires in 7 days):\n'
            f'{view_url}\n'
        )

        ses.send_email(
            Source=ALERT_EMAIL,
            Destination={'ToAddresses': [ALERT_EMAIL]},
            Message={
                'Subject': {'Data': subject},
                'Body':    {'Text': {'Data': body}},
            },
        )

        print(f'Alert sent for {clip_name}')

        # Tag the clip so the lifecycle rule knows it is not kept.
        # The thumbnail is uploaded by the device immediately after the clip,
        # but the S3 event fires as soon as the clip lands — the thumbnail may
        # not exist yet, so we only tag the clip here.  The manage Lambda tags
        # the thumbnail if the user explicitly keeps or un-keeps a clip.
        try:
            s3.put_object_tagging(
                Bucket=BUCKET,
                Key=key,
                Tagging={'TagSet': [{'Key': 'keep', 'Value': 'false'}]},
            )
            print(f'Tagged keep=false: {key}')
        except Exception as e:
            print(f'Warning: could not tag {key}: {e}')
