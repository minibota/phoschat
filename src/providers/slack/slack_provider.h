#pragma once

#include "core/chat_provider.h"
#include "providers/slack/slack_oauth.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSet>
#include <QUrlQuery>

#include <functional>
#include <memory>
#include <optional>

class QFile;
class QNetworkReply;

namespace phoschat {

class SlackProvider final : public IChatProvider {
    Q_OBJECT

public:
    explicit SlackProvider(const Account &account, QObject *parent = nullptr);

    struct TestEndpoints {
        QUrl apiBase;
        QUrl authorize;
        int historyIntervalMs { 61000 };
        int pageIntervalMs { 3200 };
        int presenceIntervalMs { 61000 };
        QUrl oauthOrigin {};
        quint16 callbackPort {};
    };
    SlackProvider(const Account &account, const TestEndpoints &endpoints, QObject *parent = nullptr);

    Capabilities capabilities() const override;
    void connectAccount(const QByteArray &secret) override;
    void disconnectAccount() override;
    bool join(const QString &) override;
    bool part(const QString &) override;
    bool sendText(const ConversationId &id, const QString &text) override;
    bool canSendFiles(const ConversationId &id) const override;
    bool sendFile(const ConversationId &id, const QString &path) override;
    bool requestProfile(const ConversationId &id) override;
    bool sendCommand(const ConversationId &, const QString &) override;
    QString canonicalTarget(const QString &target) const override;
    void requestHistory(const ConversationId &id) override;
    void setConversationOpen(const ConversationId &id, bool open) override;

private:
    struct Credentials {
        QByteArray access;
        QByteArray refresh;
        qint64 expiresAtMs {};
        QString workspace;
        QString user;
        QUrl oauthOrigin;
    };
    struct Result {
        QJsonObject data;
        QString error;
        int retryAfterMs {};
        bool retryable {};
        bool ok() const { return error.isEmpty(); }
    };
    struct History {
        QString anchor;
        QString newest;
        QString cursor;
        QString before;
        bool loaded {};
        int pages {};
    };
    using Finished = std::function<void(const Result &)>;

    void reset();
    void fail(const QString &message, bool retryable = false);
    void setState(ConnectionState state);
    bool restoreCredentials(const QByteArray &secret);
    bool updateCredentials(const QJsonObject &response, bool exchange);
    void publishCredentials();
    void authorize();
    void exchangeCode(const QString &code, const QString &verifier, const QUrl &redirect);
    void refreshToken();
    void validateToken();
    void scheduleRefresh();
    void refreshConversations();
    void readHistory();
    void readProfile();
    void queueProfile(const QString &id);
    void readPresence();
    void publishPresence(const QString &channel, Presence presence);
    void uploadFile(const QString &channel, const QString &fileId, const QString &name, const QUrl &url,
        const std::shared_ptr<QFile> &file);
    void finishFile(const QString &channel, const QString &fileId, const QString &name);
    void fileFailed(const Result &result);
    void rememberScopes(const QString &scopes);
    void emitText(const QString &channel, const QJsonObject &object, bool historical, bool outgoing = false);
    ConversationId conversationId(const QString &channel) const;
    QString remoteChannel(const ConversationId &id) const;
    bool handleSessionError(const Result &result);
    void request(const QString &method, const QUrlQuery &query, std::optional<QByteArray> body,
        const QByteArray &contentType, bool authenticated, Finished finished);
    void requestAt(const QUrl &url, std::optional<QByteArray> body, const QByteArray &contentType,
        bool authenticated, Finished finished);

    Account account_;
    QUrl apiBase_;
    QUrl authorizationEndpoint_;
    QUrl oauthOrigin_;
    quint16 callbackPort_ { 8765 };
    int historyIntervalMs_ { 61000 };
    int listPageIntervalMs_ { 3200 };
    int presenceIntervalMs_ { 61000 };
    QNetworkAccessManager network_;
    SlackOAuth oauth_;
    QTimer listTimer_;
    QTimer historyTimer_;
    QTimer refreshTimer_;
    QTimer authTimer_;
    QTimer profileTimer_;
    QTimer presenceTimer_;
    QSet<QNetworkReply *> replies_;
    Credentials credentials_;
    QString selected_;
    QString nextListCursor_;
    QSet<QString> listed_;
    QHash<QString, Conversation> channels_;
    QHash<QString, QString> directPeers_;
    QHash<QString, QString> profiles_;
    QHash<QString, ProfileDetails> profileDetails_;
    QHash<QString, ProfileDetails> conversationDetails_;
    QSet<QString> requestedProfiles_;
    std::optional<QSet<QString>> grantedScopes_;
    QString activeProfile_;
    QStringList pendingProfiles_;
    QSet<QString> openConversations_;
    QHash<QString, Presence> presenceByChannel_;
    QHash<QString, qint64> nextPresenceCheck_;
    QHash<QString, History> histories_;
    QSet<QString> sending_;
    ConnectionState state_ { ConnectionState::Disconnected };
    unsigned generation_ {};
    bool historyInFlight_ {};
    bool refreshing_ {};
    bool refreshDeferred_ {};
    bool credentialsDirty_ {};
    bool profileInFlight_ {};
    bool uploadInFlight_ {};
    bool presenceInFlight_ {};
    bool presenceUnavailable_ {};
};

} // namespace phoschat
