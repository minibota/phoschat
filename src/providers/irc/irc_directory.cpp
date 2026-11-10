#include "providers/irc/irc_directory.h"

#include <algorithm>
#include <array>

namespace phoschat::irc {
namespace {

    constexpr QStringView Libera = u"irc.libera.chat";
    constexpr QStringView Oftc = u"irc.oftc.net";
    constexpr QStringView Hackint = u"irc.hackint.org";
    constexpr QStringView Python = u"https://www.python.org/community/irc/";
    constexpr QStringView Brainz = u"https://musicbrainz.org/doc/Communication/ChatBrainz";
    constexpr QStringView Debian = u"https://wiki.debian.org/IRC";

    constexpr std::array Networks {
        NetworkSuggestion { u"Libera.Chat", Libera, 6697 },
        NetworkSuggestion { u"OFTC", Oftc, 6697 },
        NetworkSuggestion { u"hackint", Hackint, 6697 },
    };

    constexpr std::array Channels {
        ChannelSuggestion { Libera, u"#python", u"Programming", u"Python questions and community", Python },
        ChannelSuggestion { Libera, u"#pyar", u"Programming · Español",
            u"Python en español con la comunidad argentina", Python },
        ChannelSuggestion {
            Libera, u"#python-fr", u"Programming · Français", u"La communauté Python francophone", Python },
        ChannelSuggestion { Libera, u"#git", u"Programming", u"Git usage and version control",
            u"https://git-scm.com/community" },
        ChannelSuggestion { Oftc, u"#debian", u"Linux", u"Debian support and troubleshooting", Debian },
        ChannelSuggestion {
            Oftc, u"#debian-a11y", u"Accessibility · Linux", u"Debian accessibility development", Debian },
        ChannelSuggestion { Libera, u"#kde-devel", u"Linux · Desktop", u"KDE application development",
            u"https://okular.kde.org/contact/" },
        ChannelSuggestion { Libera, u"#musicbrainz", u"Music · Open data",
            u"Music metadata and the MusicBrainz community", Brainz },
        ChannelSuggestion { Libera, u"#bookbrainz", u"Books · Open data",
            u"Book metadata and the BookBrainz community", Brainz },
        ChannelSuggestion { Oftc, u"#osm", u"Maps · Open data", u"Community mapping and OpenStreetMap",
            u"https://wiki.openstreetmap.org/wiki/IRC" },
        ChannelSuggestion { Libera, u"#wikipedia-en", u"Knowledge · Community",
            u"English Wikipedia community", u"https://meta.wikimedia.org/wiki/IRC/en" },
        ChannelSuggestion { Oftc, u"#tor", u"Privacy", u"Tor usage and community feedback",
            u"https://lists.torproject.org/mailman3/postorius/lists/"
            u"tor-community-team.lists.torproject.org/" },
        ChannelSuggestion { Hackint, u"#hackint", u"Hacker community · IRC",
            u"Questions about the hackint network", u"https://hackint.org/support" },
        ChannelSuggestion {
            Libera, u"#libera", u"IRC", u"Libera.Chat network support", u"https://libera.chat/guides/faq" },
    };

} // namespace

std::span<const NetworkSuggestion> recommendedNetworks() { return Networks; }

std::span<const ChannelSuggestion> recommendedChannels() { return Channels; }

bool matchesDirectorySearch(QStringView query, std::initializer_list<QStringView> fields)
{
    const auto space = [](QChar character) { return character.isSpace(); };
    while (!query.isEmpty()) {
        const auto first = std::ranges::find_if_not(query, space);
        if (first == query.end()) {
            break;
        }
        query = query.sliced(first - query.begin());
        const auto end = std::ranges::find_if(query, space);
        const auto term = query.first(end - query.begin());
        if (!std::ranges::any_of(
                fields, [term](QStringView field) { return field.contains(term, Qt::CaseInsensitive); })) {
            return false;
        }
        query = query.sliced(term.size());
    }
    return true;
}

} // namespace phoschat::irc
