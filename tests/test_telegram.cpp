#include "providers/telegram/telegram_provider.h"
#include "providers/telegram/telegram_transport.h"

#include <QFile>
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
    QJsonObject lastRequest(const QString &type) const
    {
        for (auto request = requests.crbegin(); request != requests.crend(); ++request) {
            if (request->value(u"@type"_s).toString() == type)
                return *request;
        }
        return {};
    }

    QJsonObject lastChatListRequest(const QString &listType) const
    {
        for (auto request = requests.crbegin(); request != requests.crend(); ++request) {
            if (request->value(u"@type"_s).toString() == u"loadChats"
                && request->value(u"chat_list"_s).toObject().value(u"@type"_s).toString() == listType)
                return *request;
        }
        return {};
    }

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
    void loadsMainAndArchivedGroupsUntilBothListsFinish();
    void resumesInterruptedDiscoveryAndRestoresGroupsAfterReconnect();
    void keepsRestrictedGroupsVisibleWithoutReadingOrSending();
    void loadsOnlyCurrentAvatarFromThePrivateProfile();
    void fetchesContactDetailsOnDemand();
    void reportsOnlyKnownDirectContactPresence();
    void sendsTypedAttachmentsOnlyWhenAllowed();
    void loadsNativeLibraryWhenInstalled();
    void loadsShortHistoryPagesAndContinuesUntilEmpty();
    void historyErrorsAndOverlappingPagesRemainRetryable();
    void openChatLifecycleAndOfflineHistoryResume();
    void mapsSendStatesAndDirectReadReceipts();
    void rendersMediaMetadataWithoutDownloadingAttachments();
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

void TelegramTest::loadsMainAndArchivedGroupsUntilBothListsFinish()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy notices(&provider, &IChatProvider::notice);
    provider.connectAccount(TestApiHash);
    transport.connected(42);

    const auto main = transport.lastChatListRequest(u"chatListMain"_s);
    const auto archive = transport.lastChatListRequest(u"chatListArchive"_s);
    QVERIFY(!main.isEmpty());
    QVERIFY(!archive.isEmpty());
    QCOMPARE(main.value(u"limit"_s).toInt(), 100);
    QCOMPARE(archive.value(u"limit"_s).toInt(), 100);

    transport.publish(chat(-10, u"chatTypeBasicGroup"_s, u"Friends"_s));
    const auto basic = conversations.last().first().value<Conversation>();
    QCOMPARE(basic.kind, ConversationKind::Group);
    QCOMPARE(basic.id.remoteId, u"42/-10"_s);
    QVERIFY(basic.joined);
    transport.publish(chat(-10020, u"chatTypeSupergroup"_s, u"Community"_s));
    QCOMPARE(conversations.size(), 2);
    transport.publish(chat(-10020, u"chatTypeSupergroup"_s, u"Community"_s));
    QCOMPARE(conversations.size(), 2); // Repeated metadata must not duplicate discovery.

    transport.publish(chat(-10030, u"chatTypeSupergroup"_s, u"Archived group"_s));
    const auto archived = conversations.last().first().value<Conversation>();
    QCOMPARE(archived.kind, ConversationKind::Group);
    QVERIFY(provider.sendText(archived.id, u"Still a member"_s));
    transport.reply(archive, { { u"@type"_s, u"ok"_s } });
    QCOMPARE(
        transport.last().value(u"chat_list"_s).toObject().value(u"@type"_s).toString(), u"chatListArchive"_s);
    transport.reply(transport.lastChatListRequest(u"chatListArchive"_s),
        { { u"@type"_s, u"error"_s }, { u"code"_s, 404 } });
    const auto finishedArchive = transport.lastChatListRequest(u"chatListArchive"_s);

    // Even a short page is not EOF. Previous releases silently stopped after
    // 50 pages, although TDLib can return much less than the requested limit.
    for (int page = 0; page < 55; ++page) {
        const auto count = transport.requests.size();
        transport.reply(transport.lastChatListRequest(u"chatListMain"_s), { { u"@type"_s, u"ok"_s } });
        QCOMPARE(transport.requests.size(), count + 1);
        QCOMPARE(transport.last().value(u"chat_list"_s).toObject().value(u"@type"_s).toString(),
            u"chatListMain"_s);
    }
    transport.publish(chat(-10040, u"chatTypeSupergroup"_s, u"Older group"_s));
    QCOMPARE(conversations.size(), 4);
    const auto count = transport.requests.size();
    transport.reply(
        transport.lastChatListRequest(u"chatListMain"_s), { { u"@type"_s, u"error"_s }, { u"code"_s, 404 } });
    QCOMPARE(transport.requests.size(), count);
    QVERIFY(notices.isEmpty());
    QCOMPARE(transport.lastChatListRequest(u"chatListArchive"_s), finishedArchive);

    // Discovery must never unarchive/join groups or mark them read.
    for (const auto &request : transport.requests) {
        const auto type = request.value(u"@type"_s).toString();
        QVERIFY(type != u"addChatToList" && type != u"joinChat" && type != u"viewMessages");
    }
}

