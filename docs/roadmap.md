# PhosChat roadmap

## v0.2 · Desktop preview

- Keep the compact list and independent chat windows as the core interaction. Offer dark and light phosphor themes with the same branding.
- Browse IRC channels live with server `LIST` and curated suggestions for different topics. Verify public-network behavior in addition to the reproducible loopback fixtures.
- Evaluate Telegram cloud text chats and approved Slack workspaces using the preview adapters. Live account verification, long-running sessions and reconnect behavior are gates for broader availability.
- Build Debian, Fedora, Flatpak and Windows installers through the release workflow. Keep macOS compilation and preview checks in CI while trusted IRC TLS and signed packaging remain future work.

## Next · Conversation workflow and provider reliability

- Add grouped tab hosts and detach/reintegrate tabs without losing a chat's controller, draft or unread state. Restore tab order and account for display changes.
- Test Telegram verification, session recovery and account removal with authorized test accounts. Evaluate channel support only with the sponsored-message requirements met. Treat media and secret chats as separate capabilities.
- Test Slack permissions, token rotation, revocation and rate limits in an approved workspace. Consider thread support and a compatible delivery architecture before expanding distribution.
- Add an optional original notification sound and verify desktop notification delivery. Evaluate IRC remote history with a compatible server or bouncer.
- Verify accessibility, HiDPI, multi-monitor restoration, startup time and memory with realistic accounts.

## Last provider · Matrix

- Evaluate a maintained Matrix library for homeserver discovery, authentication, room sync and history. Compare libQuotient's maintained crypto path with a Matrix Rust SDK bridge.
- Verify encrypted-room interoperability, device verification, key backup and missing-key recovery before enabling E2EE.
- Define per-room local caching and data removal behavior before release.

See [protocol research](protocols.md) for upstream requirements and [packaging](packaging.md) for platform verification.
