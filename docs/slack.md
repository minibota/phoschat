# Connect Slack

Users do **not** need to create their own Slack app when PhosChat is built with the Client ID of an app approved in their workspace. The app owner configures the app and each user authorizes their own account.

## For users

1. Open **Accounts**, select **Slack**, then **Connect to Slack**.
2. Choose your workspace in the browser and approve access. Your workspace may require administrator approval.
3. Return to PhosChat. Enable **Remember in the system credential store** to keep the session when a supported keychain is available.

No Client ID, client secret or copied token is needed in this normal flow. Keep port 8765 free while connecting; complete one Slack authorization at a time.

If the window says **Slack sign-in is not configured in this build**, the distributor has not supplied the shared application ID. Ask for a configured build, or use **Use a workspace app or token (advanced)** if your administrator has supplied one. Installing PhosChat does not override workspace installation policies.

## One-time setup for the development workspace

This section is for whoever distributes the build or manages the workspace integration, not every user.

1. Create a Slack app at [Your Apps](https://api.slack.com/apps), using the manifest below or equivalent settings.
2. In **OAuth & Permissions**, enable **PKCE** and register exactly `http://localhost:8765/phoschat/slack/callback`. Slack treats this as a desktop callback for a PKCE-enabled app. Enabling PKCE marks the app as a public client and is a one-way setting; see [Slack's PKCE guide](https://docs.slack.dev/authentication/using-pkce/).
3. Install or approve the app in the Slack workspace where you created it. Workspace administrators may restrict app installation.
4. Copy **Basic Information → Client ID**. In GitHub, add the repository **Actions variable** `SLACK_CLIENT_ID` with that public numeric value, for example `12345.67890`. An existing repository secret with the same name also works. The release workflow includes this public ID in DEB, RPM, Windows and Flatpak builds. Do not add a Slack client secret to PhosChat or its installers.
5. Build and verify browser authorization in that workspace.

For a local build, use:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPHOSCHAT_SLACK_CLIENT_ID=12345.67890
cmake --build build
```

`PHOSCHAT_SLACK_CLIENT_ID` can also be supplied through the configure-time environment. It is optional and public. Without it, source builds retain advanced setup. Existing saved accounts keep their original application ID when the configured ID changes.

The native OAuth flow already uses a fresh state value, S256 PKCE, a loopback callback and rotating user tokens. Authorization and refresh do not require an embedded application secret. Tokens are individual user credentials; the public Client ID is shared app configuration.

## Public distribution

Slack's [public distribution checklist](https://docs.slack.dev/app-management/distribution/#enabling-ssl) requires HTTPS OAuth redirect URLs. Use the [PhosChat OAuth relay](../services/slack-oauth/README.md) for a shared app that users can authorize from other workspaces.

1. Deploy the relay behind HTTPS with a trusted certificate. Configure `SLACK_CLIENT_ID` and inject `SLACK_CLIENT_SECRET` from the hosting platform's secret store.
2. In Slack **OAuth & Permissions**, enable PKCE and token rotation, then register `https://<your-service-host>/phoschat/slack/callback`. Apply the user scopes from the manifest below.
3. Set the GitHub Actions repository variables `SLACK_CLIENT_ID` and `SLACK_OAUTH_ORIGIN`. Use only the HTTPS origin for the latter, for example `https://oauth.example.test`. A source build can supply the same origin through `-DPHOSCHAT_SLACK_OAUTH_ORIGIN=...`.
4. Build PhosChat with those settings and test a real authorization and token refresh with an approved account. Once the HTTPS flow is verified, remove the HTTP loopback redirect from the app being distributed and enable **Public Distribution**. A separate app can retain the loopback setup for development.

The relay returns the one-time OAuth code and state to the fixed local listener. PhosChat validates state and exchanges the code and PKCE verifier directly with Slack. Refreshes for HTTPS grants go through the relay using the server-side client secret. Sessions created with the loopback flow continue to refresh directly with Slack while their grants are valid. If the configured HTTPS origin changes, reconnect that account to authorize it for the new origin.

### App manifest

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
      - files:write
    user_optional:
      - files:write
  pkce_enabled: true
settings:
  org_deploy_enabled: false
  socket_mode_enabled: false
  token_rotation_enabled: true
```

These are **user scopes**, not bot scopes. `files:write` appears in both `user` and `user_optional`, as Slack requires for an optional permission. Users and workspace administrators can decline file access and still connect for text messaging. Add that optional scope to an older custom app before creating a new authorization. Existing text-only sessions keep working; file sending needs a grant that includes it. See [Slack's optional scopes](https://docs.slack.dev/authentication/installing-with-oauth/#optional-scopes).

## Advanced workspace app or token

Select **Use a workspace app or token (advanced)**. For an administrator-provided PKCE app, enter its public **Client ID**, leave **User token** blank and connect in the browser. For an approved non-rotating `xoxp-` user token, leave Client ID blank and enter the token instead. Never paste a Slack client secret or a bot token into these fields.

A saved account's application ID cannot be silently switched. Add a new account to use a different Slack app. Browser sign-in is the supported path for rotating sessions. PhosChat checks the authorized workspace and user through [auth.test](https://docs.slack.dev/reference/methods/auth.test/).

## Current scope and limits

- Joined channels, direct messages and group messages depend on membership and permissions.
- Opening a conversation retrieves recent text and polls that conversation. Non-Marketplace API limits mean updates can take a minute or longer; this is not Slack's real-time client. The bounded catch-up window can leave gaps in busy channels. See [history limits](https://docs.slack.dev/reference/methods/conversations.history/) and [API rate limits](https://docs.slack.dev/apis/web-api/rate-limits/).
- Text uses `chat.postMessage`. Images, audio, video and documents use Slack's official external-upload flow with optional `files:write`. PhosChat streams one file per account at a time, up to 512 MiB; workspace restrictions may be tighter. Sent files appear by filename; media playback and downloads are not implemented. A confirmed send does not imply that another person has read it.
- Profile information is limited to what Slack returns under the granted permissions. No extra email permission is requested by the standard app.
- Messages are held in a bounded session cache; authorization can be stored in the OS keychain. Reauthorize if Slack revokes or expires the grant.

Threads, edits, reactions, enterprise-wide installation and real-time Events API delivery remain outside this preview. Review [Slack API terms](https://slack.com/terms-of-service/api) and [distribution guidance](https://docs.slack.dev/app-management/distribution/) for the integration's intended deployment.

## Verification

Automated tests use local HTTP/provider fixtures for both authorization flows, token refresh, permissions, conversations and file transfers. UI tests cover configured and unconfigured builds, advanced setup and preservation of existing account identities. The relay tests exercise the browser redirect and refresh endpoint with synthetic inputs. A live smoke test requires a deployed HTTPS origin and an approved Slack app and user.