void TelegramTest::resumesInterruptedDiscoveryAndRestoresGroupsAfterReconnect()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy notices(&provider, &IChatProvider::notice);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    transport.publish(chat(-10, u"chatTypeBasicGroup"_s, u"Friends"_s));
    const auto known = conversations.last().first().value<Conversation>();
    transport.reply(transport.lastChatListRequest(u"chatListMain"_s),
        { { u"@type"_s, u"error"_s }, { u"code"_s, 500 }, { u"message"_s, u"private details"_s } });
    QCOMPARE(notices.size(), 1);
    QVERIFY(!notices.last().first().toString().contains(u"private details"));
    transport.reply(transport.lastChatListRequest(u"chatListArchive"_s),
        { { u"@type"_s, u"error"_s }, { u"code"_s, 404 } });
    const auto count = transport.requests.size();
    transport.network(u"connectionStateWaitingForNetwork"_s);
    QVERIFY(!provider.sendText(known.id, u"Offline"_s));
    transport.network(u"connectionStateReady"_s);
    QCOMPARE(transport.requests.size(), count + 1);
    QCOMPARE(
        transport.last().value(u"chat_list"_s).toObject().value(u"@type"_s).toString(), u"chatListMain"_s);
    QCOMPARE(conversations.size(), 2);
    QVERIFY(conversations.last().first().value<Conversation>().joined);
    transport.network(u"connectionStateReady"_s);
    QCOMPARE(transport.requests.size(), count + 1); // No duplicate in-flight page.
    QCOMPARE(conversations.size(), 2);
    QVERIFY(provider.sendText(known.id, u"Back online"_s));
}

void TelegramTest::keepsRestrictedGroupsVisibleWithoutReadingOrSending()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy notices(&provider, &IChatProvider::notice);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    provider.connectAccount(TestApiHash);
    transport.connected(42);

    for (const auto &field : { u"has_protected_content"_s, u"message_auto_delete_time"_s }) {
        const qint64 id = field == u"has_protected_content" ? -10 : -20;
        auto event = chat(id, u"chatTypeBasicGroup"_s, u"Restricted group"_s);
        auto value = event.value(u"chat"_s).toObject();
        value.insert(field, field == u"has_protected_content" ? QJsonValue(true) : QJsonValue(86400));
        event.insert(u"chat"_s, value);
        const auto count = conversations.size();
        transport.publish(event);
        QCOMPARE(conversations.size(), count + 1);
        const auto known = conversations.last().first().value<Conversation>();
        QCOMPARE(known.kind, ConversationKind::Group);
        QVERIFY(!known.joined);
        const auto requests = transport.requests.size();
        provider.requestHistory(known.id);
        QVERIFY(!provider.sendText(known.id, u"Not supported yet"_s));
        transport.publish(
            { { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, message(id, 1, u"Private"_s) } });
        QCOMPARE(transport.requests.size(), requests);
        QVERIFY(messages.isEmpty());
    }
    QCOMPARE(notices.size(), 4);

    // Group permission changes affect sending, without hiding the group.
    transport.publish(chat(-30, u"chatTypeSupergroup"_s, u"Read-only group"_s));
    transport.publish({ { u"@type"_s, u"updateChatPermissions"_s }, { u"chat_id"_s, -30 },
        { u"permissions"_s, QJsonObject { { u"can_send_basic_messages"_s, false } } } });
    const auto readonly = conversations.last().first().value<Conversation>();
    QVERIFY(!readonly.joined);
    QVERIFY(!provider.sendText(readonly.id, u"Unavailable"_s));
    provider.requestHistory(readonly.id);
    QCOMPARE(transport.lastType(), u"getChatHistory"_s);

    // Broadcast sponsored messages and secret-chat lifecycles remain outside
    // this preview; neither may leak ordinary text through the group path.
    const auto count = conversations.size();
    transport.publish(chat(-100, u"chatTypeSupergroup"_s, u"Broadcast"_s, true));
    transport.publish(chat(-101, u"chatTypeSecret"_s, u"Secret"_s));
    QCOMPARE(conversations.size(), count);
}

