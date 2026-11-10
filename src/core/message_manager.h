#pragma once

#include "models/types.h"

#include <QHash>
#include <QObject>

namespace phoschat {

class AccountManager;
class ConversationManager;
class Database;

class MessageManager final : public QObject {
    Q_OBJECT

public:
    MessageManager(AccountManager &accounts, ConversationManager &conversations, Database &database,
        QObject *parent = nullptr);
    QList<Message> history(const ConversationId &id, int limit = 200);
    bool canSend(const ConversationId &id) const;
    bool send(const ConversationId &id, const QString &text);
    void requestHistory(const ConversationId &id);

signals:
    void messageAdded(const phoschat::Message &message);
    void messageRemoved(const phoschat::ConversationId &id, const QString &messageId);
    void sessionHistoryCleared(const QString &accountId);
    void errorOccurred(const QString &message);

private:
    void receive(Message message);
    void removeMessage(const ConversationId &id, const QString &messageId);

    AccountManager &accounts_;
    ConversationManager &conversations_;
    Database &database_;
    QHash<ConversationId, QList<Message>> sessionHistory_;
};

} // namespace phoschat
