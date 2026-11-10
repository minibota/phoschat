#include "core/account_manager.h"

#include "providers/irc/irc_provider.h"
#include "providers/slack/slack_provider.h"
#include "providers/telegram/telegram_provider.h"
#include "security/credential_store.h"
#include "storage/database.h"

#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace phoschat {
namespace {

    bool sameEndpoint(const Account &first, const Account &second)
    {
        return first.protocol == second.protocol && first.host == second.host && first.port == second.port
            && first.tls == second.tls && first.nick == second.nick && first.saslUser == second.saslUser
            && first.providerId == second.providerId;
    }

    bool needsSecret(const Account &account)
    {
        return account.protocol == Protocol::Telegram
            || (account.protocol == Protocol::Irc && !account.saslUser.isEmpty())
            || (account.protocol == Protocol::Slack && account.providerId.isEmpty());
    }

} // namespace

struct AccountManager::Session final : QObject {
    Session(const Account &value, QObject *parent)
        : QObject(parent)
        , account(value)
    {
        retry.setSingleShot(true);
    }

    Account account;
    IChatProvider *provider {};
    QTimer retry;
    QByteArray secret;
    QString detail;
    ConnectionState state { ConnectionState::Disconnected };
    unsigned attempt {};
    unsigned generation {};
    bool wanted {};
    bool secretOperation {};
    bool removing {};
    bool interactive {};
};

AccountManager::AccountManager(
    Database &database, CredentialStore &credentials, QObject *parent, ProviderFactory factory)
    : QObject(parent)
    , database_(database)
    , credentials_(credentials)
    , factory_(std::move(factory))
{
    if (!factory_) {
        factory_ = [](const Account &account, QObject *owner) -> IChatProvider * {
            if (account.protocol == Protocol::Irc)
                return new IrcProvider(account, owner);
            if (account.protocol == Protocol::Telegram)
                return new TelegramProvider(account, owner);
            if (account.protocol == Protocol::Slack)
                return new SlackProvider(account, owner);
            return nullptr;
        };
    }
    for (const auto &account : database_.accounts())
        addSession(account);
}

AccountManager::Session *AccountManager::addSession(const Account &account)
{
    auto *session = new Session(account, this);
    sessions_.insert(account.id, session);
    connect(&session->retry, &QTimer::timeout, session, [this, session] {
        if (session->wanted)
            start(*session);
    });
    return session;
}

QList<Account> AccountManager::accounts() const
{
    QList<Account> values;
    values.reserve(sessions_.size());
    for (const auto *session : sessions_)
        values.append(session->account);
    std::ranges::sort(values, {}, &Account::label);
    return values;
}

bool AccountManager::contains(const QString &id) const { return sessions_.contains(id); }

Protocol AccountManager::protocol(const QString &id) const
{
    const auto *session = sessions_.value(id);
    return session ? session->account.protocol : Protocol::Irc;
}

bool AccountManager::protocolAvailable(Protocol protocol) const
{
    return protocol == Protocol::Irc || protocol == Protocol::Slack
        || (protocol == Protocol::Telegram && TelegramProvider::isAvailable());
}

ConnectionState AccountManager::state(const QString &id) const
{
    const auto *session = sessions_.value(id);
    return session ? session->state : ConnectionState::Disconnected;
}

QString AccountManager::statusDetail(const QString &id) const
{
    const auto *session = sessions_.value(id);
    return session ? session->detail : QString {};
}

IChatProvider *AccountManager::provider(const QString &id) const
{
    const auto *session = sessions_.value(id);
    return session ? session->provider : nullptr;
}

bool AccountManager::persistentSecretsAvailable() const { return credentials_.persistentAvailable(); }

