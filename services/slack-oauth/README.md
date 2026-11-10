# Slack OAuth HTTPS relay

PhosChat uses this service when the shared Slack app needs a public HTTPS redirect. The desktop starts Slack OAuth with S256 PKCE and checks the returned state. The browser returns to this service, which redirects only the one-time code and state to the fixed local PhosChat listener. The desktop exchanges that code directly with Slack. Refreshes for sessions opened with the HTTPS flow go through the relay, which authenticates to Slack on the server. The relay has no token database and returns tokens only in a response to the desktop's refresh request.

## Deployment

Build the image from the repository root.

```sh
docker build -t phoschat-slack-oauth services/slack-oauth
```

Run the container behind trusted HTTPS termination, for example as a public Cloud Run service. Gunicorn listens on all interfaces on `PORT` (8080 by default). Use `GET /healthz` for a health probe. The container listener is plain HTTP for private ingress from the HTTPS terminator.

- Set `SLACK_CLIENT_ID` to the public client ID of the Slack app.
- Inject `SLACK_CLIENT_SECRET` at runtime from the platform's secret store. For Cloud Run, grant the service account access to the corresponding Secret Manager secret. Do not supply the secret as an image build argument or installer setting.
- Configure Slack with `https://<your-service-host>/phoschat/slack/callback`, PKCE, token rotation and the required user scopes. Check a real authorization and refresh with a test account before distributing the app. See [Slack PKCE](https://docs.slack.dev/authentication/using-pkce/), [token rotation](https://docs.slack.dev/authentication/using-token-rotation/) and [distribution](https://docs.slack.dev/app-management/distribution/#enabling-ssl).
- Keep access logging disabled on Gunicorn. Configure your HTTPS ingress and logging sinks to omit OAuth query strings and refresh request or response bodies. On Cloud Run, exclude request logs for this service's callback if they include the query string. Apply ingress rate limiting to the refresh route and size service concurrency and instance count for the expected authentication volume.

The public refresh route accepts the configured app ID and a Slack refresh token. It accepts no caller-provided upstream URL, credential override or other grant type. The browser callback has a single fixed destination on the local machine.

## Local tests

Run from the repository root. No packages, Slack credentials or external requests are needed.

```sh
python3 -m unittest discover -s services/slack-oauth -v
```
