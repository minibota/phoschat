#include "storage/database.h"

#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace phoschat {
namespace {

    using namespace Qt::StringLiterals;

    constexpr int ApplicationId = 0x50484348; // PHCH
    constexpr int SchemaVersion = 3;

    const QStringList InitialSchema {
        uR"sql(CREATE TABLE accounts (
        id TEXT PRIMARY KEY NOT NULL,
        label TEXT NOT NULL,
        protocol INTEGER NOT NULL,
        host TEXT NOT NULL,
        port INTEGER NOT NULL CHECK (port BETWEEN 1 AND 65535),
        tls INTEGER NOT NULL,
        nick TEXT NOT NULL,
        sasl_user TEXT NOT NULL,
        save_secret INTEGER NOT NULL,
        auto_connect INTEGER NOT NULL
    ))sql"_s,
        uR"sql(CREATE TABLE conversations (
        account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
        remote_id TEXT NOT NULL,
        title TEXT NOT NULL,
        kind INTEGER NOT NULL,
        joined INTEGER NOT NULL,
        favorite INTEGER NOT NULL,
        muted INTEGER NOT NULL,
        auto_join INTEGER NOT NULL,
        custom_group TEXT NOT NULL,
        unread INTEGER NOT NULL CHECK (unread >= 0),
        updated INTEGER,
        PRIMARY KEY (account_id, remote_id)
    ))sql"_s,
        uR"sql(CREATE TABLE messages (
        row_id INTEGER PRIMARY KEY,
        account_id TEXT NOT NULL,
        conversation_id TEXT NOT NULL,
        message_id TEXT NOT NULL,
        sender TEXT NOT NULL,
        body TEXT NOT NULL,
        timestamp INTEGER NOT NULL,
        outgoing INTEGER NOT NULL,
        mention INTEGER NOT NULL,
        UNIQUE (account_id, conversation_id, message_id),
        FOREIGN KEY (account_id, conversation_id)
            REFERENCES conversations(account_id, remote_id) ON DELETE CASCADE
    ))sql"_s,
    };

    const QStringList HistoryIndexes {
        uR"sql(CREATE INDEX messages_by_conversation_time
        ON messages (account_id, conversation_id, timestamp DESC, row_id DESC))sql"_s,
    };

    const QStringList ProviderConfiguration {
        u"ALTER TABLE accounts ADD COLUMN provider_id TEXT NOT NULL DEFAULT ''"_s,
    };

    QString nonNull(QString text) { return text.isNull() ? u""_s : text; }

    QDateTime readTimestamp(const QVariant &value)
    {
        return value.isNull() ? QDateTime {}
                              : QDateTime::fromMSecsSinceEpoch(value.toLongLong(), QTimeZone::utc());
    }

} // namespace

