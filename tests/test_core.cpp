#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "security/credential_store.h"
#include "storage/database.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace phoschat;

namespace {

class TestProvider final : public IChatProvider {
public:
    TestProvider(Account account, QObject *parent)
        : IChatProvider(parent)
        , account_(std::move(account))
    {
    }

    Capabilities capabilities() const override
    {
        return Capability::Text | Capability::Channels | Capability::DirectMessages;
    }

    void connectAccount(const QByteArray &) override
    {
        ++connectCount;
        emit stateChanged(ConnectionState::Connected);
    }

    void disconnectAccount() override { emit stateChanged(ConnectionState::Disconnected); }

    bool join(const QString &target) override
    {
        joined.append(target);
        return true;
    }

    bool part(const QString &target) override
    {
        parted.append(target);
        return true;
    }

    bool sendText(const ConversationId &, const QString &text) override
    {
        sent = text;
        return true;
    }

    bool sendCommand(const ConversationId &, const QString &) override { return false; }
    QString canonicalTarget(const QString &target) const override { return target.toLower(); }

    void confirmJoin(const QString &target)
    {
        Conversation value;
        value.id = { account_.id, canonicalTarget(target) };
        value.title = target;
        value.joined = true;
        emit conversationChanged(value);
    }

    void receive(const Message &message) { emit messageReceived(message); }
    void loseConnection() { emit errorOccurred(u"Test disconnect"_s, true); }

    int connectCount {};
    QStringList joined;
    QStringList parted;
    QString sent;

private:
    Account account_;
};

Account testAccount(const QString &id = u"one"_s)
{
    Account account;
    account.id = id;
    account.label = id;
    account.host = u"irc.example.test"_s;
    account.nick = u"phoschat"_s;
    return account;
}

IChatProvider *makeProvider(const Account &account, QObject *parent)
{
    return new TestProvider(account, parent);
}

} // namespace

class CoreTest : public QObject {
    Q_OBJECT

private slots:
    void routesByAccountAndDeduplicates();
    void rejoinsAndKeepsHistoryAfterReconnect();
    void keepsNetworkIdentityAndDeletesLocalData();
    void cloudHistoryIsTransientAndReplayIsQuiet();
};

void CoreTest::cloudHistoryIsTransientAndReplayIsQuiet()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials, nullptr, makeProvider);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);

    auto account = testAccount(u"slack"_s);
    account.protocol = Protocol::Slack;
    account.providerId = u"1234.5678"_s;
    QVERIFY(accounts.saveAccount(account, QByteArrayLiteral("xoxp-test-only")));
    QCOMPARE(database.accounts().constFirst().providerId, account.providerId);
    accounts.connectAccount(account.id);
    auto *provider = static_cast<TestProvider *>(accounts.provider(account.id));
    QVERIFY(provider);
    const ConversationId id { account.id, u"workspace/user/channel"_s };
    provider->confirmJoin(id.remoteId);
    QVERIFY(messages.canSend(id));

    const auto now = QDateTime::currentDateTimeUtc();
    Message live { id, u"live"_s, u"Peer"_s, u"New text"_s, now };
    provider->receive(live);
    provider->receive(
        Message { id, u"replay"_s, u"Peer"_s, u"Earlier text"_s, now.addSecs(-60), false, false, true });
    QCOMPARE(conversations.conversation(id)->unread, 1);
    QCOMPARE(messages.history(id).size(), 2);
    QCOMPARE(messages.history(id).constFirst().id, u"replay"_s);
    QVERIFY(database.messages(id).isEmpty());

    live.text = u"Edited text"_s;
    provider->receive(live);
    QCOMPARE(conversations.conversation(id)->unread, 1);
    QCOMPARE(messages.history(id).constLast().text, u"Edited text"_s);
    QVERIFY(messages.send(id, u"/literal"_s));
    QCOMPARE(provider->sent, u"/literal"_s);
    provider->messageRemoved(id, live.id);
    QCOMPARE(messages.history(id).size(), 1);
    accounts.disconnectAccount(account.id);
    QVERIFY(messages.history(id).isEmpty());
}

