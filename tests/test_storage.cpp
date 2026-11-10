#include "security/credential_store.h"
#include "storage/database.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUuid>

namespace {

using namespace phoschat;
using namespace Qt::StringLiterals;
using Append = Database::AppendResult;

Account exampleAccount(const QString &id)
{
    Account account;
    account.id = id;
    account.label = u"Example network"_s;
    account.host = u"irc.example.test"_s;
    account.nick = u"tester"_s;
    account.saslUser = u"tester-account"_s;
    account.saveSecret = true;
    account.autoConnect = true;
    return account;
}

Conversation exampleChannel(const QString &accountId, const QString &channel = u"#sample"_s)
{
    Conversation conversation;
    conversation.id = { accountId, channel };
    conversation.title = channel;
    conversation.favorite = true;
    conversation.muted = true;
    conversation.autoJoin = true;
    conversation.group = u"Work"_s;
    return conversation;
}

Message exampleMessage(const ConversationId &conversation, const QString &id, qint64 seconds = 1'700'000'000)
{
    Message message;
    message.conversation = conversation;
    message.id = id;
    message.sender = u"sender"_s;
    message.text = u"A <b>plain-text</b> message with Ñ and 🌿."_s;
    message.timestamp = QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::utc());
    message.mention = true;
    return message;
}

bool executeFixtureStatements(const QString &path, const QStringList &statements)
{
    const QString connectionName = QUuid::createUuid().toString();
    bool success = false;
    {
        auto connection = QSqlDatabase::addDatabase(u"QSQLITE"_s, connectionName);
        connection.setDatabaseName(path);
        success = connection.open();
        for (const auto &statement : statements) {
            QSqlQuery query(connection);
            if (success && !query.exec(statement)) {
                qWarning() << query.lastError().text();
                success = false;
            }
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return success;
}

class StorageTest final : public QObject {
    Q_OBJECT

private slots:
    void persistsAccountConversationAndHistory();
    void rollsBackDuplicateAndInvalidInsert();
    void scopesIdentifiersAndCascadesAccountDeletion();
    void ordersLimitedHistoryChronologically();
    void upgradesOldSchemaWithoutLosingHistory();
    void rejectsUnrelatedAndFutureDatabases();
    void unavailableKeychainReturnsAsyncError();
};

void StorageTest::persistsAccountConversationAndHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(u"phoschat.sqlite"_s);
    auto account = exampleAccount(u"account-a"_s);
    auto conversation = exampleChannel(account.id);
    const auto message = exampleMessage(conversation.id, u"m1"_s);
    conversation.updated = message.timestamp;
    conversation.unread = 1;

    {
        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY2(db.saveAccount(account), qPrintable(db.lastError()));
        QCOMPARE(db.appendMessage(message, conversation), Append::Inserted);
        account.label = u"Updated network"_s;
        QVERIFY2(db.saveAccount(account), qPrintable(db.lastError()));
    }

    Database db(path);
    QVERIFY2(db.open(), qPrintable(db.lastError()));
    const auto accounts = db.accounts();
    QCOMPARE(accounts.size(), 1);
    QCOMPARE(accounts.first().id, account.id);
    QCOMPARE(accounts.first().label, account.label);
    QCOMPARE(accounts.first().port, account.port);
    QCOMPARE(accounts.first().saslUser, account.saslUser);
    QVERIFY(accounts.first().saveSecret);
    QVERIFY(accounts.first().autoConnect);

    const auto conversations = db.conversations();
    QCOMPARE(conversations.size(), 1);
    QVERIFY(conversations.first().id == conversation.id);
    QCOMPARE(conversations.first().unread, 1);
    QCOMPARE(conversations.first().updated, message.timestamp);
    QCOMPARE(conversations.first().group, conversation.group);
    QVERIFY(conversations.first().favorite);
    QVERIFY(conversations.first().muted);
    QVERIFY(conversations.first().autoJoin);

    const auto history = db.messages(conversation.id);
    QCOMPARE(history.size(), 1);
    QCOMPARE(history.first().id, message.id);
    QCOMPARE(history.first().sender, message.sender);
    QCOMPARE(history.first().text, message.text);
    QCOMPARE(history.first().timestamp, message.timestamp);
    QVERIFY(history.first().mention);
}

void StorageTest::rollsBackDuplicateAndInvalidInsert()
{
    Database db(u":memory:"_s);
    QVERIFY2(db.open(), qPrintable(db.lastError()));
    const auto account = exampleAccount(u"account-a"_s);
    QVERIFY2(db.saveAccount(account), qPrintable(db.lastError()));
    auto conversation = exampleChannel(account.id);
    const auto message = exampleMessage(conversation.id, u"m1"_s);
    conversation.unread = 1;
    QCOMPARE(db.appendMessage(message, conversation), Append::Inserted);

    conversation.unread = 2;
    conversation.favorite = false;
    auto replay = message;
    replay.text = u"Duplicate payload"_s;
    QCOMPARE(db.appendMessage(replay, conversation), Append::Duplicate);
    QVERIFY(db.lastError().isEmpty());

    const auto orphan = exampleChannel(u"no-such-account"_s);
    QCOMPARE(db.appendMessage(exampleMessage(orphan.id, u"m1"_s), orphan), Append::Error);
    QVERIFY(!db.lastError().isEmpty());
    QCOMPARE(db.appendMessage(exampleMessage(orphan.id, u"m2"_s), conversation), Append::Error);
    QVERIFY(!db.lastError().isEmpty());

    const auto conversations = db.conversations();
    QCOMPARE(conversations.size(), 1);
    QCOMPARE(conversations.first().unread, 1);
    QVERIFY(conversations.first().favorite);
    const auto history = db.messages(conversation.id);
    QCOMPARE(history.size(), 1);
    QCOMPARE(history.first().text, message.text);
}

void StorageTest::scopesIdentifiersAndCascadesAccountDeletion()
{
    Database db(u":memory:"_s);
    QVERIFY2(db.open(), qPrintable(db.lastError()));
    QVERIFY(db.saveAccount(exampleAccount(u"account-a"_s)));
    QVERIFY(db.saveAccount(exampleAccount(u"account-b"_s)));
    const auto channelA = exampleChannel(u"account-a"_s);
    const auto secondChannelA = exampleChannel(u"account-a"_s, u"#another"_s);
    const auto channelB = exampleChannel(u"account-b"_s);
    const auto messageId = u"same-remote-id"_s;
    QCOMPARE(db.appendMessage(exampleMessage(channelA.id, messageId), channelA), Append::Inserted);
    QCOMPARE(
        db.appendMessage(exampleMessage(secondChannelA.id, messageId), secondChannelA), Append::Inserted);
    QCOMPARE(db.appendMessage(exampleMessage(channelB.id, messageId), channelB), Append::Inserted);

    QVERIFY2(db.removeAccount(channelA.id.accountId), qPrintable(db.lastError()));
    QCOMPARE(db.accounts().size(), 1);
    QCOMPARE(db.conversations().size(), 1);
    QVERIFY(db.messages(channelA.id).isEmpty());
    QVERIFY(db.messages(secondChannelA.id).isEmpty());
    QCOMPARE(db.messages(channelB.id).size(), 1);
}

void StorageTest::ordersLimitedHistoryChronologically()
{
    Database db(u":memory:"_s);
    QVERIFY2(db.open(), qPrintable(db.lastError()));
    const auto account = exampleAccount(u"account-a"_s);
    QVERIFY(db.saveAccount(account));
    const auto conversation = exampleChannel(account.id);
    const auto id = conversation.id;

    QCOMPARE(db.appendMessage(exampleMessage(id, u"z"_s, 30), conversation), Append::Inserted);
    QCOMPARE(db.appendMessage(exampleMessage(id, u"m"_s, 20), conversation), Append::Inserted);
    QCOMPARE(db.appendMessage(exampleMessage(id, u"a"_s, 30), conversation), Append::Inserted);
    QCOMPARE(db.appendMessage(exampleMessage(id, u"n"_s, 10), conversation), Append::Inserted);

    const auto history = db.messages(id, 3);
    QCOMPARE(history.size(), 3);
    QCOMPARE(history.at(0).id, u"m"_s);
    QCOMPARE(history.at(1).id, u"z"_s);
    QCOMPARE(history.at(2).id, u"a"_s);
    QVERIFY(db.messages(id, 0).isEmpty());
}

void StorageTest::upgradesOldSchemaWithoutLosingHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(u"migration.sqlite"_s);
    const auto account = exampleAccount(u"account-a"_s);
    const auto conversation = exampleChannel(account.id);
    const auto message = exampleMessage(conversation.id, u"existing"_s);
    {
        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.saveAccount(account));
        QCOMPARE(db.appendMessage(message, conversation), Append::Inserted);
    }
    QVERIFY(executeFixtureStatements(path,
        { u"DROP INDEX messages_by_conversation_time"_s, u"ALTER TABLE accounts DROP COLUMN provider_id"_s,
            u"PRAGMA user_version = 1"_s }));
    {
        Database db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        const auto history = db.messages(conversation.id);
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().id, message.id);
    }
    QVERIFY(executeFixtureStatements(path, { u"DROP INDEX messages_by_conversation_time"_s }));
}