void TelegramTest::loadsOnlyCurrentAvatarFromThePrivateProfile()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    provider.connectAccount(TestApiHash);
    transport.connected(42);

    const auto photo = [](int id) {
        return QJsonObject { { u"small"_s,
            QJsonObject {
                { u"id"_s, id }, { u"local"_s, QJsonObject { { u"can_be_downloaded"_s, true } } } } } };
    };
    const auto publishChat = [&](qint64 id, int file) {
        auto event = chat(id, u"chatTypePrivate"_s, u"Contact"_s);
        auto data = event.value(u"chat"_s).toObject();
        data.insert(u"photo"_s, photo(file));
        event.insert(u"chat"_s, data);
        transport.publish(event);
    };
    const auto updatePhoto = [&](qint64 id, const QJsonValue &value) {
        transport.publish(
            { { u"@type"_s, u"updateChatPhoto"_s }, { u"chat_id"_s, id }, { u"photo"_s, value } });
    };
    const auto completedFile = [](int id, const QString &path) {
        return QJsonObject { { u"@type"_s, u"file"_s }, { u"id"_s, id },
            { u"local"_s, QJsonObject { { u"path"_s, path }, { u"is_downloading_completed"_s, true } } } };
    };
    // The transport only handles bounded opaque JPEG bytes. Image decoding belongs to the UI.
    const auto jpeg = QByteArray::fromHex("ffd8ffe0ffd9");
    const auto writePhoto = [&](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(jpeg) == jpeg.size();
    };

    publishChat(7, 100);
    QCOMPARE(transport.lastType(), u"downloadFile"_s);
    const auto download = transport.last();
    QCOMPARE(download.value(u"file_id"_s).toInt(), 100);
    QVERIFY(conversations.last().first().value<Conversation>().avatar.isEmpty());
    const auto requestCount = transport.requests.size();
    publishChat(8, 100);
    QCOMPARE(transport.requests.size(), requestCount);
    updatePhoto(7, QJsonValue::Null);

    const auto local = transport.profile + u"/photo.jpg"_s;
    QVERIFY(writePhoto(local));
    const auto before = conversations.size();
    transport.reply(download, completedFile(100, local));
    QCOMPARE(conversations.size(), before + 1);
    QCOMPARE(conversations.last().first().value<Conversation>().id.remoteId, u"42/8"_s);
    QCOMPARE(conversations.last().first().value<Conversation>().avatar, jpeg);

    updatePhoto(7, photo(100));
    QCOMPARE(transport.requests.size(), requestCount);
    QCOMPARE(conversations.last().first().value<Conversation>().avatar, jpeg);

    updatePhoto(8, photo(101));
    QCOMPARE(transport.last().value(u"file_id"_s).toInt(), 101);
    QVERIFY(conversations.last().first().value<Conversation>().avatar.isEmpty());
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const auto outsideFile = outside.filePath(u"photo.jpg"_s);
    QVERIFY(writePhoto(outsideFile));
    const auto beforeInvalid = conversations.size();
    transport.reply(transport.last(), completedFile(101, outsideFile));
    QCOMPARE(conversations.size(), beforeInvalid);
}