Database::Database(QString path)
    : m_path(std::move(path))
    , m_connectionName(u"phoschat-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

Database::~Database() { close(); }

bool Database::open()
{
    m_lastError.clear();
    if (m_connection.isOpen()) {
        return true;
    }
    if (!QSqlDatabase::isDriverAvailable(u"QSQLITE"_s)) {
        m_lastError = u"The Qt SQLite driver (QSQLITE) is unavailable."_s;
        return false;
    }
    if (m_path.isEmpty()) {
        m_lastError = u"A local database path is required."_s;
        return false;
    }

    m_connection = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_connectionName);
    m_connection.setDatabaseName(m_path);
    m_connection.setConnectOptions(u"QSQLITE_BUSY_TIMEOUT=100"_s);
    if (!m_connection.open()) {
        fail(u"Open the local database"_s, m_connection.lastError());
        close();
        return false;
    }

    if (!run(u"PRAGMA foreign_keys = ON"_s) || !run(u"PRAGMA secure_delete = ON"_s) || !initializeSchema()) {
        close();
        return false;
    }
#ifdef Q_OS_UNIX
    if (m_path != u":memory:"_s
        && !QFile::setPermissions(m_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        m_lastError = u"Could not restrict access to the local database file."_s;
        close();
        return false;
    }
#endif
    return true;
}

void Database::close()
{
    if (!m_connection.isValid()) {
        return;
    }
    m_connection.close();
    m_connection = QSqlDatabase {};
    QSqlDatabase::removeDatabase(m_connectionName);
}

QString Database::lastError() const { return m_lastError; }

bool Database::ready()
{
    m_lastError.clear();
    if (m_connection.isOpen()) {
        return true;
    }
    m_lastError = u"The local database is closed."_s;
    return false;
}

bool Database::initializeSchema()
{
    QSqlQuery query(m_connection);
    if (!query.exec(u"PRAGMA application_id"_s) || !query.next()) {
        return fail(u"Read the database identity"_s, query.lastError());
    }
    const int applicationId = query.value(0).toInt();
    if (!query.exec(u"PRAGMA user_version"_s) || !query.next()) {
        return fail(u"Read the database schema version"_s, query.lastError());
    }
    const int version = query.value(0).toInt();
    query.finish();

    if (applicationId == ApplicationId && version == SchemaVersion) {
        return true;
    }
    if (applicationId == 0 && version == 0) {
        if (!query.exec(u"SELECT 1 FROM sqlite_master WHERE name NOT GLOB 'sqlite_*' LIMIT 1"_s)) {
            return fail(u"Inspect the local database schema"_s, query.lastError());
        }
        if (query.next()) {
            m_lastError = u"The local database belongs to another application."_s;
            return false;
        }
        if (query.lastError().isValid()) {
            return fail(u"Inspect the local database schema"_s, query.lastError());
        }
        query.finish();
    } else if (applicationId != ApplicationId || version < 1 || version > SchemaVersion) {
        m_lastError = u"The local database format is incompatible with this version of PhosChat."_s;
        return false;
    }

    if (!m_connection.transaction()) {
        return fail(u"Start the database migration"_s, m_connection.lastError());
    }
    const auto migrate = [this](const QStringList &statements, int version) {
        return std::all_of(statements.cbegin(), statements.cend(), [this](const auto &sql) {
            return run(sql);
        }) && run(u"PRAGMA user_version = %1"_s.arg(version));
    };
    const bool migrated
        = (version >= 1
              || (run(u"PRAGMA application_id = %1"_s.arg(ApplicationId)) && migrate(InitialSchema, 1)))
        && (version >= 2 || migrate(HistoryIndexes, 2))
        && (version >= 3 || migrate(ProviderConfiguration, 3));
    return finishTransaction(migrated);
}

bool Database::run(const QString &statement)
{
    QSqlQuery query(m_connection);
    return query.exec(statement) || fail(u"Configure the local database"_s, query.lastError());
}

bool Database::finishTransaction(bool commit)
{
    if (commit) {
        if (m_connection.commit()) {
            return true;
        }
        fail(u"Commit the local database transaction"_s, m_connection.lastError());
    }
    if (!m_connection.rollback()) {
        fail(u"Roll back the local database transaction"_s, m_connection.lastError());
    }
    return false;
}

bool Database::fail(const QString &operation, const QSqlError &error)
{
    m_lastError = error.text().isEmpty() ? operation + u" failed."_s : operation + u". "_s + error.text();
    return false;
}

QList<Account> Database::accounts()
{
    if (!ready()) {
        return {};
    }
    QSqlQuery query(m_connection);
    if (!query.exec(
            uR"sql(SELECT id, label, protocol, host, port, tls, nick, sasl_user, save_secret, auto_connect, provider_id
                              FROM accounts ORDER BY label COLLATE NOCASE, id)sql"_s)) {
        fail(u"Load accounts"_s, query.lastError());
        return {};
    }

    QList<Account> result;
    while (query.next()) {
        Account account;
        account.id = query.value(0).toString();
        account.label = query.value(1).toString();
        account.protocol = static_cast<Protocol>(query.value(2).toInt());
        account.host = query.value(3).toString();
        account.port = static_cast<quint16>(query.value(4).toUInt());
        account.tls = query.value(5).toBool();
        account.nick = query.value(6).toString();
        account.saslUser = query.value(7).toString();
        account.saveSecret = query.value(8).toBool();
        account.autoConnect = query.value(9).toBool();
        account.providerId = query.value(10).toString();
        result.append(std::move(account));
    }
    if (query.lastError().isValid()) {
        fail(u"Load accounts"_s, query.lastError());
        return {};
    }
    return result;
}

bool Database::saveAccount(const Account &account)
{
    if (!ready()) {
        return false;
    }
    if (account.id.isEmpty()) {
        m_lastError = u"An account identifier is required."_s;
        return false;
    }
    QSqlQuery query(m_connection);
    if (!query.prepare(uR"sql(INSERT INTO accounts
        (id, label, protocol, host, port, tls, nick, sasl_user, save_secret, auto_connect, provider_id)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET label = excluded.label, protocol = excluded.protocol,
        host = excluded.host, port = excluded.port, tls = excluded.tls, nick = excluded.nick,
        sasl_user = excluded.sasl_user, save_secret = excluded.save_secret, auto_connect = excluded.auto_connect,
        provider_id = excluded.provider_id)sql"_s)) {
        return fail(u"Prepare account persistence"_s, query.lastError());
    }
    query.addBindValue(account.id);
    query.addBindValue(nonNull(account.label));
    query.addBindValue(static_cast<int>(account.protocol));
    query.addBindValue(nonNull(account.host));
    query.addBindValue(account.port);
    query.addBindValue(account.tls);
    query.addBindValue(nonNull(account.nick));
    query.addBindValue(nonNull(account.saslUser));
    query.addBindValue(account.saveSecret);
    query.addBindValue(account.autoConnect);
    query.addBindValue(nonNull(account.providerId));
    return query.exec() || fail(u"Save the account"_s, query.lastError());
}

bool Database::removeAccount(const QString &id)
{
    if (!ready()) {
        return false;
    }
    QSqlQuery query(m_connection);
    if (!query.prepare(u"DELETE FROM accounts WHERE id = ?"_s)) {
        return fail(u"Prepare local account removal"_s, query.lastError());
    }
    query.addBindValue(id);
    return query.exec() || fail(u"Remove local account data"_s, query.lastError());
}

QList<Conversation> Database::conversations()
{
    if (!ready()) {
        return {};
    }
    QSqlQuery query(m_connection);
    if (!query.exec(uR"sql(SELECT account_id, remote_id, title, kind, joined, favorite, muted, auto_join,
                              custom_group, unread, updated FROM conversations
                              ORDER BY updated DESC, account_id, remote_id)sql"_s)) {
        fail(u"Load conversations"_s, query.lastError());
        return {};
    }

    QList<Conversation> result;
    while (query.next()) {
        Conversation conversation;
        conversation.id = { query.value(0).toString(), query.value(1).toString() };
        conversation.title = query.value(2).toString();
        conversation.kind = static_cast<ConversationKind>(query.value(3).toInt());
        conversation.joined = query.value(4).toBool();
        conversation.favorite = query.value(5).toBool();
        conversation.muted = query.value(6).toBool();
        conversation.autoJoin = query.value(7).toBool();
        conversation.group = query.value(8).toString();
        conversation.unread = query.value(9).toInt();
        conversation.updated = readTimestamp(query.value(10));
        result.append(std::move(conversation));
    }
    if (query.lastError().isValid()) {
        fail(u"Load conversations"_s, query.lastError());
        return {};
    }
    return result;
}

bool Database::saveConversation(const Conversation &conversation)
{
    return ready() && writeConversation(conversation);
}

bool Database::writeConversation(const Conversation &conversation)
{
    if (!conversation.id.isValid()) {
        m_lastError = u"Account and remote conversation identifiers are required."_s;
        return false;
    }
    QSqlQuery query(m_connection);
    if (!query.prepare(uR"sql(INSERT INTO conversations
        (account_id, remote_id, title, kind, joined, favorite, muted, auto_join, custom_group, unread, updated)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(account_id, remote_id) DO UPDATE SET title = excluded.title, kind = excluded.kind,
        joined = excluded.joined, favorite = excluded.favorite, muted = excluded.muted, auto_join = excluded.auto_join,
        custom_group = excluded.custom_group, unread = excluded.unread, updated = excluded.updated)sql"_s)) {
        return fail(u"Prepare conversation persistence"_s, query.lastError());
    }
    query.addBindValue(conversation.id.accountId);
    query.addBindValue(conversation.id.remoteId);
    query.addBindValue(nonNull(conversation.title));
    query.addBindValue(static_cast<int>(conversation.kind));
    query.addBindValue(conversation.joined);
    query.addBindValue(conversation.favorite);
    query.addBindValue(conversation.muted);
    query.addBindValue(conversation.autoJoin);
    query.addBindValue(nonNull(conversation.group));
    query.addBindValue(conversation.unread);
    query.addBindValue(
        conversation.updated.isValid() ? QVariant { conversation.updated.toMSecsSinceEpoch() } : QVariant {});
    return query.exec() || fail(u"Save the conversation"_s, query.lastError());
}

QList<Message> Database::messages(const ConversationId &conversation, int limit)
{
    if (!ready() || limit <= 0) {
        return {};
    }
    QSqlQuery query(m_connection);
    if (!query.prepare(uR"sql(SELECT message_id, sender, body, timestamp, outgoing, mention FROM messages
                              WHERE account_id = ? AND conversation_id = ?
                              ORDER BY timestamp DESC, row_id DESC LIMIT ?)sql"_s)) {
        fail(u"Prepare local history loading"_s, query.lastError());
        return {};
    }
    query.addBindValue(conversation.accountId);
    query.addBindValue(conversation.remoteId);
    query.addBindValue(limit);
    if (!query.exec()) {
        fail(u"Load local message history"_s, query.lastError());
        return {};
    }

    QList<Message> result;
    while (query.next()) {
        Message message;
        message.conversation = conversation;
        message.id = query.value(0).toString();
        message.sender = query.value(1).toString();
        message.text = query.value(2).toString();
        message.timestamp = readTimestamp(query.value(3));
        message.outgoing = query.value(4).toBool();
        message.mention = query.value(5).toBool();
        result.append(std::move(message));
    }
    if (query.lastError().isValid()) {
        fail(u"Load local message history"_s, query.lastError());
        return {};
    }
    std::reverse(result.begin(), result.end());
    return result;
}

Database::AppendResult Database::appendMessage(
    const Message &message, const Conversation &updatedConversation)
{
    if (!ready()) {
        return AppendResult::Error;
    }
    if (message.conversation != updatedConversation.id || message.id.isEmpty()
        || !message.timestamp.isValid()) {
        m_lastError = u"A message requires a matching conversation, identifier and timestamp."_s;
        return AppendResult::Error;
    }
    if (!m_connection.transaction()) {
        fail(u"Start the message transaction"_s, m_connection.lastError());
        return AppendResult::Error;
    }

    const auto result = writeConversation(updatedConversation) ? insertMessage(message) : AppendResult::Error;
    if (result != AppendResult::Inserted) {
        finishTransaction(false);
        return m_lastError.isEmpty() ? result : AppendResult::Error;
    }
    return finishTransaction(true) ? AppendResult::Inserted : AppendResult::Error;
}

Database::AppendResult Database::insertMessage(const Message &message)
{
    QSqlQuery query(m_connection);
    if (!query.prepare(uR"sql(INSERT INTO messages
        (account_id, conversation_id, message_id, sender, body, timestamp, outgoing, mention)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(account_id, conversation_id, message_id) DO NOTHING)sql"_s)) {
        fail(u"Prepare message persistence"_s, query.lastError());
        return AppendResult::Error;
    }
    query.addBindValue(message.conversation.accountId);
    query.addBindValue(message.conversation.remoteId);
    query.addBindValue(message.id);
    query.addBindValue(nonNull(message.sender));
    query.addBindValue(nonNull(message.text));
    query.addBindValue(message.timestamp.toMSecsSinceEpoch());
    query.addBindValue(message.outgoing);
    query.addBindValue(message.mention);
    if (!query.exec()) {
        fail(u"Save the local message"_s, query.lastError());
        return AppendResult::Error;
    }
    return query.numRowsAffected() == 0 ? AppendResult::Duplicate : AppendResult::Inserted;
}

} // namespace phoschat
