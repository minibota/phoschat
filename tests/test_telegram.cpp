#include "providers/telegram/telegram_provider.h"
#include "providers/telegram/telegram_transport.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace phoschat;
using namespace Qt::StringLiterals;

namespace {

class FakeTelegramTransport final : public TelegramTransport {
public:
    bool isAvailable() const override { return true; }

    int createClient(const QString &profile, std::shared_ptr<QTemporaryDir> temporary) override
    {
        this->profile = profile;
        this->temporary = std::move(temporary);
        return ++client;
    }

    void send(int clientId, const QJsonObject &request) override
    {
        QCOMPARE(clientId, client);
        requests.append(request);
    }

    void closeClient(int id) override
    {
        QCOMPARE(id, client);
        ++closed;
        temporary.reset();
    }

    void forgetProfile(const QString &) override { }

    void publish(const QJsonObject &event) { emit received(client, event); }

    void reply(const QJsonObject &request, QJsonObject event)
    {
        event.insert(u"@extra"_s, request.value(u"@extra"_s));
        publish(event);
    }

    void auth(const QString &state)
    {
        publish({ { u"@type"_s, u"updateAuthorizationState"_s },
            { u"authorization_state"_s, QJsonObject { { u"@type"_s, state } } } });
    }

    void network(const QString &state)
    {
        publish({ { u"@type"_s, u"updateConnectionState"_s },
            { u"state"_s, QJsonObject { { u"@type"_s, state } } } });
    }

    QJsonObject last() const { return requests.last(); }
    QString lastType() const { return last().value(u"@type"_s).toString(); }

    void connected(qint64 owner)
    {
        network(u"connectionStateReady"_s);
        auth(u"authorizationStateReady"_s);
        QCOMPARE(lastType(), u"getMe"_s);
        reply(last(), { { u"@type"_s, u"user"_s }, { u"id"_s, owner }, { u"first_name"_s, u"Owner"_s } });
    }

    int client {};
    int closed {};
    QString profile;
    QList<QJsonObject> requests;
    std::shared_ptr<QTemporaryDir> temporary;
};

Account account()
{
    Account value;
    value.id = u"test-account"_s;
    value.protocol = Protocol::Telegram;
    value.providerId = u"1001"_s;
    return value;
}

const QByteArray TestApiHash = QByteArrayLiteral("00000000000000000000000000000000");

QJsonObject chat(qint64 id, const QString &type, const QString &title, bool channel = false)
{
    return { { u"@type"_s, u"updateNewChat"_s },
        { u"chat"_s,
            QJsonObject { { u"id"_s, id }, { u"title"_s, title },
                { u"type"_s, QJsonObject { { u"@type"_s, type }, { u"is_channel"_s, channel } } } } } };
}

QJsonObject message(qint64 chat, qint64 id, const QString &text)
{
    return { { u"id"_s, id }, { u"chat_id"_s, chat }, { u"date"_s, 1700000000 },
        { u"sender_id"_s, QJsonObject { { u"@type"_s, u"messageSenderUser"_s }, { u"user_id"_s, 7 } } },
        { u"content"_s,
            QJsonObject {
                { u"@type"_s, u"messageText"_s }, { u"text"_s, QJsonObject { { u"text"_s, text } } } } } };
}

} // namespace

class TelegramTest final : public QObject {
    Q_OBJECT

private slots:
    void authenticatesAndSafeguardsTemporaryProfile();
    void mapsAndFiltersCloudText();
    void loadsNativeLibraryWhenInstalled();
};

