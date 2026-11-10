#pragma once

#include "models/types.h"

#include <QHash>
#include <QObject>

#include <optional>

namespace phoschat {

class AccountManager;
class Database;
class MessageManager;

class ConversationManager final : public QObject {
    Q_OBJECT

public:
    ConversationManager(AccountManager &accounts, Database &database, QObject *parent = nullptr);
    QList<Conversation> conversations() const;
    std::optional<Conversation> conversation(const ConversationId &id) const;
    bool joinChannel(const QString &accountId, const QString &target);
    ConversationId openDirect(const QString &accountId, const QString &target);
    bool partChannel(const ConversationId &id);
    void setFavorite(const ConversationId &id, bool enabled);
    void setMuted(const ConversationId &id, bool enabled);
    void setGroup(const ConversationId &id, const QString &group);
    void markRead(const ConversationId &id);
    QStringList members(const ConversationId &id) const;

signals:
    void conversationsChanged();
    void channelJoinStarted(const QString &channel);
    void channelJoined(const phoschat::ConversationId &id);
    void membersChanged(const phoschat::ConversationId &id);
    void errorOccurred(const QString &message);

private:
    friend class MessageManager;
    void acceptRemoteConversation(const Conversation &remote);
    bool save(const Conversation &value);
    void dropAccount(const QString &accountId);
    void clearMembership(const QString &accountId);
    void rejoin(const QString &accountId);

    AccountManager &accounts_;
    Database &database_;
    QHash<ConversationId, Conversation> conversations_;
    QHash<ConversationId, QStringList> members_;
    QHash<ConversationId, quint64> pendingJoins_;
    quint64 nextJoinAttempt_ {};
};

} // namespace phoschat
