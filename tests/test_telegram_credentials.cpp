#include "core/account_manager.h"
#include "providers/telegram/telegram_provider.h"
#include "providers/telegram/telegram_transport.h"
#include "security/credential_store.h"
#include "security/telegram_app_credentials.h"
#include "security/telegram_credentials.h"
#include "storage/database.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace phoschat;
using namespace Qt::StringLiterals;

namespace {
const QByteArray ApiHash = QByteArrayLiteral("0123456789abcdef0123456789abcdef");

Account telegramAccount(bool saved = true)
{
    Account account;
    account.id = u"credential-test"_s;
    account.label = u"Telegram test"_s;
    account.protocol = Protocol::Telegram;
    account.providerId = u"1001"_s;
    account.saveSecret = saved;
    return account;
}

class MemoryStore final : public CredentialStore {
public:
    bool persistentAvailable() const override { return true; }
    void read(const QString &, ReadCallback callback) override
    {
        callback(
            failReads ? QByteArray {} : value, failReads ? u"Simulated keychain read failure"_s : QString {});
    }
    void write(const QString &, const QByteArray &secret, WriteCallback callback) override
    {
        ++writes;
        if (holdWrites) {
            pendingValue = secret;
            pendingWrite = std::move(callback);
            return;
        }
        if (!failWrites)
            value = secret;
        callback(failWrites ? u"Simulated keychain failure"_s : QString {});
    }
    void remove(const QString &, WriteCallback callback) override
    {
        if (failRemoves) {
            callback(u"Simulated keychain deletion failure"_s);
            return;
        }
        value.clear();
        callback({});
    }
    QByteArray value;
    int writes {};
    bool failWrites {};
    bool failReads {};
    bool failRemoves {};
    bool holdWrites {};
    QByteArray pendingValue;
    WriteCallback pendingWrite;
};

class CaptureProvider final : public IChatProvider {
public:
    using IChatProvider::IChatProvider;
    Capabilities capabilities() const override { return {}; }
    void connectAccount(const QByteArray &secret) override
    {
        connected = secret;
        emit stateChanged(ConnectionState::Connected);
    }
    void disconnectAccount() override { }
    bool join(const QString &) override { return false; }
    bool part(const QString &) override { return false; }
    bool sendText(const ConversationId &, const QString &) override { return false; }
    bool sendCommand(const ConversationId &, const QString &) override { return false; }
    QString canonicalTarget(const QString &target) const override { return target; }
    QByteArray connected;
};

class Transport final : public TelegramTransport {
public:
    bool isAvailable() const override { return true; }
    int createClient(const QString &, std::shared_ptr<QTemporaryDir>) override { return 1; }
    void send(int, const QJsonObject &request) override { requests.append(request); }
    void closeClient(int) override { }
    void forgetProfile(const QString &) override { }
    void auth(const QString &state)
    {
        emit received(1,
            { { u"@type"_s, u"updateAuthorizationState"_s },
                { u"authorization_state"_s, QJsonObject { { u"@type"_s, state } } } });
    }
    void reply(QJsonObject event)
    {
        event.insert(u"@extra"_s, requests.last().value(u"@extra"_s));
        emit received(1, event);
    }
    QByteArray key() const
    {
        return QByteArray::fromBase64(
            requests.last().value(u"database_encryption_key"_s).toString().toLatin1());
    }
    QString type() const { return requests.last().value(u"@type"_s).toString(); }
    QList<QJsonObject> requests;
};
} // namespace

class TelegramCredentialsTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void generatesIndependentKeysAndValidatesEnvelope();
    void persistsBeforeOpeningAndKeepsKeyAcrossStarts();
    void migrationRequiresSuccessfulKeychainWrite();
    void rotatesLegacyKeyBeforeSignInAndRecoversInterruptedRotation();
    void keyRotationFailureRetainsRecoveryKeys();
    void bundledIdentityUsesIndependentSessionSecret();
    void switchingSessionPersistenceKeepsApplicationIdentity();
    void reconnectDuringMigrationWriteDoesNotStall();
    void failedPersistenceRemovalKeepsOriginalSession();
};

