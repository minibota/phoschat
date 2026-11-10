#pragma once

#include "core/chat_provider.h"

#include <QHash>

#include <functional>

namespace phoschat {

class CredentialStore;
class Database;

class AccountManager final : public QObject {
    Q_OBJECT

public:
    using ProviderFactory = std::function<IChatProvider *(const Account &, QObject *)>;

    AccountManager(Database &database, CredentialStore &credentials, QObject *parent = nullptr,
        ProviderFactory factory = {});
    QList<Account> accounts() const;
    bool contains(const QString &id) const;
    Protocol protocol(const QString &id) const;
    bool protocolAvailable(Protocol protocol) const;
    ConnectionState state(const QString &id) const;
    QString statusDetail(const QString &id) const;
    bool persistentSecretsAvailable() const;
    bool saveAccount(Account account, QByteArray secret = {});
    void connectAccount(const QString &id);
    void disconnectAccount(const QString &id);
    void submitAuthentication(const QString &id, AuthenticationKind kind, const QString &value);
    void removeAccount(const QString &id);
    void startAutomaticConnections();
    void shutdown();
    IChatProvider *provider(const QString &id) const;
    bool requestProfile(const ConversationId &id);

signals:
    void accountsChanged();
    void accountRemoved(const QString &id);
    void accountStateChanged(const QString &id, phoschat::ConnectionState state);
    void errorOccurred(const QString &accountId, const QString &message);
    void authenticationRequired(
        const QString &accountId, phoschat::AuthenticationKind kind, const QString &detail);
    void authorizationUrlRequested(const QUrl &url);
    void conversationChanged(const phoschat::Conversation &conversation);
    void profileReceived(const phoschat::ConversationId &id, const phoschat::ProfileDetails &details);
    void joinFailed(const phoschat::ConversationId &id, const QString &detail);
    void messageReceived(const phoschat::Message &message);
    void messageRemoved(const phoschat::ConversationId &id, const QString &messageId);
    void messagesReadThrough(const phoschat::ConversationId &id, qint64 sequence);
    void historyStateChanged(const phoschat::ConversationId &id);
    void historyCleared(const phoschat::ConversationId &id);
    void membersChanged(const phoschat::ConversationId &id, const QStringList &members);
    void presenceChanged(const phoschat::ConversationId &id, phoschat::Presence presence);

private:
    struct Session;
    Session *addSession(const Account &account);
    void start(Session &session);
    void connectSession(const QString &id, bool interactive);
    void stop(Session &session);
    void createProvider(Session &session);
    void updateCredentials(Session &session, const QByteArray &secret);
    void onFailure(Session &session, const QString &message, bool retryable);
    void setState(Session &session, ConnectionState state);

    Database &database_;
    CredentialStore &credentials_;
    ProviderFactory factory_;
    QHash<QString, Session *> sessions_;
};

} // namespace phoschat
