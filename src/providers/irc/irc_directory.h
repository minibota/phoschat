#pragma once

#include <QString>
#include <QStringView>

#include <initializer_list>
#include <span>

namespace phoschat::irc {

struct ChannelListing {
    QString name;
    QString topic;
    int users {};
};

enum class ChannelListStatus { NotRequested, Loading, Ready, Truncated, TimedOut, Rejected, Interrupted };

struct NetworkSuggestion {
    QStringView name;
    QStringView host;
    quint16 port;
};

struct ChannelSuggestion {
    QStringView networkHost;
    QStringView name;
    QStringView category;
    QStringView description;
    QStringView source;
};

std::span<const NetworkSuggestion> recommendedNetworks();
std::span<const ChannelSuggestion> recommendedChannels();
bool matchesDirectorySearch(QStringView query, std::initializer_list<QStringView> fields);

} // namespace phoschat::irc