bool AccountManager::saveAccount(Account account, QByteArray secret)
{
    account.label = account.label.trimmed();
    account.host = account.host.trimmed();
    account.nick = account.nick.trimmed();
    account.saslUser = account.saslUser.trimmed();
    account.providerId = account.providerId.trimmed();
    if (account.id.isEmpty() || account.label.isEmpty()
        || (account.protocol != Protocol::Irc && account.protocol != Protocol::Telegram
            && account.protocol != Protocol::Slack)) {
        emit errorOccurred(account.id, tr("Enter an account name and choose a supported protocol."));
        return false;
    }
    static const QRegularExpression invalidHost(QStringLiteral("[\\s/\\x00-\\x1f\\x7f]"));
    static const QRegularExpression invalidNick(QStringLiteral("[\\s,#!&:+*?@.\\x00-\\x1f\\x7f]"));
    if (account.protocol == Protocol::Irc
        && (account.host.isEmpty() || account.host.contains(invalidHost) || account.port == 0
            || account.nick.isEmpty() || account.nick.contains(invalidNick)
            || account.nick.toUtf8().size() > 100
            || (!account.saslUser.isEmpty() && (!account.tls || account.saslUser.contains(QChar::Null)))
            || (!secret.isEmpty() && (account.saslUser.isEmpty() || secret.contains('\0'))))) {
        emit errorOccurred(
            account.id, tr("Enter a label, server, port and valid nickname. SASL requires TLS."));
        return false;
    }
    if (account.protocol == Protocol::Irc && account.saslUser.isEmpty())
        account.saveSecret = false;
    if (account.protocol == Protocol::Telegram) {
        bool validId = false;
        const int apiId = account.providerId.toInt(&validId);
        static const QRegularExpression hash(QStringLiteral("^[0-9A-Fa-f]{32}$"));
        if (!validId || apiId <= 0
            || (!secret.isEmpty() && !hash.match(QString::fromLatin1(secret)).hasMatch())) {
            emit errorOccurred(account.id, tr("Enter a valid Telegram API ID and 32-character API hash."));
            return false;
        }
    }
    if (account.protocol == Protocol::Slack) {
        static const QRegularExpression clientId(QStringLiteral("^[0-9]{1,32}\\.[0-9]{1,32}$"));
        if ((!account.providerId.isEmpty() && !clientId.match(account.providerId).hasMatch())
            || (!secret.isEmpty()
                && (!secret.startsWith("xoxp-") || secret.size() > 4096
                    || std::ranges::any_of(secret, [](char c) { return c <= ' ' || c >= 127; })))) {
            emit errorOccurred(
                account.id, tr("Enter a valid Slack Client ID or a user token beginning with xoxp-."));
            return false;
        }
    }

    auto *session = sessions_.value(account.id);
    if (session && (session->secretOperation || session->removing)) {
        emit errorOccurred(account.id, tr("Wait for the current credential operation to finish."));
        return false;
    }
    const Account previous = session ? session->account : Account {};
    if (session
        && (previous.protocol != account.protocol || previous.providerId != account.providerId
            || (account.protocol == Protocol::Irc
                && (previous.host.compare(account.host, Qt::CaseInsensitive) != 0
                    || previous.port != account.port || previous.tls != account.tls)))) {
        emit errorOccurred(account.id, tr("Add a new account to use a different network or application ID."));
        return false;
    }
    if (session && previous.saveSecret && account.protocol == Protocol::Telegram && !secret.isEmpty()) {
        emit errorOccurred(
            account.id, tr("Remove and recreate this Telegram account to change its saved API hash."));
        return false;
    }
    if (!session && needsSecret(account) && account.protocol != Protocol::Irc && secret.isEmpty()) {
        emit errorOccurred(account.id, tr("Enter the provider credential to add this account."));
        return false;
    }
    const bool newCredentialIdentity = !session || previous.saslUser != account.saslUser;
    if (previous.saveSecret && newCredentialIdentity) {
        emit errorOccurred(account.id, tr("Remove the saved password before changing the SASL username."));
        return false;
    }
    const bool preserveStored = previous.saveSecret && !newCredentialIdentity && account.saveSecret;
    const bool eraseStored = previous.saveSecret && !account.saveSecret;
    const bool suppliedSecret = !secret.isEmpty();

    if (secret.isEmpty() && session && !newCredentialIdentity)
        secret = session->secret;
    const bool updateStored = account.saveSecret && !secret.isEmpty() && (!preserveStored || suppliedSecret);
    if (account.saveSecret
        && (!persistentSecretsAvailable() || (!preserveStored && secret.isEmpty() && needsSecret(account)))) {
        emit errorOccurred(
            account.id, tr("A working system credential store and the account credential are required."));
        return false;
    }

    // Keep tracking a stored key until the keychain confirms its removal.
    Account persisted = account;
    if (eraseStored)
        persisted.saveSecret = true;
    if (!database_.saveAccount(persisted)) {
        emit errorOccurred(account.id, tr("Could not save account settings."));
        return false;
    }
    if (!session)
        session = addSession(account);
    else if (!sameEndpoint(previous, account)
        || (account.protocol == Protocol::Telegram && previous.saveSecret != account.saveSecret)
        || (account.protocol != Protocol::Irc && !secret.isEmpty() && session->secret != secret))
        stop(*session);
    session->account = account;
    session->secret = std::move(secret);
    if (account.protocol == Protocol::Irc && account.saslUser.isEmpty())
        session->secret.clear();

    if (updateStored || eraseStored) {
        session->secretOperation = true;
        ++session->generation;
        const QPointer<Session> guard(session);
        auto complete = [this, guard, updateStored, eraseStored, previouslySaved = previous.saveSecret](
                            const QString &error) {
            if (!guard)
                return;
            auto &current = *guard;
            current.secretOperation = false;
            if (!error.isEmpty()) {
                // A failed write can still be used in memory, and an unchanged old key remains tracked.
                current.account.saveSecret = previouslySaved;
                if (!database_.saveAccount(current.account))
                    emit errorOccurred(current.account.id, tr("Could not reconcile account settings."));
                emit errorOccurred(current.account.id, error);
                emit accountsChanged();
            } else if (updateStored) {
                current.secret.clear();
            } else if (eraseStored) {
                if (current.account.protocol == Protocol::Telegram)
                    TelegramProvider::removeLocalData(current.account.id);
                if (!database_.saveAccount(current.account)) {
                    current.account.saveSecret = previouslySaved;
                    emit errorOccurred(current.account.id, tr("Could not reconcile account settings."));
                    emit accountsChanged();
                }
            }
            if (current.wanted && current.state == ConnectionState::Connecting)
                start(current);
        };
        if (updateStored)
            credentials_.write(account.id, session->secret, std::move(complete));
        else
            credentials_.remove(account.id, std::move(complete));
    }
    emit accountsChanged();
    return true;
}

