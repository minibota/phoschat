# Interface design

PhosChat opens a 328 × 620 window with a search field and an account accordion. Only one account list is open at a time; its conversations scroll independently of the other account headers. Dark and Light use slate and warm paper surfaces with the same phosphor identity. Appearance changes apply to all open windows and persist across restarts. The main window stays focused on the conversation list. Every conversation opens in its own native window.

## Everyday controls

The top card summarizes every configured account, with online, offline and connecting totals. Its tooltip lists individual account states; selecting a conversation does not change this overview.

The account headers stay outside the conversation list's scroll area. At very small window sizes or with more account headers than can physically fit, the header area also scrolls so every account remains reachable.

- Use **Accounts** to choose a popular IRC network or **Custom server**, set a nickname and connect. TLS starts enabled. An existing account retains its server, port and TLS settings. Add a separate account to change network identity.
- Use **+** to join a channel or open a direct message. Search matches conversation, network, protocol and custom group names.
- Double-click a conversation, or activate it with the keyboard, to open or focus its chat window. Each window shows the account connection state. **People** opens the channel roster when applicable.
- Use the conversation context menu to favorite, mute, group or leave a channel. The settings menu switches the list between account, protocol, custom group and recent activity. Favorites stay at the top.
- Enter sends a message from the composer. Right-click a message or select it and use the system copy shortcut to copy its text.

## State and ownership

The window manager keys windows and controllers by account and remote conversation identifiers. A chat window binds to a message model and draft held by its controller. Closing the window preserves that controller for the current run. Reopening a conversation focuses the existing window when one is already open. Messages from IRC are displayed as plain text. New chat controllers load the latest 200 messages from SQLite. The live timeline keeps up to 1,000 messages in memory while SQLite retains the account history.

The application remembers main and chat geometry, collapsed groups and the grouping mode. Open conversations can be restored on restart. Tray minimization and close-to-tray are separate settings and are available where the desktop provides a tray.

Grouped tabs and drag to detach remain future work. The same controller and message model can back a tab or an independent window. The interface uses Qt scaling for HiDPI displays.