void TelegramTest::fetchesContactDetailsOnDemand()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    QList<QPair<ConversationId, ProfileDetails>> profiles;
    connect(&provider, &IChatProvider::profileReceived, this,
        [&](const ConversationId &id, const ProfileDetails &details) { profiles.append({ id, details }); });
    transport.publish({ { u"@type"_s, u"updateNewChat"_s },
        { u"chat"_s,
            QJsonObject { { u"id"_s, 7 }, { u"title"_s, u"Contact"_s },
                { u"type"_s,
                    QJsonObject { { u"@type"_s, u"chatTypePrivate"_s }, { u"user_id"_s, 7 } } } } } });
    const ConversationId id { account().id, u"42/7"_s };
    QVERIFY(provider.requestProfile(id));
    QCOMPARE(transport.lastType(), u"getUser"_s);
    const auto requestCount = transport.requests.size();
    QVERIFY(provider.requestProfile(id));
    QCOMPARE(transport.requests.size(), requestCount);
    transport.reply(transport.last(),
        { { u"@type"_s, u"user"_s }, { u"id"_s, 7 },
            { u"usernames"_s, QJsonObject { { u"active_usernames"_s, QJsonArray { u"example"_s } } } } });
    QCOMPARE(transport.lastType(), u"getUserFullInfo"_s);
    QVERIFY(profiles.isEmpty());
    transport.reply(transport.last(),
        { { u"@type"_s, u"userFullInfo"_s }, { u"bio"_s, QJsonObject { { u"text"_s, u"Profile bio"_s } } } });
    QCOMPARE(profiles.size(), 1);
    QVERIFY(profiles.first().first == id);
    QCOMPARE(profiles.first().second,
        (ProfileDetails { { u"Username"_s, u"@example"_s }, { u"Bio"_s, u"Profile bio"_s } }));
}

void TelegramTest::reportsOnlyKnownDirectContactPresence()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy presence(&provider, &IChatProvider::presenceChanged);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    transport.publish({ { u"@type"_s, u"updateNewChat"_s },
        { u"chat"_s,
            QJsonObject { { u"id"_s, 7 }, { u"title"_s, u"Contact"_s },
                { u"type"_s,
                    QJsonObject { { u"@type"_s, u"chatTypePrivate"_s }, { u"user_id"_s, 7 } } } } } });
    const ConversationId direct { account().id, u"42/7"_s };
    provider.setConversationOpen(direct, true);
    QCOMPARE(transport.lastType(), u"openChat"_s);
    const auto lookup = transport.requests.at(transport.requests.size() - 2);
    QCOMPARE(lookup.value(u"@type"_s).toString(), u"getUser"_s);
    QCOMPARE(lookup.value(u"user_id"_s).toInteger(), 7);
    const auto now = QDateTime::currentSecsSinceEpoch();
    const QJsonObject online { { u"@type"_s, u"userStatusOnline"_s }, { u"expires"_s, now + 120 } };
    transport.reply(lookup, { { u"@type"_s, u"user"_s }, { u"id"_s, 7 }, { u"status"_s, online } });
    QCOMPARE(presence.size(), 1);
    QVERIFY(presence.last().first().value<ConversationId>() == direct);
    QCOMPARE(presence.last().at(1).value<Presence>(), Presence::Online);

    const auto status = [&](qint64 user, const QJsonObject &value) {
        transport.publish(
            { { u"@type"_s, u"updateUserStatus"_s }, { u"user_id"_s, user }, { u"status"_s, value } });
    };
    status(42, online);
    status(99, online);
    QCOMPARE(presence.size(), 1);
    status(7, { { u"@type"_s, u"userStatusRecently"_s } });
    QCOMPARE(presence.last().at(1).value<Presence>(), Presence::Unknown);
    status(7, { { u"@type"_s, u"userStatusOffline"_s }, { u"was_online"_s, now - 60 } });
    QCOMPARE(presence.last().at(1).value<Presence>(), Presence::Offline);
    status(7, { { u"@type"_s, u"userStatusOnline"_s }, { u"expires"_s, now - 1 } });
    QCOMPARE(presence.last().at(1).value<Presence>(), Presence::Unknown);

    provider.setConversationOpen(direct, false);
    QCOMPARE(transport.lastType(), u"closeChat"_s);
    const auto afterClose = presence.size();
    status(7, online);
    QCOMPARE(presence.size(), afterClose);
}

