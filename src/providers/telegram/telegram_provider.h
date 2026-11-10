#pragma once

#include "core/chat_provider.h"
#include "security/telegram_credentials.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>

#include <array>
#include <optional>

namespace phoschat {

class TelegramTransport;

class TelegramProvider final : public IChatProvider {
    Q_OBJECT

public:
    explicit TelegramProvider(Account account, QObject *parent = nullptr);
    TelegramProvider(Account account, TelegramTransport &transport, QObject *parent = nullptr);
    ~TelegramProvider() override;

    static bool isAvailable();
    static void removeLocalData(const QString &accountId);

    Capabilities capabilities() const override;
    void connectAccount(const QByteArray &secret) override;
    void disconnectAccount() override;
    bool join(const QString &target) override;
    bool part(const QString &target) override;
    bool sendText(const ConversationId &id, const QString &text) override;
    bool sendCommand(const ConversationId &id, const QString &command) override;
    QString canonicalTarget(const QString &target) const override;
    void submitAuthentication(AuthenticationKind kind, const QString &value) override;
    void markRead(const ConversationId &id) override;
    void requestHistory(const ConversationId &id) override;

private:
    struct Chat {
        QString title;
        ConversationKind kind;
        qint64 latest {};
        bool canSend { true };
        bool protectedContent {};
        bool autoDeletes {};
    };

    struct ChatListLoad {
        bool pending {};
        bool complete {};
    };

    void setState(ConnectionState state);
    void reset();
    void processEvent(const QJsonObject &event);
    void processResult(const QJsonObject &event);
    void processAuthorization(const QJsonObject &state);
    void sendParameters();
    void finishKeyMigration();
    void rotateSessionKeyIfReady();
    void prompt(AuthenticationKind kind, const QString &detail = {});
    void fail(const QString &message, bool retryable = false);
    void request(QJsonObject object, const QString &operation, qint64 chatId = 0);
    void updateConnectionState();
    void updateUser(const QJsonObject &user);
    void loadMoreChats(bool archived);
    void acceptChat(const QJsonObject &chat);
    void announce(qint64 chatId, const Chat &chat);
    void acceptMessage(const QJsonObject &message, bool historical);
    qint64 chatId(const ConversationId &id) const;
    void fetchHistory(qint64 id);
    void sendRead(qint64 id);

    const Account account_;
    TelegramTransport &transport_;
    ConnectionState state_ { ConnectionState::Disconnected };
    int client_ {};
    QString profile_;
    QByteArray apiHash_;
    int apiId_ {};
    TelegramCredentials credentials_;
    QJsonObject pendingAuthorization_;
    bool legacyKeyInUse_ {};
    bool parametersPending_ {};
    bool parametersConfigured_ {};
    bool keyRotationPending_ {};
    qint64 owner_ {};
    std::optional<AuthenticationKind> challenge_;
    QString challengeDetail_;
    bool authenticationPending_ {};
    bool authorized_ {};
    bool networkReady_ {};
    std::array<ChatListLoad, 2> chatLists_ {};
    QHash<qint64, Chat> chats_;
    QHash<qint64, QString> users_;
    QSet<qint64> historyPending_;
    QSet<qint64> historyLoaded_;
    QSet<qint64> chatPending_;
    QSet<qint64> readPending_;
};

} // namespace phoschat
