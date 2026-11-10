# IRC channel discovery

Choose **+ → Discover IRC channels** to explore curated channels or request the live directory from one of your IRC accounts.

- **Suggestions** works offline. Search by channel, topic, network or language. Each channel links to its project's guide. **Add network** prefills its TLS endpoint, then you choose a nickname and whether to connect.
- **Live directory** requests the network's public IRC channel list. Search names and topics, sort by channel or user count, and set a minimum user count. Select a result to join it on the chosen account.

Servers decide which channels to advertise. PhosChat retains up to 20,000 results per connected account and shows partial results when a request reaches that bound or times out. Results are refreshed only when requested. [IRC LIST specification](https://modern.ircdocs.horse/#list-message) · [Libera.Chat discovery guide](https://libera.chat/guides/findingchannels)

The [loopback demo server](../tests/demo_irc_server.py) advertises three sample channels for trying live search without a public network account.

## Included networks

| Network | TLS endpoint | Examples |
| --- | --- | --- |
| [Libera.Chat](https://libera.chat/guides/connect) | `irc.libera.chat:6697` | Python in English, Spanish and French, Git, KDE development, music and book metadata, English Wikipedia |
| [OFTC](https://www.oftc.net/) | `irc.oftc.net:6697` | Debian support and accessibility, OpenStreetMap, Tor |
| [hackint](https://www.hackint.org/servers) | `irc.hackint.org:6697` | Hacker community and network support |

The suggestions use official project and network references. Channel rules, registration requirements and activity can change. [Python](https://www.python.org/community/irc/) · [Git](https://git-scm.com/community) · [KDE Okular](https://okular.kde.org/contact/) · [MusicBrainz and BookBrainz](https://musicbrainz.org/doc/Communication/ChatBrainz) · [Wikimedia](https://meta.wikimedia.org/wiki/IRC/en) · [Debian](https://wiki.debian.org/IRC) · [OpenStreetMap](https://wiki.openstreetmap.org/wiki/IRC) · [Tor Project](https://lists.torproject.org/mailman3/postorius/lists/tor-community-team.lists.torproject.org/) · [hackint](https://hackint.org/support)