void TelegramTest::sendsTypedAttachmentsOnlyWhenAllowed()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    transport.publish(chat(7, u"chatTypePrivate"_s, u"Contact"_s));
    const ConversationId id { account().id, u"42/7"_s };
    QTemporaryDir files;
    QVERIFY(files.isValid());
    QFile jpeg(files.filePath(u"photo.jpg"_s));
    QVERIFY(jpeg.open(QIODevice::WriteOnly));
    const auto jpegHeader = QByteArray::fromHex("ffd8ffe000104a46494600010100000100010000ffd9");
    QCOMPARE(jpeg.write(jpegHeader), jpegHeader.size());
    jpeg.close();
    QVERIFY(provider.capabilities().testFlag(Capability::Files));
    QVERIFY(provider.sendFile(id, jpeg.fileName()));
    QCOMPARE(transport.lastType(), u"sendMessage"_s);
    const auto content = transport.last().value(u"input_message_content"_s).toObject();
    QCOMPARE(content.value(u"@type"_s).toString(), u"inputMessagePhoto"_s);
    const auto input = content.value(u"photo"_s).toObject();
    QCOMPARE(input.value(u"@type"_s).toString(), u"inputFileLocal"_s);
    QCOMPARE(input.value(u"path"_s).toString(), QFileInfo(jpeg).canonicalFilePath());

    transport.publish({ { u"@type"_s, u"updateChatPermissions"_s }, { u"chat_id"_s, 7 },
        { u"permissions"_s,
            QJsonObject { { u"can_send_basic_messages"_s, true }, { u"can_send_photos"_s, false } } } });
    const auto requests = transport.requests.size();
    QVERIFY(!provider.sendFile(id, jpeg.fileName()));
    QVERIFY(!provider.sendFile({ account().id, u"99/7"_s }, jpeg.fileName()));
    QCOMPARE(transport.requests.size(), requests);

    transport.publish({ { u"@type"_s, u"updateChatPermissions"_s }, { u"chat_id"_s, 7 },
        { u"permissions"_s,
            QJsonObject { { u"can_send_basic_messages"_s, false }, { u"can_send_photos"_s, true } } } });
    QVERIFY(provider.canSendFiles(id));
    QVERIFY(!provider.sendText(id, u"Text is restricted"_s));
    QVERIFY(provider.sendFile(id, jpeg.fileName()));
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

void TelegramTest::loadsShortHistoryPagesAndContinuesUntilEmpty()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    QSignalSpy states(&provider, &IChatProvider::historyStateChanged);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    transport.publish(chat(-10, u"chatTypeBasicGroup"_s, u"Friends"_s));
    const ConversationId id { account().id, u"42/-10"_s };
    QCOMPARE(provider.historyState(id), HistoryState::Ready);
    QVERIFY(provider.capabilities().testFlag(Capability::PaginatedHistory));
    provider.requestHistory(id);
    QCOMPARE(provider.historyState(id), HistoryState::Loading);
    const auto first = transport.lastRequest(u"getChatHistory"_s);
    QCOMPARE(first.value(u"from_message_id"_s).toInteger(), 0);
    transport.reply(first,
        { { u"@type"_s, u"messages"_s },
            { u"messages"_s, QJsonArray { message(-10, 100, u"Latest only"_s) } } });
    QCOMPARE(messages.size(), 1);
    QTRY_COMPARE(transport.lastRequest(u"getChatHistory"_s).value(u"from_message_id"_s).toInteger(), 100);
    const auto second = transport.lastRequest(u"getChatHistory"_s);
    QJsonArray page { message(-10, 100, u"Boundary duplicate"_s) };
    for (qint64 number = 99; number >= 51; --number)
        page.append(message(-10, number, u"Earlier"_s));
    transport.reply(second, { { u"@type"_s, u"messages"_s }, { u"messages"_s, page } });
    QCOMPARE(messages.size(), 50);
    QCOMPARE(provider.historyState(id), HistoryState::Ready);
    const auto requestCount = transport.requests.size();
    provider.requestHistory(id); // Re-activation must not silently fetch older pages.
    QCOMPARE(transport.requests.size(), requestCount);
    provider.requestOlderHistory(id);
    const auto third = transport.lastRequest(u"getChatHistory"_s);
    QCOMPARE(third.value(u"from_message_id"_s).toInteger(), 51);
    auto media = message(-10, 40, u"Filtered media"_s);
    media.insert(u"content"_s, QJsonObject { { u"@type"_s, u"messagePhoto"_s } });
    transport.reply(third,
        { { u"@type"_s, u"messages"_s },
            { u"messages"_s, QJsonArray { message(-10, 50, u"Older"_s), media } } });
    QTRY_COMPARE(transport.lastRequest(u"getChatHistory"_s).value(u"from_message_id"_s).toInteger(), 40);
    QCOMPARE(messages.size(), 52); // Media is represented by a safe text placeholder.
    const auto last = transport.lastRequest(u"getChatHistory"_s);
    transport.reply(last, { { u"@type"_s, u"messages"_s }, { u"messages"_s, QJsonArray {} } });
    QCOMPARE(provider.historyState(id), HistoryState::Complete);
    const auto finishedCount = transport.requests.size();
    provider.requestOlderHistory(id);
    QCOMPARE(transport.requests.size(), finishedCount);
    QVERIFY(!states.isEmpty());
    for (const auto &event : messages) {
        const auto value = event.first().value<Message>();
        QVERIFY(value.historical);
        QCOMPARE(value.sequence, value.id.toLongLong());
    }
}

