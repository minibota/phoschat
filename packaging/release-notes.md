## PhosChat 0.2.1 preview

- Add IRC accounts by choosing a popular network or a custom server. Suggested networks use TLS and keep server details visible.
- Switch accounts through an accordion with fixed headers. A long Telegram contact list scrolls without hiding IRC or Slack accounts.
- See all-account online, offline and connecting totals above the conversation list.
- Sign in to Telegram with your phone number and verification code. Official installers include PhosChat's application identity; creating your own API application is optional advanced setup.
- Protect each Telegram profile with an independent random key in the system keychain, with recovery-safe migration for existing sessions.
- Discover main and archived Telegram groups and restore sending after reconnection.
- Keep search and clear icons centered, without a rounded border around the focused contact list. Both phosphor themes retain the same behavior.

Telegram and Slack remain preview integrations. Telegram broadcast channels,
secret chats, protected/auto-deleting message content and media remain outside
this preview. Real-account sign-in and session migration still require live
verification; the release exercises protocol adapters and native TDLib startup.
See the [Telegram guide](https://github.com/minibota/phoschat/blob/v0.2.1/docs/telegram.md).

Download the x86-64 installer for Windows, Fedora 45, Debian 13 or Flatpak.
Check downloaded files against `SHA256SUMS`. Windows and RPM packages are
unsigned. macOS remains a source-build preview without an installer.
See the [packaging guide](https://github.com/minibota/phoschat/blob/v0.2.1/docs/packaging.md).
