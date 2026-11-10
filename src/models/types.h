#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFlags>
#include <QHashFunctions>
#include <QList>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QStringList>

namespace phoschat {

enum class Protocol { Irc = 0, Telegram = 1, Matrix = 2, Slack = 3 };
enum class ConnectionState { Disconnected, Connecting, Authenticating, Connected, Reconnecting, Error };
enum class Presence { Unknown, Online, Away, Offline };
enum class ConversationKind { Channel = 0, Direct = 1, Group = 2 };
enum class AuthenticationKind { Phone, Code, Password, EmailAddress, EmailCode, Registration, Unsupported };
enum class HistoryState { Unavailable, Ready, Loading, Complete };
enum class DeliveryState { None, Pending, Sent, Read, Failed };
using ProfileDetails = QList<QPair<QString, QString>>;
enum class Capability : unsigned {
    Text = 1u << 0,
    DirectMessages = 1u << 1,
    Groups = 1u << 2,
    Channels = 1u << 3,
    Presence = 1u << 4,
    Files = 1u << 5,
    Reactions = 1u << 6,
    Edit = 1u << 7,
    Delete = 1u << 8,
    Threads = 1u << 9,
    RemoteHistory = 1u << 10,
    Typing = 1u << 11,
    ReadReceipts = 1u << 12,
    E2ee = 1u << 13,
    PaginatedHistory = 1u << 14
};
Q_DECLARE_FLAGS(Capabilities, Capability)

struct Account {
    QString id;
    QString label;
    Protocol protocol { Protocol::Irc };
    QString host;
    quint16 port { 6697 };
    bool tls { true };
    QString nick;
    QString saslUser;
    bool saveSecret { false };
    bool autoConnect { false };
    QString providerId;
};

struct ConversationId {
    QString accountId;
    QString remoteId;

    bool operator==(const ConversationId &) const = default;
    bool isValid() const { return !accountId.isEmpty() && !remoteId.isEmpty(); }
};

inline size_t qHash(const ConversationId &id, size_t seed = 0) noexcept
{
    return qHashMulti(seed, id.accountId, id.remoteId);
}

struct Conversation {
    ConversationId id;
    QString title;
    ConversationKind kind { ConversationKind::Channel };
    bool joined {};
    bool favorite {};
    bool muted {};
    bool autoJoin {};
    QString group;
    int unread {};
    QDateTime updated;
    QByteArray avatar;
    Presence presence { Presence::Unknown };
};

struct Message {
    ConversationId conversation;
    QString id;
    QString sender;
    QString text;
    QDateTime timestamp;
    bool outgoing {};
    bool mention {};
    bool historical {};
    qint64 sequence {};
    DeliveryState delivery { DeliveryState::None };
};

inline bool messageBefore(const Message &first, const Message &second)
{
    if (first.timestamp != second.timestamp)
        return first.timestamp < second.timestamp;
    return first.sequence < second.sequence;
}

} // namespace phoschat

Q_DECLARE_OPERATORS_FOR_FLAGS(phoschat::Capabilities)
Q_DECLARE_METATYPE(phoschat::ConnectionState)
Q_DECLARE_METATYPE(phoschat::Presence)
Q_DECLARE_METATYPE(phoschat::AuthenticationKind)
Q_DECLARE_METATYPE(phoschat::ConversationId)
Q_DECLARE_METATYPE(phoschat::Conversation)
Q_DECLARE_METATYPE(phoschat::Message)
