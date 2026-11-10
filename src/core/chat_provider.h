#pragma once

#include "models/types.h"

#include <QByteArray>
#include <QObject>
#include <QUrl>

namespace phoschat {

class IChatProvider : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IChatProvider() override = default;

    virtual Capabilities capabilities() const = 0;
    virtual void connectAccount(const QByteArray &secret) = 0;
    virtual void disconnectAccount() = 0;
    virtual bool join(const QString &target) = 0;
    virtual bool part(const QString &target) = 0;
    virtual bool sendText(const ConversationId &id, const QString &text) = 0;
    virtual bool canSendFiles(const ConversationId &) const { return false; }
    virtual bool sendFile(const ConversationId &, const QString &) { return false; }
    virtual bool sendCommand(const ConversationId &id, const QString &command) = 0;
    virtual QString canonicalTarget(const QString &target) const = 0;
    virtual void submitAuthentication(AuthenticationKind, const QString &) { }
    virtual void requestHistory(const ConversationId &) { }
    virtual HistoryState historyState(const ConversationId &) const { return HistoryState::Unavailable; }
    virtual void requestOlderHistory(const ConversationId &) { }
    virtual void setConversationOpen(const ConversationId &, bool) { }
    virtual bool requestProfile(const ConversationId &) { return false; }
    virtual void markRead(const ConversationId &) { }

signals:
    void stateChanged(phoschat::ConnectionState state);
    void errorOccurred(const QString &message, bool retryable);
    void notice(const QString &message);
    void authenticationRequired(phoschat::AuthenticationKind kind, const QString &detail);
    void authorizationUrlRequested(const QUrl &url);
    void credentialsUpdated(const QByteArray &secret);
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
};

} // namespace phoschat
