# Slack

PhosChat connects as a Slack user through Slack's official Web API. The integration is a workspace-scoped preview for approved Slack apps. It opens joined channels and direct or group messages, reads recent text in the selected conversation, and sends plain text.

## Set up browser authorization

1. Create or use an internal Slack app in the workspace you intend to connect. Confirm its installation, conversation access and local-client data handling with the workspace administrator.
2. In **OAuth & Permissions**, enable **PKCE**. Add the exact redirect URL `http://localhost:8765/phoschat/slack/callback`. The manifest below contains the user scopes.
3. Copy the app's **Client ID** from **Basic Information**. Open **Accounts** in PhosChat, select **Slack**, enter the Client ID and select **Save & connect** with the user token field blank.
4. Complete Slack's consent screen in your browser. Return to PhosChat once the loopback page acknowledges authorization. Keep port 8765 free and complete one Slack authorization at a time.

PKCE uses an S256 challenge, a fresh state value and an IPv4 loopback listener. The Client ID is public. PhosChat exchanges the authorization code and refreshes rotating tokens without an application secret. Slack's [PKCE documentation](https://docs.slack.dev/authentication/using-pkce/) describes this desktop flow.

Example Slack app manifest:

```yaml
display_information:
  name: PhosChat
oauth_config:
  redirect_urls:
    - http://localhost:8765/phoschat/slack/callback
  scopes:
    user:
      - channels:read
      - channels:history
      - groups:read
      - groups:history
      - im:read
      - im:history
      - mpim:read
      - mpim:history
      - chat:write
      - users:read
  pkce_enabled: true
settings:
  org_deploy_enabled: false
  socket_mode_enabled: false
  token_rotation_enabled: true
```

A workspace-approved non-rotating `xoxp-` user token with the same scopes can be entered directly instead. Keep the Client ID blank for a token-only account. Use browser authorization for rotating sessions. PhosChat verifies the workspace and user through [auth.test](https://docs.slack.dev/reference/methods/auth.test/) before listing conversations.

## What to expect

- **Joined conversations**. The provider lists joined channels and accessible direct and group messages. The `users:read` scope resolves names and may be omitted for a manual token if IDs are sufficient.
- **On-demand text history**. Opening a Slack conversation loads up to 15 recent entries and polls that conversation while the account is connected. The app schedules at most one history request per workspace every 61 seconds, including follow-up pages, and observes Slack's `Retry-After` header. The first snapshot is quiet. Incremental pages have a bounded catch-up window, so a busy channel may have gaps. See [conversations.history](https://docs.slack.dev/reference/methods/conversations.history/) and [Slack Web API rate limits](https://docs.slack.dev/apis/web-api/rate-limits/).
- **Plain-text sends**. PhosChat uses `chat.postMessage` as the authorized user. It shows the outgoing message after Slack confirms it. When a send is uncertain, check the conversation before retrying. See [chat.postMessage](https://docs.slack.dev/reference/methods/chat.postMessage/).
- **Local handling**. Slack text is kept in a bounded session cache. Choose **Remember in system keychain** to keep the authorization between runs when a supported desktop keychain is available. Keep tokens out of repository files, logs and messages. For a revoked authorization, reconnect with a new approved grant.

Channels and private conversations depend on workspace membership and installed scopes. Threads, attachments, edits, reactions, Slack Connect edge cases, enterprise-wide installation and real-time Events API delivery are outside this preview.

Slack publishes additional obligations for commercial distribution and cross-organization use, including organization authorization and minimum retention of API data. Review these before offering an integration beyond its approved workspace. See the [Slack API Terms](https://slack.com/terms-of-service/api) and the [non-Marketplace rate-limit guidance](https://docs.slack.dev/changelog/2025/05/29/rate-limit-changes-for-non-marketplace-apps/).

## Verification

The Slack test suite uses a loopback HTTP fixture for OAuth callback validation, token refresh, conversation pagination, text sending and receiving, bot rejection, redirect refusal and rate-limit handling. A live workspace smoke test requires an approved Slack app and account.
