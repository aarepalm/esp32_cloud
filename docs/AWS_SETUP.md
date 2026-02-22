# AWS Account Setup

One-time account preparation needed before deploying any project in this repo.
These steps are not automated by Terraform — they are prerequisites.

---

## 1. IAM admin user (avoid using root)

AWS best practice: never use the root account for day-to-day work.

1. Sign in as root → IAM → Users → Create user
2. Attach policy: `AdministratorAccess`
3. Enable console access, set a strong password
4. Enable MFA on both root and the admin user
5. Sign out of root; use the admin IAM user for everything else

---

## 2. AWS CLI profile setup

```bash
aws configure
# AWS Access Key ID:     <create under IAM → Users → Security credentials>
# AWS Secret Access Key: <same>
# Default region:        eu-north-1
# Default output format: json
```

Verify: `aws sts get-caller-identity` should return your IAM user ARN.

---

## 3. SES (email sending)

New AWS accounts start in **SES sandbox mode**. In sandbox:
- You can only send to verified email addresses
- Verify the recipient address: SES → Verified identities → Create identity → Email address
- Verification email arrives within a minute; click the link

For personal/home use, staying in sandbox and verifying one Gmail address is sufficient.

**Important:** SES emails are silently dropped by Proton Mail — they show 0 bounces
in SES metrics but never arrive. Use Gmail as the alert address.

To request production access (remove sandbox limits): SES → Account dashboard →
Request production access. Not needed for single-device personal use.

---

## 4. Budget alert + kill-switch

Protects against runaway costs. Two layers:

### 4a. Budget alert (email at threshold)

1. Billing → Budgets → Create budget → Cost budget
2. Amount: $2/month
3. Add alert: 80% actual → email your address

### 4b. Automated kill-switch (deny all at $2)

Automatically denies all API calls from the Lambda execution role if spending hits $2.

1. Create an IAM role for the budget action:
   ```
   Role name: aws-budgets-actions-role
   Trust policy: { "Service": "budgets.amazonaws.com" }
   Permissions: AWSBudgetsActionsWithAWSResourceControlAccess
   ```
2. In the budget: Add action → Apply IAM policy → `AWSDenyAll` → target role:
   `esp32-telemetry-lambda-role` (or whichever Lambda role to protect)
3. Threshold: 100% of $2 actual

**UI note (2026):** The action configuration is in the budget's Edit flow → Step 3,
not on a separate "Actions" tab.

---

## 5. Known account constraints

These are issues encountered on a new AWS account that may not affect established accounts:

### Lambda Function URLs return 403

Lambda Function URLs with `authorization_type=NONE` return HTTP 403 on some new accounts,
even with a correct resource policy and no SCPs. CloudWatch shows the request never
reaches the Lambda. **Workaround:** Use API Gateway HTTP API v2 instead
(`aws_apigatewayv2_api` + integration + route + stage + `aws_lambda_permission`).

### Lambda reserved concurrency requires ≥10 unreserved

New accounts cannot set `reserved_concurrent_executions` on any Lambda if total
account concurrency would drop below 10 unreserved. Remove the reserved concurrency
setting from Terraform if you hit this error.

### SES + Proton Mail: silent drop

SES emails are silently discarded by Proton Mail — no bounces, no delivery, nothing
in any folder. Use Gmail. See section 3 above.
