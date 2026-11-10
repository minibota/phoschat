## PhosChat 0.2.2 preview

- Browse earlier Telegram messages with **Load older messages**, including chats whose first history response contains only one message. Loading older pages keeps your reading position; restored windows retry after connecting.
- Show Telegram contact and group photos, clickable profile headers, media descriptions and captions. Available profile details can also be opened for Slack contacts and IRC users.
- Send files, images, audio and video in supported Telegram and Slack conversations through **Attach…**. Delivery and read indicators reflect the protocol's actual confirmations; Slack and IRC do not invent read receipts.
- Keep unread conversations first within their account and show protocol-provided contact presence in direct-chat headers. Private or unavailable presence remains unknown.
- Receive native Linux notifications, including desktops without a visible tray. Notification actions open the correct conversation; notification settings remain persistent.
- Discover more IRC networks and topics, with clearer feedback when joining a channel succeeds or fails.
- Use simplified Slack browser sign-in in configured builds, while preserving advanced workspace apps and existing account identities. Optional file permissions can be declined without preventing text messaging.

### Preview boundaries

Shared Slack browser sign-in depends on a configured Slack application and an
operational HTTPS authorization service. The public build settings alone do not
deploy that service or establish workspace approval. If it is unavailable, use
the documented advanced workspace setup. Live end-to-end authorization remains
a separate verification step. See the
[Slack guide](https://github.com/minibota/phoschat/blob/v0.2.2/docs/slack.md).

Telegram broadcast channels, secret chats, protected/auto-deleting message
content, media playback and downloads remain outside this preview. Telegram
history is fetched on demand and retained for the connected session. Slack
history remains rate-limited, and Slack threads are not implemented. Adapter and
UI tests use local fixtures; they do not replace live-account verification.
See the [Telegram guide](https://github.com/minibota/phoschat/blob/v0.2.2/docs/telegram.md).

Download the x86-64 installer for Windows, Fedora 45, Debian 13 or Flatpak.
Check downloaded files against `SHA256SUMS`. Windows and RPM packages are
unsigned. macOS remains a source-build preview without an installer.
See the [packaging guide](https://github.com/minibota/phoschat/blob/v0.2.2/docs/packaging.md).
