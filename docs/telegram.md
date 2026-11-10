# Telegram preview

PhosChat uses the Telegram API through the official Telegram Database Library (TDLib). The preview supports sign-in to an existing account, the main chat list, cloud direct and group text messages, and recent history on demand. Use separate PhosChat accounts for separate Telegram sessions.

## Set up an account

1. Install a TDLib JSON shared library at version 1.8.38 or newer. On Debian 13, install `libtdjson1.8.38`. An official Windows build can place `tdjson.dll` beside `phoschat.exe`. Linux packages can provide `libtdjson.so` in the application's library directory.
2. Obtain an API ID and API hash from **API development tools** at [my.telegram.org](https://my.telegram.org). Treat the hash as a credential. [Telegram application registration](https://core.telegram.org/api/obtaining_api_id)
3. In **Accounts**, add a Telegram account, enter the API ID and hash, then connect. The sign-in dialog follows the steps requested by Telegram, which can include a phone number, a code, two-step verification, and email verification. Use an official Telegram client for registration and device-confirmation flows outside this preview.
4. Open a discovered direct or group chat to request its recent text messages. The client updates the conversation as new messages arrive.

## Session and message data

A session-only account uses a private temporary TDLib profile. Remembering the API hash in the system keychain enables reuse of an encrypted TDLib profile across launches. PhosChat derives its profile key from the API hash and local account identity. Use **Remove account** to clear the local profile and stored credentials. For server-side revocation, open **Settings → Devices** in an official Telegram client.

TDLib's message database is disabled, and Telegram message bodies shown by PhosChat stay in memory. Recent history is fetched from Telegram when needed. Sign-in codes, two-step verification passwords, and email addresses entered during sign-in are passed to TDLib and omitted from PhosChat settings and logs. Conversation identifiers are scoped to the authenticated Telegram user and the local account.

## Scope and verification

Broadcast channels require official sponsored-message support. The preview also omits secret chats, protected or self-deleting conversations, attachments, and account registration. [Telegram API terms](https://core.telegram.org/api/terms)

The adapter tests replay TDLib JSON updates for sign-in, routing, text history, sends, and deletion. With the official library installed, the native test starts and closes two TDLib clients before supplying initialization parameters. Signing in and exchanging messages with a live Telegram account is a separate verification step. [TDLib JSON API](https://core.telegram.org/tdlib/docs/td__json__client_8h.html) · [TDLib authorization and chat flow](https://core.telegram.org/tdlib/getting-started)
