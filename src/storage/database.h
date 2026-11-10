#pragma once

#include "models/types.h"

#include <QSqlDatabase>

class QSqlError;

namespace phoschat {

class Database final {
public:
    enum class AppendResult { Inserted, Duplicate, Error };

    explicit Database(QString path);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open();
    [[nodiscard]] QString lastError() const;

    [[nodiscard]] QList<Account> accounts();
    bool saveAccount(const Account &account);
    bool removeAccount(const QString &id);

    [[nodiscard]] QList<Conversation> conversations();
    bool saveConversation(const Conversation &conversation);

    [[nodiscard]] QList<Message> messages(const ConversationId &conversation, int limit = 200);
    AppendResult appendMessage(const Message &message, const Conversation &updatedConversation);

private:
    void close();
    bool ready();
    bool initializeSchema();
    bool run(const QString &statement);
    bool writeConversation(const Conversation &conversation);
    AppendResult insertMessage(const Message &message);
    bool finishTransaction(bool commit);
    bool fail(const QString &operation, const QSqlError &error);

    QString m_path;
    QString m_connectionName;
    QSqlDatabase m_connection;
    QString m_lastError;
};

} // namespace phoschat