void TelegramCredentialsTest::generatesIndependentKeysAndValidatesEnvelope()
{
    const auto first = newTelegramCredentials(ApiHash);
    const auto second = newTelegramCredentials(ApiHash);
    QCOMPARE(first.databaseKey.size(), 32);
    QVERIFY(first.databaseKey != second.databaseKey);
    const auto encoded = encodeTelegramCredentials(first);
    const auto restored = decodeTelegramCredentials(encoded);
    QVERIFY(restored);
    QCOMPARE(restored->databaseKey, first.databaseKey);
    QCOMPARE(restored->apiHash, ApiHash);
    QVERIFY(restored->legacyDatabaseKey.isEmpty());
    QVERIFY(!decodeTelegramCredentials(ApiHash));
    QVERIFY(!decodeTelegramCredentials("{\"phoschat_telegram\":2}"));
    auto object = QJsonDocument::fromJson(encoded).object();
    object.insert(u"database_key"_s, u"invalid!!!"_s);
    QVERIFY(!decodeTelegramCredentials(QJsonDocument(object).toJson()));
    const auto legacy = migrateTelegramCredentials(ApiHash, u"one"_s);
    QCOMPARE(legacy.legacyDatabaseKey.size(), 32);
    QVERIFY(legacy.databaseKey != legacy.legacyDatabaseKey);
    QVERIFY(legacy.legacyDatabaseKey != migrateTelegramCredentials(ApiHash, u"two"_s).legacyDatabaseKey);
}

void TelegramCredentialsTest::persistsBeforeOpeningAndKeepsKeyAcrossStarts()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    MemoryStore store;
    CaptureProvider *provider = nullptr;
    AccountManager accounts(database, store, nullptr,
        [&provider](const Account &, QObject *owner) { return provider = new CaptureProvider(owner); });
    const auto account = telegramAccount();
    QVERIFY(accounts.saveAccount(account, ApiHash));
    const auto saved = decodeTelegramCredentials(store.value);
    QVERIFY(saved);
    QCOMPARE(saved->apiHash, ApiHash);
    QVERIFY(!database.accounts().isEmpty());
    accounts.connectAccount(account.id);
    QCOMPARE(provider->connected, store.value);
    const auto first = provider->connected;
    accounts.disconnectAccount(account.id);
    accounts.connectAccount(account.id);
    QCOMPARE(provider->connected, first);
    accounts.removeAccount(account.id);
    QVERIFY(store.value.isEmpty());
}

void TelegramCredentialsTest::migrationRequiresSuccessfulKeychainWrite()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    const auto account = telegramAccount();
    QVERIFY(database.saveAccount(account));
    MemoryStore store;
    store.value = ApiHash;
    store.failWrites = true;
    CaptureProvider *provider = nullptr;
    AccountManager accounts(database, store, nullptr,
        [&provider](const Account &, QObject *owner) { return provider = new CaptureProvider(owner); });
    accounts.connectAccount(account.id);
    QCOMPARE(accounts.state(account.id), ConnectionState::Error);
    QVERIFY(provider->connected.isEmpty());
    QCOMPARE(store.value, ApiHash);
    store.failWrites = false;
    accounts.connectAccount(account.id);
    const auto migrated = decodeTelegramCredentials(store.value);
    QVERIFY(migrated);
    QVERIFY(!migrated->legacyDatabaseKey.isEmpty());
    QCOMPARE(provider->connected, store.value);
}