void TelegramTest::authenticatesAndSafeguardsTemporaryProfile()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy challenge(&provider, &IChatProvider::authenticationRequired);
    QSignalSpy states(&provider, &IChatProvider::stateChanged);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);
    provider.connectAccount("invalid");
    QCOMPARE(errors.size(), 1);
    QVERIFY(transport.requests.isEmpty());

    provider.connectAccount(TestApiHash);
    QCOMPARE(transport.lastType(), u"getAuthorizationState"_s);
    QVERIFY(QFileInfo::exists(transport.profile));
    transport.auth(u"authorizationStateWaitTdlibParameters"_s);
    QCOMPARE(transport.lastType(), u"setTdlibParameters"_s);
    const auto parameters = transport.last();
    QCOMPARE(parameters.value(u"api_id"_s).toInt(), 1001);
    QCOMPARE(parameters.value(u"api_hash"_s).toString().toLatin1(), TestApiHash);
    QCOMPARE(
        QByteArray::fromBase64(parameters.value(u"database_encryption_key"_s).toString().toLatin1()).size(),
        32);
    QVERIFY(!parameters.value(u"use_message_database"_s).toBool());
    QVERIFY(!parameters.value(u"use_secret_chats"_s).toBool());

    transport.auth(u"authorizationStateWaitPhoneNumber"_s);
    QCOMPARE(challenge.last().at(0).value<AuthenticationKind>(), AuthenticationKind::Phone);
    provider.submitAuthentication(AuthenticationKind::Phone, u"+34 612 345 678"_s);
    QCOMPARE(transport.lastType(), u"setAuthenticationPhoneNumber"_s);
    QCOMPARE(transport.last().value(u"phone_number"_s).toString(), u"+34612345678"_s);

    transport.auth(u"authorizationStateWaitEmailAddress"_s);
    QCOMPARE(challenge.last().at(0).value<AuthenticationKind>(), AuthenticationKind::EmailAddress);
    provider.submitAuthentication(AuthenticationKind::EmailAddress, u"person@example.invalid"_s);
    QCOMPARE(transport.lastType(), u"setAuthenticationEmailAddress"_s);
    transport.auth(u"authorizationStateWaitEmailCode"_s);
    provider.submitAuthentication(AuthenticationKind::EmailCode, u"009875"_s);
    QCOMPARE(transport.lastType(), u"checkAuthenticationEmailCode"_s);
    QCOMPARE(transport.last().value(u"code"_s).toObject().value(u"code"_s).toString(), u"009875"_s);

    transport.auth(u"authorizationStateWaitCode"_s);
    provider.submitAuthentication(AuthenticationKind::Code, u"00123"_s);
    QCOMPARE(transport.lastType(), u"checkAuthenticationCode"_s);
    transport.reply(transport.last(),
        { { u"@type"_s, u"error"_s }, { u"code"_s, 400 }, { u"message"_s, u"sensitive remote string"_s } });
    QVERIFY(!challenge.last().at(1).toString().contains(u"sensitive remote string"));
    transport.auth(u"authorizationStateWaitPassword"_s);
    provider.submitAuthentication(AuthenticationKind::Password, u" spaces are significant "_s);
    QCOMPARE(transport.lastType(), u"checkAuthenticationPassword"_s);
    QCOMPARE(transport.last().value(u"password"_s).toString(), u" spaces are significant "_s);

    transport.connected(1234);
    QCOMPARE(states.last().at(0).value<ConnectionState>(), ConnectionState::Connected);
    QCOMPARE(transport.lastType(), u"loadChats"_s);
    provider.disconnectAccount();
    QCOMPARE(states.last().at(0).value<ConnectionState>(), ConnectionState::Disconnected);
    QCOMPARE(transport.closed, 1);
    QVERIFY(!QFileInfo::exists(transport.profile));
}

