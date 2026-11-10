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
    QList<Message> history(const ConversationId &id, int limit = -1);
    bool canSend(const ConversationId &id) const;
    bool canSendFiles(const ConversationId &id) const;
    bool send(const ConversationId &id, const QString &text);
    bool sendFile(const ConversationId &id, const QString &path);
    void requestHistory(const ConversationId &id);
    bool hasPagedHistory(const ConversationId &id) const;
    HistoryState historyState(const ConversationId &id) const;
    void requestOlderHistory(const ConversationId &id);
    void setConversationOpen(const ConversationId &id, bool open);

signals:
    void messageAdded(const phoschat::Message &message);
    void messageRemoved(const phoschat::ConversationId &id, const QString &messageId);
    void historyStateChanged(const phoschat::ConversationId &id);
    void historyCleared(const phoschat::ConversationId &id);
    void sessionHistoryCleared(const QString &accountId);
    void errorOccurred(const QString &message);

private:
    void receive(Message message);
    void removeMessage(const ConversationId &id, const QString &messageId);
    void markReadThrough(const ConversationId &id, qint64 sequence);

    AccountManager &accounts_;
    ConversationManager &conversations_;
    Database &database_;
    QHash<ConversationId, QList<Message>> sessionHistory_;
};

} // namespace phoschat
