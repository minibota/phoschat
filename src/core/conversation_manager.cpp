#include "core/conversation_manager.h"

#include "core/account_manager.h"
#include "storage/database.h"

#include <QRegularExpression>
#include <QTimer>

namespace phoschat {

ConversationManager::ConversationManager(AccountManager &accounts, Database &database, QObject *parent)
    : QObject(parent)
    , accounts_(accounts)
    , database_(database)
{
    for (auto value : database_.conversations()) {
        value.joined = false;
        conversations_.insert(value.id, value);
    }
    connect(&accounts_, &AccountManager::conversationChanged, this,
        &ConversationManager::acceptRemoteConversation);
    connect(&accounts_, &AccountManager::presenceChanged, this,
        [this](const ConversationId &id, Presence presence) {
            if (accounts_.state(id.accountId) != ConnectionState::Connected)
                return;
            const auto conversation = conversations_.find(id);
            if (conversation != conversations_.end() && conversation->kind == ConversationKind::Direct
                && conversation->presence != presence) {
                conversation->presence = presence;
                emit conversationsChanged();
            }
        });
    connect(&accounts_, &AccountManager::joinFailed, this,
        [this](const ConversationId &id, const QString &detail) {
            pendingJoins_.remove(id);
            emit errorOccurred(detail);
        });
    connect(&accounts_, &AccountManager::membersChanged, this,
        [this](const ConversationId &id, const QStringList &names) {
            if (!conversations_.contains(id))
                return;
            members_.insert(id, names);
            emit membersChanged(id);
        });
    connect(&accounts_, &AccountManager::accountRemoved, this, &ConversationManager::dropAccount);
    connect(&accounts_, &AccountManager::accountStateChanged, this,
        [this](const QString &id, ConnectionState state) {
            if (state == ConnectionState::Connected)
                rejoin(id);
            else
                clearMembership(id);
        });
}

QList<Conversation> ConversationManager::conversations() const { return conversations_.values(); }

std::optional<Conversation> ConversationManager::conversation(const ConversationId &id) const
{
    const auto it = conversations_.constFind(id);
    return it == conversations_.cend() ? std::nullopt : std::optional<Conversation>(*it);
}

bool ConversationManager::joinChannel(const QString &accountId, const QString &target)
{
    auto *provider = accounts_.provider(accountId);
    if (!provider || accounts_.state(accountId) != ConnectionState::Connected
        || !provider->capabilities().testFlag(Capability::Channels)) {
        emit errorOccurred(tr("Connect an account before joining a channel."));
        return false;
    }
    const QString name = target.trimmed();
    const ConversationId id { accountId, provider->canonicalTarget(name) };
    if (!id.isValid())
        return false;
    if (auto value = conversation(id); value && value->joined) {
        value->autoJoin = true;
        if (!save(*value))
            return false;
        emit channelJoined(id);
        return true;
    }
    if (pendingJoins_.contains(id))
        return true;
    const auto attempt = ++nextJoinAttempt_;
    pendingJoins_.insert(id, attempt);
    if (!provider->join(name)) {
        pendingJoins_.remove(id);
        return false;
    }
    if (pendingJoins_.value(id) == attempt) {
        emit channelJoinStarted(name);
        QTimer::singleShot(12000, this, [this, id, name, attempt] {
            if (pendingJoins_.value(id) == attempt) {
                pendingJoins_.remove(id);
                emit errorOccurred(tr("The IRC server did not confirm joining %1. Try again.").arg(name));
            }
        });
    }
    return true;
}

ConversationId ConversationManager::openDirect(const QString &accountId, const QString &target)
{
    auto *provider = accounts_.provider(accountId);
    static const QRegularExpression invalid(QStringLiteral("[\\s,#!&:+*?@.\\x00-\\x1f\\x7f]"));
    const QString name = target.trimmed();
    if (!provider || accounts_.state(accountId) != ConnectionState::Connected
        || !provider->capabilities().testFlag(Capability::DirectMessages) || name.isEmpty()
        || name.contains(invalid) || name.toUtf8().size() > 128) {
        emit errorOccurred(tr("Connect an account and enter a valid nickname."));
        return {};
    }
    const ConversationId id { accountId, provider->canonicalTarget(name) };
    if (conversations_.contains(id))
        return id;
    Conversation value;
    value.id = id;
    value.title = name;
    value.kind = ConversationKind::Direct;
    return save(value) ? id : ConversationId {};
}

bool ConversationManager::partChannel(const ConversationId &id)
{
    auto value = conversation(id);
    if (!value || value->kind != ConversationKind::Channel)
        return false;
    if (value->joined) {
        auto *provider = accounts_.provider(id.accountId);
        if (!provider || !provider->part(value->title))
            return false;
    }
    value->autoJoin = false;
    return save(*value);
}

void ConversationManager::setFavorite(const ConversationId &id, bool enabled)
{
    if (auto value = conversation(id); value && value->favorite != enabled) {
        value->favorite = enabled;
        save(*value);
    }
}

void ConversationManager::setMuted(const ConversationId &id, bool enabled)
{
    if (auto value = conversation(id); value && value->muted != enabled) {
        value->muted = enabled;
        save(*value);
    }
}

void ConversationManager::setGroup(const ConversationId &id, const QString &group)
{
    if (auto value = conversation(id); value && value->group != group.trimmed()) {
        value->group = group.trimmed();
        save(*value);
    }
}

void ConversationManager::markRead(const ConversationId &id)
{
    if (auto *provider = accounts_.provider(id.accountId); provider
        && accounts_.state(id.accountId) == ConnectionState::Connected
        && accounts_.protocol(id.accountId) != Protocol::Irc)
        provider->markRead(id);
    if (auto value = conversation(id); value && value->unread > 0) {
        value->unread = 0;
        save(*value);
    }
}

QStringList ConversationManager::members(const ConversationId &id) const { return members_.value(id); }

void ConversationManager::acceptRemoteConversation(const Conversation &remote)
{
    if (!remote.id.isValid() || !accounts_.contains(remote.id.accountId))
        return;
    const auto existing = conversation(remote.id);
    const bool confirmedJoin = remote.joined && pendingJoins_.remove(remote.id);
    if (existing && existing->title == remote.title && existing->kind == remote.kind
        && existing->joined == remote.joined && !confirmedJoin) {
        if (existing->avatar != remote.avatar) {
            auto &value = conversations_[remote.id];
            value.avatar = remote.avatar;
            emit conversationsChanged();
        }
        return;
    }
    Conversation value = existing.value_or(remote);
    value.title = remote.title;
    value.kind = remote.kind;
    value.joined = remote.joined;
    value.avatar = remote.avatar;
    if (!existing || value.kind != ConversationKind::Direct)
        value.presence = Presence::Unknown;
    if (confirmedJoin)
        value.autoJoin = true;
    if (!value.joined && members_.remove(value.id))
        emit membersChanged(value.id);
    if (save(value) && confirmedJoin)
        emit channelJoined(value.id);
}

bool ConversationManager::save(const Conversation &value)
{
    if (!database_.saveConversation(value)) {
        emit errorOccurred(tr("Could not save the conversation locally."));
        return false;
    }
    conversations_.insert(value.id, value);
    emit conversationsChanged();
    return true;
}

void ConversationManager::dropAccount(const QString &accountId)
{
    pendingJoins_.removeIf([&](auto it) { return it.key().accountId == accountId; });
    conversations_.removeIf([&](auto it) { return it.key().accountId == accountId; });
    members_.removeIf([&](auto it) { return it.key().accountId == accountId; });
    emit conversationsChanged();
}

void ConversationManager::clearMembership(const QString &accountId)
{
    pendingJoins_.removeIf([&](auto it) { return it.key().accountId == accountId; });
    bool changed = false;
    for (auto it = conversations_.begin(); it != conversations_.end(); ++it) {
        if (it.key().accountId != accountId)
            continue;
        if (it->joined) {
            it->joined = false;
            changed = true;
        }
        if (it->presence != Presence::Unknown) {
            it->presence = Presence::Unknown;
            changed = true;
        }
        if (members_.remove(it.key()))
            emit membersChanged(it.key());
    }
    if (changed)
        emit conversationsChanged();
}

void ConversationManager::rejoin(const QString &accountId)
{
    auto *provider = accounts_.provider(accountId);
    if (!provider || !provider->capabilities().testFlag(Capability::Channels))
        return;
    for (const auto &value : conversations_) {
        if (value.id.accountId == accountId && value.kind == ConversationKind::Channel && value.autoJoin)
            provider->join(value.title);
    }
}

} // namespace phoschat