void TelegramTest::mapsAndFiltersCloudText()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    QSignalSpy removed(&provider, &IChatProvider::messageRemoved);
    QSignalSpy states(&provider, &IChatProvider::stateChanged);
    provider.connectAccount(TestApiHash);

    constexpr qint64 group = -1001234567890;
    constexpr qint64 direct = 901234567891;
    transport.publish(chat(group, u"chatTypeSupergroup"_s, u"Team"_s));
    QVERIFY(conversations.isEmpty());
    transport.connected(123456789);
    QCOMPARE(conversations.size(), 1);
    const auto known = conversations.last().at(0).value<Conversation>();
    QCOMPARE(known.kind, ConversationKind::Group);
    QCOMPARE(known.id.remoteId, u"123456789/-1001234567890"_s);
    transport.publish(chat(direct, u"chatTypePrivate"_s, u"Friend"_s));
    QCOMPARE(conversations.size(), 2);
    transport.publish(chat(-1001, u"chatTypeSupergroup"_s, u"Broadcast"_s, true));
    transport.publish(chat(-1002, u"chatTypeSecret"_s, u"Secret"_s));
    QCOMPARE(conversations.size(), 2);

    transport.publish(
        { { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, message(-1001, 1, u"hidden"_s) } });
    auto ephemeral = message(group, 2, u"auto-deleting"_s);
    ephemeral.insert(u"auto_delete_in"_s, 30);
    transport.publish({ { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, ephemeral } });
    auto pending = message(group, 3, u"pending"_s);
    pending.insert(u"sending_state"_s, QJsonObject { { u"@type"_s, u"messageSendingStatePending"_s } });
    transport.publish({ { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, pending } });
    QVERIFY(messages.isEmpty());

    provider.requestHistory(known.id);
    QCOMPARE(transport.lastType(), u"getChatHistory"_s);
    const auto history = transport.last();
    provider.markRead(known.id);
    QCOMPARE(transport.lastType(), u"getChatHistory"_s);
    transport.reply(history,
        { { u"@type"_s, u"messages"_s },
            { u"messages"_s, QJsonArray { message(group, 5, u"newer"_s), message(group, 4, u"older"_s) } } });
    QCOMPARE(messages.size(), 2);
    const auto earlier = messages.first().at(0).value<Message>();
    QCOMPARE(earlier.id, u"4"_s);
    QVERIFY(earlier.historical);
    QCOMPARE(transport.lastType(), u"viewMessages"_s);

    QVERIFY(!provider.sendText({ known.id.accountId, u"456/901234567891"_s }, u"wrong identity"_s));
    QVERIFY(provider.sendText(known.id, u"/literal text"_s));
    QCOMPARE(transport.lastType(), u"sendMessage"_s);
    auto confirmed = message(group, 6, u"/literal text"_s);
    confirmed.insert(u"is_outgoing"_s, true);
    transport.publish({ { u"@type"_s, u"updateMessageSendSucceeded"_s }, { u"message"_s, confirmed } });
    QCOMPARE(messages.size(), 3);
    const auto sent = messages.last().at(0).value<Message>();
    QCOMPARE(sent.id, u"6"_s);
    QVERIFY(sent.outgoing);
    QVERIFY(!sent.historical);
    transport.publish({ { u"@type"_s, u"updateDeleteMessages"_s }, { u"chat_id"_s, group },
        { u"message_ids"_s, QJsonArray { 6 } }, { u"from_cache"_s, false } });
    QCOMPARE(removed.size(), 1);
    QCOMPARE(removed.last().at(1).toString(), u"6"_s);

    transport.network(u"connectionStateWaitingForNetwork"_s);
    QCOMPARE(states.last().at(0).value<ConnectionState>(), ConnectionState::Reconnecting);
    transport.network(u"connectionStateReady"_s);
    QCOMPARE(states.last().at(0).value<ConnectionState>(), ConnectionState::Connected);
}

void TelegramTest::loadsNativeLibraryWhenInstalled()
{
    if (!TelegramProvider::isAvailable()) {
        if (qEnvironmentVariableIsSet("PHOSCHAT_REQUIRE_TDLIB_TEST"))
            QFAIL("The official TDLib shared library is missing or does not expose the supported JSON "
                  "interface");
        QSKIP("Official TDLib runtime is not installed");
    }
    auto &transport = TelegramTransport::native();
    QSignalSpy events(&transport, &TelegramTransport::received);
    auto firstProfile = std::make_shared<QTemporaryDir>();
    auto secondProfile = std::make_shared<QTemporaryDir>();
    QVERIFY(firstProfile->isValid());
    QVERIFY(secondProfile->isValid());
    const auto firstPath = firstProfile->path();
    const auto secondPath = secondProfile->path();
    const int first = transport.createClient(firstPath, std::move(firstProfile));
    const int second = transport.createClient(secondPath, std::move(secondProfile));
    QVERIFY(first > 0);
    QVERIFY(second > 0 && second != first);

    const auto observed = [&events](int id, const QString &state) {
        for (const auto &event : events) {
            if (event.at(0).toInt() == id
                && event.at(1)
                        .toJsonObject()
                        .value(u"authorization_state"_s)
                        .toObject()
                        .value(u"@type"_s)
                        .toString()
                    == state)
                return true;
        }
        return false;
    };
    // TDLib requests initialization parameters first. No credentials or parameters are sent.
    transport.send(first, { { u"@type"_s, u"getAuthorizationState"_s } });
    transport.send(second, { { u"@type"_s, u"getAuthorizationState"_s } });
    QTRY_VERIFY(observed(first, u"authorizationStateWaitTdlibParameters"_s));
    QTRY_VERIFY(observed(second, u"authorizationStateWaitTdlibParameters"_s));
    transport.closeClient(first);
    transport.closeClient(second);
    QTRY_VERIFY(observed(first, u"authorizationStateClosed"_s));
    QTRY_VERIFY(observed(second, u"authorizationStateClosed"_s));
    QVERIFY(!QFileInfo::exists(firstPath));
    QVERIFY(!QFileInfo::exists(secondPath));
}

QTEST_GUILESS_MAIN(TelegramTest)
#include "test_telegram.moc"