void TelegramTest::historyErrorsAndOverlappingPagesRemainRetryable()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    QSignalSpy notices(&provider, &IChatProvider::notice);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    transport.publish(chat(10, u"chatTypePrivate"_s, u"Friend"_s));
    const ConversationId id { account().id, u"42/10"_s };
    provider.requestHistory(id);
    const auto first = transport.lastRequest(u"getChatHistory"_s);
    transport.reply(first,
        { { u"@type"_s, u"messages"_s }, { u"messages"_s, QJsonArray { message(10, 100, u"Latest"_s) } } });
    QTRY_COMPARE(transport.lastRequest(u"getChatHistory"_s).value(u"from_message_id"_s).toInteger(), 100);
    const auto next = transport.lastRequest(u"getChatHistory"_s);
    transport.reply(
        next, { { u"@type"_s, u"error"_s }, { u"code"_s, 500 }, { u"message"_s, u"sensitive"_s } });
    QCOMPARE(provider.historyState(id), HistoryState::Ready);
    QVERIFY(!notices.last().first().toString().contains(u"sensitive"));
    provider.requestOlderHistory(id);
    const auto retried = transport.lastRequest(u"getChatHistory"_s);
    QCOMPARE(retried.value(u"from_message_id"_s).toInteger(), 100);
    // A stale result from an earlier request must not complete or rewind the retry.
    transport.reply(next, { { u"@type"_s, u"messages"_s }, { u"messages"_s, QJsonArray {} } });
    QCOMPARE(provider.historyState(id), HistoryState::Loading);
    transport.reply(retried,
        { { u"@type"_s, u"messages"_s },
            { u"messages"_s, QJsonArray { message(10, 100, u"Only duplicate"_s) } } });
    QCOMPARE(provider.historyState(id), HistoryState::Ready);
    QCOMPARE(messages.size(), 1);
    const auto count = transport.requests.size();
    QTest::qWait(1);
    QCOMPARE(transport.requests.size(), count); // No endless loop on a nonadvancing page.
    provider.requestOlderHistory(id);
    const auto progress = transport.lastRequest(u"getChatHistory"_s);
    transport.reply(progress,
        { { u"@type"_s, u"messages"_s },
            { u"messages"_s,
                QJsonArray { message(10, 90, u"Recovered"_s), message(10, 90, u"Recovered"_s) } } });
    QCOMPARE(messages.size(), 2);
    QTRY_COMPARE(transport.lastRequest(u"getChatHistory"_s).value(u"from_message_id"_s).toInteger(), 90);
    provider.disconnectAccount();
    QTest::qWait(1);
}

