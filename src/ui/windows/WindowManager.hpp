#pragma once

#include "models/types.h"

#include <QHash>
#include <QObject>
#include <QPointer>

namespace phoschat {
class AccountManager;
class ConversationManager;
class MessageManager;
}

namespace phoschat::ui {

class ChatWindow;
class ConversationController;

class WindowManager final : public QObject {
public:
    WindowManager(AccountManager &accounts, ConversationManager &conversations, MessageManager &messages,
        QObject *parent = nullptr);
    ~WindowManager() override;

    void open(const ConversationId &id);
    bool isActive(const ConversationId &id) const;
    void restoreSession();
    void saveSession() const;
    void closeAll();

private:
    void showConversation(const ConversationId &id, bool focus);
    void refreshOpenWindows();
    void refreshWindow(ChatWindow &window, const Conversation &conversation) const;
    void pruneRemovedConversations();

    AccountManager &m_accounts;
    ConversationManager &m_conversations;
    MessageManager &m_messages;
    QHash<ConversationId, QPointer<ChatWindow>> m_windows;
    QHash<ConversationId, ConversationController *> m_controllers;
    bool m_closing {};
};

} // namespace phoschat::ui
