#include "core/message_manager.h"

#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "storage/database.h"

#include <QUuid>

#include <algorithm>
#include <limits>

namespace phoschat {

MessageManager::MessageManager(
    AccountManager &accounts, ConversationManager &conversations, Database &database, QObject *parent)
    : QObject(parent)
    , accounts_(accounts)
    , conversations_(conversations)
    , database_(database)
{
    connect(&accounts_, &AccountManager::messageReceived, this, &MessageManager::receive);
    connect(&accounts_, &AccountManager::messageRemoved, this, &MessageManager::removeMessage);
    connect(&accounts_, &AccountManager::accountStateChanged, this,
        [this](const QString &id, ConnectionState state) {
            if (state != ConnectionState::Disconnected || accounts_.protocol(id) == Protocol::Irc)
                return;
            sessionHistory_.removeIf([&](auto it) { return it.key().accountId == id; });
            emit sessionHistoryCleared(id);
        });
}

QList<Message> MessageManager::history(const ConversationId &id, int limit)
{
    if (accounts_.protocol(id.accountId) == Protocol::Irc)
        return database_.messages(id, limit);
    const auto items = sessionHistory_.value(id);
    return items.last(std::clamp<qsizetype>(limit, 0, items.size()));
}

void MessageManager::requestHistory(const ConversationId &id)
{
    if (auto *provider = accounts_.provider(id.accountId);
        provider && provider->capabilities().testFlag(Capability::RemoteHistory))
        provider->requestHistory(id);
}

bool MessageManager::canSend(const ConversationId &id) const
{
    const auto value = conversations_.conversation(id);
    auto *provider = accounts_.provider(id.accountId);
    const bool irc = accounts_.protocol(id.accountId) == Protocol::Irc;
    return value && provider && accounts_.state(id.accountId) == ConnectionState::Connected
        && provider->capabilities().testFlag(Capability::Text)
        && (value->joined || (irc && value->kind != ConversationKind::Channel));
}

bool MessageManager::send(const ConversationId &id, const QString &text)
{
    if (text.trimmed().isEmpty())
        return false;
    auto *provider = accounts_.provider(id.accountId);
    if (!provider || accounts_.state(id.accountId) != ConnectionState::Connected) {
        emit errorOccurred(tr("The account is offline."));
        return false;
    }
    const bool irc = accounts_.protocol(id.accountId) == Protocol::Irc;
    if (irc && text.startsWith(u'/') && !text.startsWith(QStringLiteral("//"))) {
        const auto command = text.section(u' ', 0, 0).toLower();
        const auto arguments = text.section(u' ', 1).trimmed();
        if (command == QStringLiteral("/join"))
            return conversations_.joinChannel(id.accountId, arguments);
        if (command == QStringLiteral("/part")) {
            const ConversationId target = arguments.isEmpty()
                ? id
                : ConversationId { id.accountId, provider->canonicalTarget(arguments) };
            return conversations_.partChannel(target);
        }
        return provider->sendCommand(id, text);
    }
    if (!canSend(id)) {
        emit errorOccurred(irc ? tr("Join the channel before sending a message.")
                               : tr("Sending is unavailable in this conversation."));
        return false;
    }
    return provider->sendText(id, irc && text.startsWith(QStringLiteral("//")) ? text.mid(1) : text);
}

void MessageManager::receive(Message message)
{
    const auto &id = message.conversation;
    if (!id.isValid() || !accounts_.contains(id.accountId))
        return;
    auto value = conversations_.conversation(id);
    if (!value) {
        // Providers announce newly discovered conversations before their messages.
        emit errorOccurred(tr("Received a message for an unavailable conversation."));
        return;
    }
    if (message.id.isEmpty())
        message.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!message.timestamp.isValid())
        message.timestamp = QDateTime::currentDateTimeUtc();
    if (!value->updated.isValid() || message.timestamp > value->updated)
        value->updated = message.timestamp;
    if (!message.historical && !message.outgoing && value->unread < std::numeric_limits<int>::max())
        ++value->unread;

    if (accounts_.protocol(id.accountId) != Protocol::Irc) {
        auto &items = sessionHistory_[id];
        const auto duplicate = std::ranges::find(items, message.id, &Message::id);
        if (duplicate != items.end()) {
            if (duplicate->text != message.text) {
                message.historical = true;
                duplicate->text = message.text;
                emit messageAdded(message);
            }
            return;
        }
        const auto position = std::upper_bound(items.begin(), items.end(), message.timestamp,
            [](const QDateTime &time, const Message &entry) { return time < entry.timestamp; });
        items.insert(position, message);
        if (items.size() > 200)
            items.removeFirst();
        if (!message.historical)
            conversations_.save(*value);
        emit messageAdded(message);
        return;
    }

    const auto result = database_.appendMessage(message, *value);
    if (result == Database::AppendResult::Duplicate)
        return;
    if (result == Database::AppendResult::Error) {
        emit errorOccurred(tr("Could not save a message to local history."));
        return;
    }
    conversations_.conversations_.insert(id, *value);
    emit messageAdded(message);
    emit conversations_.conversationsChanged();
}

void MessageManager::removeMessage(const ConversationId &id, const QString &messageId)
{
    const auto entry = sessionHistory_.find(id);
    if (entry == sessionHistory_.end())
        return;
    entry->removeIf([&](const Message &message) { return message.id == messageId; });
    emit messageRemoved(id, messageId);
}

} // namespace phoschat