void TelegramTest::openChatLifecycleAndOfflineHistoryResume()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy cleared(&provider, &IChatProvider::historyCleared);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    transport.publish(chat(-10, u"chatTypeBasicGroup"_s, u"Friends"_s));
    const ConversationId id { account().id, u"42/-10"_s };
    provider.setConversationOpen(id, true);
    QCOMPARE(transport.lastType(), u"openChat"_s);
    const auto opened = transport.requests.size();
    provider.setConversationOpen(id, true);
    QCOMPARE(transport.requests.size(), opened);
    transport.network(u"connectionStateWaitingForNetwork"_s);
    provider.requestHistory(id);
    QCOMPARE(provider.historyState(id), HistoryState::Unavailable);
    QCOMPARE(transport.requests.size(), opened);
    transport.network(u"connectionStateReady"_s);
    QCOMPARE(transport.lastType(), u"getChatHistory"_s);
    QCOMPARE(provider.historyState(id), HistoryState::Loading);
    const auto history = transport.last();
    transport.reply(history,
        { { u"@type"_s, u"messages"_s }, { u"messages"_s, QJsonArray { message(-10, 100, u"Latest"_s) } } });
    // Becoming restricted closes the TDLib chat and cancels a queued older page.
    transport.publish({ { u"@type"_s, u"updateChatHasProtectedContent"_s }, { u"chat_id"_s, -10 },
        { u"has_protected_content"_s, true } });
    QCOMPARE(transport.lastType(), u"closeChat"_s);
    QCOMPARE(cleared.size(), 1);
    QCOMPARE(cleared.constFirst().constFirst().value<ConversationId>(), id);
    const auto restrictedCount = transport.requests.size();
    QTest::qWait(1);
    QCOMPARE(transport.requests.size(), restrictedCount);
    QCOMPARE(provider.historyState(id), HistoryState::Unavailable);
    transport.publish({ { u"@type"_s, u"updateChatHasProtectedContent"_s }, { u"chat_id"_s, -10 },
        { u"has_protected_content"_s, false } });
    QCOMPARE(transport.lastType(), u"openChat"_s);
    provider.setConversationOpen(id, false);
    QCOMPARE(transport.lastType(), u"closeChat"_s);
    provider.setConversationOpen(id, true);
    provider.disconnectAccount();
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    transport.publish(chat(-10, u"chatTypeBasicGroup"_s, u"Friends"_s));
    QCOMPARE(transport.lastType(), u"openChat"_s); // Intent survives a new TDLib client.
}