void TelegramCredentialsTest::rotatesLegacyKeyBeforeSignInAndRecoversInterruptedRotation()
{
    const auto credentials = migrateTelegramCredentials(ApiHash, telegramAccount().id);
    const auto envelope = encodeTelegramCredentials(credentials);
    Transport transport;
    TelegramProvider provider(telegramAccount(), transport);
    QSignalSpy updated(&provider, &IChatProvider::credentialsUpdated);
    QSignalSpy prompts(&provider, &IChatProvider::authenticationRequired);
    provider.connectAccount(envelope);
    transport.auth(u"authorizationStateWaitTdlibParameters"_s);
    QCOMPARE(transport.key(), credentials.databaseKey);
    transport.reply({ { u"@type"_s, u"error"_s }, { u"code"_s, 401 } });
    QCOMPARE(transport.key(), credentials.legacyDatabaseKey);
    transport.auth(u"authorizationStateWaitPhoneNumber"_s);
    QCOMPARE(prompts.size(), 1);
    transport.reply({ { u"@type"_s, u"ok"_s } });
    QCOMPARE(transport.type(), u"setTdlibParameters"_s);
    transport.auth(u"authorizationStateReady"_s);
    QCOMPARE(transport.type(), u"setDatabaseEncryptionKey"_s);
    QCOMPARE(QByteArray::fromBase64(
                 transport.requests.last().value(u"new_encryption_key"_s).toString().toLatin1()),
        credentials.databaseKey);
    QVERIFY(updated.isEmpty());
    transport.reply({ { u"@type"_s, u"ok"_s } });
    QCOMPARE(updated.size(), 1);
    const auto completed = decodeTelegramCredentials(updated.last().at(0).toByteArray());
    QVERIFY(completed);
    QVERIFY(completed->legacyDatabaseKey.isEmpty());
    QCOMPARE(transport.type(), u"getMe"_s);

    // Crash after rekey but before cleanup reaches the keychain: both keys are
    // still saved, the new key opens directly and no second rotation is needed.
    provider.disconnectAccount();
    transport.requests.clear();
    provider.connectAccount(envelope);
    transport.auth(u"authorizationStateWaitTdlibParameters"_s);
    QCOMPARE(transport.key(), credentials.databaseKey);
    transport.auth(u"authorizationStateReady"_s);
    transport.reply({ { u"@type"_s, u"ok"_s } });
    QCOMPARE(updated.size(), 2);
    QCOMPARE(transport.type(), u"getMe"_s);
}

void TelegramCredentialsTest::keyRotationFailureRetainsRecoveryKeys()
{
    const auto credentials = migrateTelegramCredentials(ApiHash, telegramAccount().id);
    const auto envelope = encodeTelegramCredentials(credentials);
    Transport transport;
    TelegramProvider provider(telegramAccount(), transport);
    QSignalSpy updated(&provider, &IChatProvider::credentialsUpdated);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);
    provider.connectAccount(envelope);
    transport.auth(u"authorizationStateWaitTdlibParameters"_s);
    transport.reply({ { u"@type"_s, u"error"_s }, { u"code"_s, 401 } });
    transport.reply({ { u"@type"_s, u"ok"_s } });
    transport.auth(u"authorizationStateReady"_s);
    transport.reply({ { u"@type"_s, u"error"_s }, { u"code"_s, 500 } });
    QCOMPARE(errors.size(), 1);
    QVERIFY(updated.isEmpty());
    provider.connectAccount(envelope);
    transport.auth(u"authorizationStateWaitTdlibParameters"_s);
    QCOMPARE(transport.key(), credentials.databaseKey);
    transport.reply({ { u"@type"_s, u"error"_s }, { u"code"_s, 401 } });
    QCOMPARE(transport.key(), credentials.legacyDatabaseKey);
}

void TelegramCredentialsTest::bundledIdentityUsesIndependentSessionSecret()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    MemoryStore store;
    CaptureProvider *capture = nullptr;
    AccountManager accounts(database, store, nullptr,
        [&capture](const Account &, QObject *owner) { return capture = new CaptureProvider(owner); });
    auto account = telegramAccount();
    account.providerId.clear();
    const bool bundled = bundledTelegramCredentialsAvailable();
    QCOMPARE(accounts.saveAccount(account), bundled);
    if (!bundled)
        return;
    const auto session = decodeTelegramCredentials(store.value);
    QVERIFY(session);
    QVERIFY(session->apiHash.isEmpty());
    accounts.connectAccount(account.id);
    QCOMPARE(capture->connected, store.value);
    Transport transport;
    TelegramProvider provider(account, transport);
    provider.connectAccount(store.value);
    transport.auth(u"authorizationStateWaitTdlibParameters"_s);
    QCOMPARE(transport.requests.last().value(u"api_id"_s).toInt(), bundledTelegramApiId());
    QCOMPARE(transport.requests.last().value(u"api_hash"_s).toString().toLatin1(), bundledTelegramApiHash());
    QCOMPARE(transport.key(), session->databaseKey);

    // A session-only official account reopened after a restart can sign in
    // directly, without opening Accounts and pressing Save again.
    Database restartedDatabase(u":memory:"_s);
    QVERIFY(restartedDatabase.open());
    account.saveSecret = false;
    QVERIFY(restartedDatabase.saveAccount(account));
    AccountManager restarted(restartedDatabase, store, nullptr,
        [&capture](const Account &, QObject *owner) { return capture = new CaptureProvider(owner); });
    restarted.connectAccount(account.id);
    QCOMPARE(restarted.state(account.id), ConnectionState::Connected);
    const auto temporary = decodeTelegramCredentials(capture->connected);
    QVERIFY(temporary);
    QVERIFY(temporary->databaseKey != session->databaseKey);
}