void StorageTest::rejectsUnrelatedAndFutureDatabases()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto foreignPath = directory.filePath(u"foreign.sqlite"_s);
    QVERIFY(
        executeFixtureStatements(foreignPath, { u"CREATE TABLE foreign_app (id INTEGER PRIMARY KEY)"_s }));
    Database foreignDb(foreignPath);
    QVERIFY(!foreignDb.open());
    QVERIFY(foreignDb.lastError().contains(u"another application"_s));

    const auto futurePath = directory.filePath(u"future.sqlite"_s);
    {
        Database db(futurePath);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
    }
    QVERIFY(executeFixtureStatements(futurePath, { u"PRAGMA user_version = 999"_s }));
    Database futureDb(futurePath);
    QVERIFY(!futureDb.open());
    QVERIFY(futureDb.lastError().contains(u"incompatible"_s));
}

void StorageTest::unavailableKeychainReturnsAsyncError()
{
    CredentialStore store;
    if (store.persistentAvailable()) {
        QSKIP("This build includes QtKeychain. No desktop keychain entries are modified during tests.");
    }
    const auto id = u"test-account"_s;
    int completions = 0;
    store.read(id, [&](const QByteArray &secret, const QString &error) {
        QVERIFY(secret.isEmpty());
        QVERIFY(!error.isEmpty());
        ++completions;
    });
    store.write(id, QByteArrayLiteral("synthetic-test-secret"), [&](const QString &error) {
        QVERIFY(!error.isEmpty());
        ++completions;
    });
    store.remove(id, [&](const QString &error) {
        QVERIFY(!error.isEmpty());
        ++completions;
    });
    QCOMPARE(completions, 0);
    QTRY_COMPARE(completions, 3);
}

} // namespace

QTEST_GUILESS_MAIN(StorageTest)
#include "test_storage.moc"