void TelegramTest::mapsSendStatesAndDirectReadReceipts()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    QSignalSpy removed(&provider, &IChatProvider::messageRemoved);
    QSignalSpy receipts(&provider, &IChatProvider::messagesReadThrough);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    auto initial = chat(10, u"chatTypePrivate"_s, u"Friend"_s);
    auto value = initial.value(u"chat"_s).toObject();
    value.insert(u"last_read_outbox_message_id"_s, 90);
    initial.insert(u"chat"_s, value);
    transport.publish(initial);
    QVERIFY(provider.capabilities().testFlag(Capability::ReadReceipts));
    const ConversationId id { account().id, u"42/10"_s };

    auto pending = message(10, 1001, u"Outgoing"_s);
    pending.insert(u"is_outgoing"_s, true);
    pending.insert(u"sending_state"_s, QJsonObject { { u"@type"_s, u"messageSendingStatePending"_s } });
    QVERIFY(provider.sendText(id, u"Outgoing"_s));
    const auto request = transport.lastRequest(u"sendMessage"_s);
    transport.reply(request, pending);
    QCOMPARE(messages.last().first().value<Message>().delivery, DeliveryState::Pending);

    auto sent = pending;
    sent.insert(u"id"_s, 100);
    transport.publish({ { u"@type"_s, u"updateMessageSendSucceeded"_s }, { u"old_message_id"_s, 1001 },
        { u"message"_s, sent } });
    QCOMPARE(removed.last().at(1).toString(), u"1001"_s);
    QCOMPARE(messages.last().first().value<Message>().delivery, DeliveryState::Sent);
    const auto settledCount = messages.size();
    transport.reply(request, pending); // A late pending result cannot resurrect the temporary row.
    QCOMPARE(messages.size(), settledCount);

    transport.publish({ { u"@type"_s, u"updateChatReadOutbox"_s }, { u"chat_id"_s, 10 },
        { u"last_read_outbox_message_id"_s, 100 } });
    QCOMPARE(receipts.last().at(0).value<ConversationId>(), id);
    QCOMPARE(receipts.last().at(1).toLongLong(), 100);
    const auto receiptCount = receipts.size();
    transport.publish({ { u"@type"_s, u"updateChatReadOutbox"_s }, { u"chat_id"_s, 10 },
        { u"last_read_outbox_message_id"_s, 90 } });
    QCOMPARE(receipts.size(), receiptCount);
    sent.remove(u"sending_state"_s);
    transport.publish({ { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, sent } });
    QCOMPARE(messages.last().first().value<Message>().delivery, DeliveryState::Read);

    auto failed = pending;
    failed.insert(u"id"_s, 1002);
    transport.publish({ { u"@type"_s, u"updateMessageSendFailed"_s }, { u"old_message_id"_s, 1002 },
        { u"message"_s, failed }, { u"error"_s, QJsonObject { { u"code"_s, 400 } } } });
    QCOMPARE(messages.last().first().value<Message>().delivery, DeliveryState::Failed);
    const auto failedCount = messages.size();
    transport.publish({ { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, failed } });
    QCOMPARE(messages.size(), failedCount);

    transport.publish(chat(-10, u"chatTypeBasicGroup"_s, u"Group"_s));
    transport.publish({ { u"@type"_s, u"updateChatReadOutbox"_s }, { u"chat_id"_s, -10 },
        { u"last_read_outbox_message_id"_s, 500 } });
    QCOMPARE(receipts.size(), receiptCount);
    sent.insert(u"chat_id"_s, -10);
    transport.publish({ { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, sent } });
    QCOMPARE(messages.last().first().value<Message>().delivery, DeliveryState::Sent);
}

void TelegramTest::rendersMediaMetadataWithoutDownloadingAttachments()
{
    FakeTelegramTransport transport;
    TelegramProvider provider(account(), transport);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    provider.connectAccount(TestApiHash);
    transport.connected(42);
    transport.publish(chat(10, u"chatTypePrivate"_s, u"Friend"_s));
    const auto requests = transport.requests.size();
    const QStringList types { u"messagePhoto"_s, u"messageVideo"_s, u"messageAudio"_s, u"messageDocument"_s,
        u"messageVoiceNote"_s, u"messageVideoNote"_s, u"messageAnimation"_s, u"messageSticker"_s };
    qint64 id = 1;
    for (const auto &type : types) {
        auto content = QJsonObject { { u"@type"_s, type },
            { u"caption"_s, QJsonObject { { u"text"_s, u"<b>Plain caption</b>"_s } } } };
        if (type == u"messageDocument")
            content.insert(u"document"_s, QJsonObject { { u"file_name"_s, u"../../secret\nreport.pdf"_s } });
        auto item = message(10, id++, {});
        item.insert(u"content"_s, content);
        transport.publish({ { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, item } });
        QVERIFY(messages.last().first().value<Message>().text.contains(u"<b>Plain caption</b>"));
        QCOMPARE(messages.last().first().value<Message>().delivery, DeliveryState::None);
    }
    QCOMPARE(messages.size(), types.size());
    QCOMPARE(transport.requests.size(), requests); // Never fetch attachment bodies for previews.
    const auto document = messages.at(3).first().value<Message>().text;
    QVERIFY(document.startsWith(u"File · secretreport.pdf"));
    QVERIFY(!document.contains(u"../"));
    auto restricted = message(10, 99, {});
    restricted.insert(u"content"_s, QJsonObject { { u"@type"_s, u"messagePhoto"_s } });
    restricted.insert(u"can_be_saved"_s, false);
    transport.publish({ { u"@type"_s, u"updateNewMessage"_s }, { u"message"_s, restricted } });
    QCOMPARE(messages.size(), types.size());
}

QTEST_GUILESS_MAIN(TelegramTest)
#include "test_telegram.moc"