void AccountManager::connectAccount(const QString &id) { connectSession(id, true); }

void AccountManager::connectSession(const QString &id, bool interactive)
{
    auto *session = sessions_.value(id);
    if (!session || session->removing || (session->wanted && !session->retry.isActive()))
        return;
    session->wanted = true;
    session->interactive = interactive;
    session->attempt = 0;
    session->retry.stop();
    session->detail.clear();
    start(*session);
}

void AccountManager::submitAuthentication(const QString &id, AuthenticationKind kind, const QString &value)
{
    if (auto *chat = provider(id); chat && state(id) == ConnectionState::Authenticating)
        chat->submitAuthentication(kind, value);
}

void AccountManager::disconnectAccount(const QString &id)
{
    if (auto *session = sessions_.value(id))
        stop(*session);
}

void AccountManager::startAutomaticConnections()
{
    for (const auto &account : accounts()) {
        if (account.autoConnect)
            connectSession(account.id, false);
    }
}

void AccountManager::shutdown()
{
    for (auto *session : std::as_const(sessions_))
        stop(*session);
}

void AccountManager::start(Session &session)
{
    setState(session, ConnectionState::Connecting);
    if (session.secretOperation)
        return;
    if (!session.provider)
        createProvider(session);
    if (!session.provider) {
        onFailure(session, tr("This protocol is unavailable in this release."), false);
        return;
    }
    if (!session.secret.isEmpty()
        || (session.account.protocol == Protocol::Irc && !needsSecret(session.account))) {
        session.provider->connectAccount(session.secret);
        return;
    }
    if (!session.account.saveSecret) {
        if (!needsSecret(session.account) && session.interactive)
            session.provider->connectAccount({});
        else
            onFailure(session, tr("Enter credentials or authorize the account from Accounts."), false);
        return;
    }
    const unsigned generation = ++session.generation;
    const QPointer<Session> guard(&session);
    credentials_.read(session.account.id, [this, guard, generation](QByteArray secret, const QString &error) {
        if (!guard || !guard->wanted || guard->generation != generation)
            return;
        if (secret.isEmpty() || !error.isEmpty()) {
            if (guard->account.protocol == Protocol::Slack && guard->interactive
                && !needsSecret(guard->account)) {
                guard->provider->connectAccount({});
                return;
            }
            onFailure(*guard, error.isEmpty() ? tr("The saved password is missing.") : error, false);
            return;
        }
        guard->provider->connectAccount(secret);
        secret.fill('\0');
    });
}

