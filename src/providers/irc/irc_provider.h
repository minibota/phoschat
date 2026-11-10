#pragma once

#include "core/chat_provider.h"
#include "providers/irc/irc_directory.h"
#include "providers/irc/irc_message.h"

#include <QElapsedTimer>
#include <QSet>
#include <QSslConfiguration>
#include <QTimer>

class QSslSocket;

namespace phoschat {

class IrcProvider final : public IChatProvider {
    Q_OBJECT

public:
    explicit IrcProvider(Account account, QObject *parent = nullptr);
    IrcProvider(Account account, QSslConfiguration tlsConfiguration, QObject *parent = nullptr);

    [[nodiscard]] Capabilities capabilities() const override;
    void connectAccount(const QByteArray &secret) override;
    void disconnectAccount() override;
    bool join(const QString &target) override;
    bool part(const QString &target) override;
    bool sendText(const ConversationId &conversation, const QString &text) override;
    bool sendCommand(const ConversationId &conversation, const QString &command) override;
    bool requestProfile(const ConversationId &id) override;
    void setConversationOpen(const ConversationId &id, bool open) override;
    [[nodiscard]] QString canonicalTarget(const QString &target) const override;
    [[nodiscard]] QString requestChannelList();
    [[nodiscard]] irc::ChannelListStatus channelListStatus() const { return channelListStatus_; }
    [[nodiscard]] const QList<irc::ChannelListing> &channelList() const { return channelList_; }
    [[nodiscard]] bool channelListBusy() const { return awaitingChannelListEnd_; }

signals:
    void channelListChanged();

private:
    struct Channel {
        QString name;
        QHash<QString, QString> members;
        QHash<QString, QString> names;
        bool readingNames = false;
    };

    struct WhoisRequest {
        ConversationId id;
        ProfileDetails details;
        quint64 serial;
        bool expired = false;
    };

    void setState(ConnectionState state);
    void closeTransport();
    void fail(const QString &message, bool retryable);
    void startRegistration();
    void finishRegistration();
    void clearSecret();
    void checkKeepalive();
    void queryPresence();
    void clearPresence();

    [[nodiscard]] bool writeLine(const QByteArray &line);
    [[nodiscard]] bool canUseTarget(const QString &target) const;
    [[nodiscard]] bool isChannel(const QString &target) const;
    [[nodiscard]] bool isSelf(const QString &nick) const;
    [[nodiscard]] bool isMention(const QString &text) const;
    [[nodiscard]] Conversation makeConversation(const QString &target, bool joined) const;
    bool sendPrivmsg(const QString &target, const QString &text, bool action);

    void dispatch(const irc::IrcMessage &message);
    void handleCapability(const QList<QByteArray> &parameters);
    void requestCapabilities(bool includeSasl);
    void endCapabilityNegotiation();
    void handleAuthenticate(const QList<QByteArray> &parameters);
    void handleSupport(const QList<QByteArray> &parameters);
    void handlePrivmsg(const irc::IrcMessage &message);
    void handleMembership(const irc::IrcMessage &message);
    void handleNames(const irc::IrcMessage &message);
    void handleChannelList(const irc::IrcMessage &message);
    bool handleWhois(const irc::IrcMessage &message, int numeric);
    bool handleJoinFailure(const irc::IrcMessage &message, int numeric);
    void handleIson(const QList<QByteArray> &parameters);
    void finishChannelList(irc::ChannelListStatus status);
    void publishMembers(const QString &key);
    void removeChannel(const QString &channel);
    void updateMember(Channel &channel, const QString &oldNick, const QString &newNick);

    const Account account_;
    const QSslConfiguration tlsConfiguration_;
    QSslSocket *socket_ = nullptr;
    QTimer registrationTimer_;
    QTimer keepaliveTimer_;
    QTimer presenceTimer_;
    QTimer channelListTimeout_;
    QTimer channelListUpdates_;
    QElapsedTimer lastChannelListRequest_;
    QList<irc::ChannelListing> channelList_;
    QSet<QString> channelListNames_;
    irc::ChannelListStatus channelListStatus_ = irc::ChannelListStatus::NotRequested;
    bool awaitingChannelListEnd_ = false;
    bool channelListTruncated_ = false;
    QElapsedTimer lastReceived_;
    QElapsedTimer pingSent_;
    QByteArray pingToken_;
    irc::FrameDecoder decoder_;
    ConnectionState state_ = ConnectionState::Disconnected;

    QString nick_;
    QString channelTypes_;
    QString prefixSymbols_;
    QString statusPrefixes_;
    irc::CaseMapping caseMapping_ = irc::CaseMapping::Rfc1459;
    qsizetype ownSourceBytes_ = 0;
    QHash<QString, Channel> channels_;
    QHash<QString, WhoisRequest> whoisRequests_;
    quint64 whoisSerial_ = 0;
    QHash<QString, Presence> presencePeers_;
    QStringList presencePending_;
    qsizetype nextPresencePeer_ = 0;
    bool presenceUnavailable_ = false;

    QByteArray secret_;
    QHash<QByteArray, QByteArray> offeredCapabilities_;
    QSet<QByteArray> pendingCapabilities_;
    QSet<QByteArray> enabledCapabilities_;
    QHash<QByteArray, bool> capabilityReply_;
    bool welcomeReceived_ = false;
    bool negotiationEnded_ = false;
    bool authenticating_ = false;
    bool authenticated_ = false;
};

} // namespace phoschat