void TelegramCredentialsTest::switchingSessionPersistenceKeepsApplicationIdentity()
{
    for (const bool bundled : { false, true }) {
        if (bundled && !bundledTelegramCredentialsAvailable())
            continue;
        Database database(u":memory:"_s);
        QVERIFY(database.open());
        MemoryStore store;
        CaptureProvider *provider = nullptr;
        AccountManager accounts(database, store, nullptr,
            [&provider](const Account &, QObject *owner) { return provider = new CaptureProvider(owner); });
        auto account = telegramAccount();
        if (bundled)
            account.providerId.clear();
        QVERIFY(accounts.saveAccount(account, bundled ? QByteArray {} : ApiHash));
        const auto previous = decodeTelegramCredentials(store.value);
        QVERIFY(previous);
        account.saveSecret = false;
        QVERIFY(accounts.saveAccount(account));
        QVERIFY(store.value.isEmpty());
        accounts.connectAccount(account.id);
        QCOMPARE(accounts.state(account.id), ConnectionState::Connected);
        const auto temporary = decodeTelegramCredentials(provider->connected);
        QVERIFY(temporary);
        QCOMPARE(temporary->apiHash, previous->apiHash);
        QVERIFY(temporary->databaseKey != previous->databaseKey);
        account.saveSecret = true;
        QVERIFY(accounts.saveAccount(account));
        const auto persistent = decodeTelegramCredentials(store.value);
        QVERIFY(persistent);
        QCOMPARE(persistent->apiHash, temporary->apiHash);
        accounts.connectAccount(account.id);
        QCOMPARE(provider->connected, store.value);
    }
}

void TelegramCredentialsTest::reconnectDuringMigrationWriteDoesNotStall()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    const auto account = telegramAccount();
    QVERIFY(database.saveAccount(account));
    MemoryStore store;
    store.value = ApiHash;
    store.holdWrites = true;
    CaptureProvider *provider = nullptr;
    AccountManager accounts(database, store, nullptr,
        [&provider](const Account &, QObject *owner) { return provider = new CaptureProvider(owner); });
    accounts.connectAccount(account.id);
    QVERIFY(store.pendingWrite);
    QVERIFY(provider->connected.isEmpty());
    accounts.disconnectAccount(account.id);
    accounts.connectAccount(account.id);
    QCOMPARE(accounts.state(account.id), ConnectionState::Connecting);
    store.value = store.pendingValue;
    store.holdWrites = false;
    std::exchange(store.pendingWrite, {})({});
    QCOMPARE(accounts.state(account.id), ConnectionState::Connected);
    QCOMPARE(provider->connected, store.value);
}

void TelegramCredentialsTest::failedPersistenceRemovalKeepsOriginalSession()
{
    for (const bool failRead : { false, true }) {
        Database database(u":memory:"_s);
        QVERIFY(database.open());
        MemoryStore store;
        CaptureProvider *provider = nullptr;
        AccountManager accounts(database, store, nullptr,
            [&provider](const Account &, QObject *owner) { return provider = new CaptureProvider(owner); });
        auto account = telegramAccount();
        QVERIFY(accounts.saveAccount(account, ApiHash));
        const auto original = store.value;
        store.failReads = failRead;
        store.failRemoves = !failRead;
        account.saveSecret = false;
        QVERIFY(accounts.saveAccount(account));
        QVERIFY(accounts.accounts().first().saveSecret);
        QVERIFY(database.accounts().first().saveSecret);
        QCOMPARE(store.value, original);
        store.failReads = false;
        store.failRemoves = false;
        accounts.connectAccount(account.id);
        QCOMPARE(accounts.state(account.id), ConnectionState::Connected);
        QCOMPARE(provider->connected, original);
    }
}

QTEST_GUILESS_MAIN(TelegramCredentialsTest)
#include "test_telegram_credentials.moc"
