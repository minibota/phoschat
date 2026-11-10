#pragma once

#include "ui/conversations/MessageModel.hpp"

namespace phoschat {
class MessageManager;
}

namespace phoschat::ui {

class ConversationController final : public QObject {
    Q_OBJECT
public:
    ConversationController(ConversationId id, MessageManager &messages, QObject *parent = nullptr);

    const ConversationId &id() const { return m_id; }
    MessageModel &model() { return m_model; }
    const QString &draft() const { return m_draft; }
    void setDraft(const QString &draft) { m_draft = draft; }
    bool canSend() const;
    bool send();
    bool canSendFiles() const;
    bool sendFile(const QString &path);
    bool hasPagedHistory() const;
    HistoryState historyState() const;
    void requestOlderHistory();

signals:
    void historyStateChanged();

private:
    ConversationId m_id;
    MessageManager &m_messages;
    MessageModel m_model;
    QString m_draft;
};

} // namespace phoschat::ui
