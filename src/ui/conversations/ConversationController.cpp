#include "ui/conversations/ConversationController.hpp"

#include "core/message_manager.h"

#include <utility>

namespace phoschat::ui {

ConversationController::ConversationController(ConversationId id, MessageManager &messages, QObject *parent)
    : QObject(parent)
    , m_id(std::move(id))
    , m_messages(messages)
    , m_model(messages.history(m_id))
{
    connect(&m_messages, &MessageManager::messageAdded, this, [this](const Message &message) {
        if (message.conversation == m_id) {
            m_model.append(message);
        }
    });
    connect(&m_messages, &MessageManager::messageRemoved, this,
        [this](const ConversationId &id, const QString &messageId) {
            if (id == m_id)
                m_model.remove(messageId);
        });
    connect(&m_messages, &MessageManager::sessionHistoryCleared, this, [this](const QString &accountId) {
        if (accountId == m_id.accountId) {
            m_model.clear();
        }
    });
}

bool ConversationController::canSend() const { return m_messages.canSend(m_id); }

bool ConversationController::send()
{
    if (m_draft.trimmed().isEmpty() || !m_messages.send(m_id, m_draft)) {
        return false;
    }
    m_draft.clear();
    return true;
}

} // namespace phoschat::ui