void CoreTest::routesByAccountAndDeduplicates()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials, nullptr, makeProvider);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    QSignalSpy added(&messages, &MessageManager::messageAdded);

    const auto first = testAccount();
    const auto second = testAccount(u"two"_s);
    for (const auto &account : { first, second }) {
        QVERIFY(accounts.saveAccount(account));
        accounts.connectAccount(account.id);
        const auto id = conversations.openDirect(account.id, u"Peer"_s);
        QVERIFY(id.isValid());
        QCOMPARE(id.remoteId, u"peer"_s);
        auto *provider = static_cast<TestProvider *>(accounts.provider(account.id));
        const Message message { id, u"network-msgid"_s, u"Peer"_s, u"Hello"_s,
            QDateTime::currentDateTimeUtc(), false, false };
        provider->receive(message);
        provider->receive(message);
        QCOMPARE(conversations.conversation(id)->unread, 1);
        QCOMPARE(messages.history(id).size(), 1);
        QVERIFY(messages.send(id, u"//literal"_s));
        QCOMPARE(provider->sent, u"/literal"_s);
        conversations.markRead(id);
        QCOMPARE(conversations.conversation(id)->unread, 0);
    }
    QCOMPARE(added.size(), 2);
}

void CoreTest::rejoinsAndKeepsHistoryAfterReconnect()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials, nullptr, makeProvider);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);

    const auto account = testAccount();
    QVERIFY(accounts.saveAccount(account));
    accounts.connectAccount(account.id);
    auto *provider = static_cast<TestProvider *>(accounts.provider(account.id));
    const ConversationId id { account.id, u"#phoschat"_s };
    QVERIFY(conversations.joinChannel(account.id, u"#PhosChat"_s));
    QVERIFY(!conversations.conversation(id));
    QVERIFY(!messages.canSend(id));
    provider->confirmJoin(u"#PhosChat"_s);
    QVERIFY(messages.canSend(id));
    provider->receive(Message { id, u"history"_s, u"Peer"_s, u"Hello"_s, QDateTime::currentDateTimeUtc() });

    provider->loseConnection();
    QCOMPARE(accounts.state(account.id), ConnectionState::Reconnecting);
    QVERIFY(!messages.canSend(id));
    QTRY_COMPARE_WITH_TIMEOUT(provider->connectCount, 2, 5000);
    QCOMPARE(provider->joined, (QStringList { u"#PhosChat"_s, u"#PhosChat"_s }));
    QCOMPARE(messages.history(id).size(), 1);
    provider->confirmJoin(u"#PhosChat"_s);
    QVERIFY(conversations.partChannel(id));
    QCOMPARE(provider->parted, QStringList { u"#PhosChat"_s });
    QVERIFY(!conversations.conversation(id)->autoJoin);
}

void CoreTest::keepsNetworkIdentityAndDeletesLocalData()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(u"phoschat.sqlite3"_s);
    const auto account = testAccount();
    {
        Database database(path);
        QVERIFY(database.open());
        CredentialStore credentials;
        AccountManager accounts(database, credentials, nullptr, makeProvider);
        ConversationManager conversations(accounts, database);
        QVERIFY(accounts.saveAccount(account));
        auto other = account;
        other.host = u"different.example.test"_s;
        QVERIFY(!accounts.saveAccount(other));
        accounts.connectAccount(account.id);
        const auto id = conversations.openDirect(account.id, u"Peer"_s);
        QVERIFY(id.isValid());
        conversations.setFavorite(id, true);
        conversations.setMuted(id, true);
        conversations.setGroup(id, u"Friends"_s);
    }
    {
        Database database(path);
        QVERIFY(database.open());
        CredentialStore credentials;
        AccountManager accounts(database, credentials, nullptr, makeProvider);
        ConversationManager conversations(accounts, database);
        const ConversationId id { account.id, u"peer"_s };
        QCOMPARE(accounts.accounts().front().host, account.host);
        QCOMPARE(conversations.conversation(id)->group, u"Friends"_s);
        QVERIFY(conversations.conversation(id)->favorite);
        QVERIFY(conversations.conversation(id)->muted);
        accounts.removeAccount(account.id);
        QVERIFY(!conversations.conversation(id));
        QVERIFY(database.accounts().isEmpty());
        QVERIFY(database.conversations().isEmpty());
    }
}

QTEST_GUILESS_MAIN(CoreTest)
#include "test_core.moc"