void AccountManager::createProvider(Session &session)
{
    session.provider = factory_(session.account, &session);
    if (!session.provider)
        return;
    auto *provider = session.provider;
    connect(provider, &IChatProvider::stateChanged, &session, [this, &session](ConnectionState state) {
        if (state == ConnectionState::Connected) {
            session.attempt = 0;
            session.detail.clear();
            session.interactive = false;
        }
        setState(session, state);
    });
    connect(provider, &IChatProvider::errorOccurred, &session,
        [this, &session](const QString &message, bool retryable) { onFailure(session, message, retryable); });
    connect(provider, &IChatProvider::notice, &session,
        [this, &session](const QString &message) { emit errorOccurred(session.account.id, message); });
    connect(provider, &IChatProvider::conversationChanged, this, &AccountManager::conversationChanged);
    connect(provider, &IChatProvider::messageReceived, this, &AccountManager::messageReceived);
    connect(provider, &IChatProvider::messageRemoved, this, &AccountManager::messageRemoved);
    connect(provider, &IChatProvider::membersChanged, this, &AccountManager::membersChanged);
    connect(provider, &IChatProvider::authenticationRequired, &session,
        [this, &session](AuthenticationKind kind, const QString &detail) {
            emit authenticationRequired(session.account.id, kind, detail);
        });
    connect(provider, &IChatProvider::authorizationUrlRequested, this,
        &AccountManager::authorizationUrlRequested);
    connect(provider, &IChatProvider::credentialsUpdated, &session,
        [this, &session](const QByteArray &secret) { updateCredentials(session, secret); });
}

void AccountManager::updateCredentials(Session &session, const QByteArray &secret)
{
    session.secret = secret;
    if (!session.account.saveSecret || session.secretOperation)
        return;
    session.secretOperation = true;
    const QPointer<Session> guard(&session);
    credentials_.write(session.account.id, secret, [this, guard, secret](const QString &error) {
        if (!guard)
            return;
        guard->secretOperation = false;
        if (!error.isEmpty())
            emit errorOccurred(guard->account.id, error);
        if (guard->secret != secret)
            updateCredentials(*guard, guard->secret);
        if (guard->wanted && guard->state == ConnectionState::Connecting)
            start(*guard);
    });
}

void AccountManager::stop(Session &session)
{
    session.wanted = false;
    ++session.generation;
    session.retry.stop();
    session.detail.clear();
    if (auto *provider = std::exchange(session.provider, nullptr)) {
        provider->disconnect(&session);
        provider->disconnect(this);
        provider->disconnectAccount();
        provider->deleteLater();
    }
    setState(session, ConnectionState::Disconnected);
}

void AccountManager::removeAccount(const QString &id)
{
    auto *session = sessions_.value(id);
    if (!session || session->removing)
        return;
    if (session->secretOperation) {
        emit errorOccurred(id, tr("Wait for the current credential operation to finish."));
        return;
    }

    stop(*session);
    session->removing = true;
    const QPointer<Session> guard(session);
    auto finish = [this, guard, id](const QString &error) {
        if (!guard)
            return;
        guard->removing = false;
        if (!error.isEmpty()) {
            emit errorOccurred(id, error);
            return;
        }
        if (!database_.removeAccount(id)) {
            emit errorOccurred(id, tr("Could not remove account and local history."));
            return;
        }
        if (guard->account.protocol == Protocol::Telegram)
            TelegramProvider::removeLocalData(id);
        sessions_.remove(id);
        guard->deleteLater();
        emit accountRemoved(id);
        emit accountsChanged();
    };
    if (session->account.saveSecret)
        credentials_.remove(id, std::move(finish));
    else
        finish({});
}

void AccountManager::onFailure(Session &session, const QString &message, bool retryable)
{
    if (!session.wanted)
        return;
    session.detail = message;
    if (retryable) {
        const unsigned delay = std::min(60u, 2u << std::min(5u, session.attempt++));
        session.retry.start(static_cast<int>(delay * 1000));
        setState(session, ConnectionState::Reconnecting);
    } else {
        session.wanted = false;
        setState(session, ConnectionState::Error);
    }
    emit errorOccurred(session.account.id, message);
}

void AccountManager::setState(Session &session, ConnectionState state)
{
    if (session.state == state)
        return;
    session.state = state;
    emit accountStateChanged(session.account.id, state);
}

} // namespace phoschat
