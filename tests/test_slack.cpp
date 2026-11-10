#include "providers/slack/slack_provider.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>

using namespace Qt::StringLiterals;
using namespace phoschat;

namespace {

struct Request {
    QByteArray verb;
    QUrl url;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;

    QUrlQuery fields() const { return QUrlQuery(QString::fromUtf8(body)); }
    QString method() const { return url.path().section(u'/', -1); }
};

class SlackFixture final : public QObject {
public:
    using Handler = std::function<void(QTcpSocket &, const Request &)>;

    SlackFixture()
    {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const auto divider = buffer->indexOf("\r\n\r\n");
                    if (divider < 0)
                        return;
                    const auto lines = buffer->first(divider).split('\n');
                    const auto first = lines.first().trimmed().split(' ');
                    if (first.size() != 3)
                        return;
                    Request request;
                    request.verb = first[0];
                    request.url = QUrl::fromEncoded(first[1]);
                    for (const auto &line : lines.sliced(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0)
                            request.headers.insert(
                                line.first(colon).toLower(), line.sliced(colon + 1).trimmed());
                    }
                    const auto length = request.headers.value("content-length").toInt();
                    if (buffer->size() < divider + 4 + length)
                        return;
                    request.body = buffer->sliced(divider + 4, length);
                    received.append(request);
                    handler(*socket, request);
                });
            }
        });
    }

    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }

    SlackProvider::TestEndpoints endpoints() const
    {
        const auto base = u"http://127.0.0.1:%1/"_s.arg(server.serverPort());
        return { QUrl(base + u"api/"_s), QUrl(base + u"oauth/v2/authorize"_s), 150, 5 };
    }

    static void respond(
        QTcpSocket &socket, const QJsonObject &body, int status = 200, QByteArrayView headers = {})
    {
        const auto json = QJsonDocument(body).toJson(QJsonDocument::Compact);
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\n";
        response += headers;
        response += "Content-Type: application/json\r\nConnection: close\r\nContent-Length: ";
        response += QByteArray::number(json.size());
        response += "\r\n\r\n" + json;
        socket.write(response);
        socket.disconnectFromHost();
    }

    QTcpServer server;
    Handler handler;
    QList<Request> received;
};

Account slackAccount()
{
    Account account;
    account.id = u"local-slack"_s;
    account.protocol = Protocol::Slack;
    account.providerId = u"111.222"_s;
    return account;
}

QJsonObject identity()
{
    return { { u"ok"_s, true }, { u"team_id"_s, u"TFIXTURE"_s }, { u"user_id"_s, u"UOWNER"_s },
        { u"user"_s, u"owner"_s } };
}

QJsonObject historyMessage(const QString &timestamp, const QString &text)
{
    return { { u"type"_s, u"message"_s }, { u"user"_s, u"UCOLLEAGUE"_s }, { u"ts"_s, timestamp },
        { u"text"_s, text } };
}

bool hasState(const QSignalSpy &spy, ConnectionState state)
{
    for (const auto &args : spy) {
        if (args.front().value<ConnectionState>() == state)
            return true;
    }
    return false;
}

int requestCount(const SlackFixture &fixture, const QString &method)
{
    return static_cast<int>(std::count_if(fixture.received.cbegin(), fixture.received.cend(),
        [&](const Request &request) { return request.method() == method; }));
}

} // namespace

class SlackTest final : public QObject {
    Q_OBJECT

private slots:
    void oauthLoopbackValidatesStateAndRefreshes_data();
    void oauthLoopbackValidatesStateAndRefreshes();
    void listsPollsAndSendsUserMessages();
    void rejectsBotsAndRedirects();
    void waitsAfterRateLimit();
    void profileUsesCachedUserAndConversationDetails();
    void uploadsFileWithoutForwardingToken();
    void rejectsUnsafeUploadsAndMissingPermissions();
    void failedSendDoesNotClaimDelivery();
    void presenceOnlyPollsOpenDirectConversations();
    void presenceHandlesMissingScopeAndLateReplies();
    void presenceHonorsRateLimit();
};

void SlackTest::oauthLoopbackValidatesStateAndRefreshes_data()
{
    QTest::addColumn<bool>("publicCallback");
    QTest::newRow("local-pkce") << false;
    QTest::newRow("public-callback") << true;
}

void SlackTest::oauthLoopbackValidatesStateAndRefreshes()
{
    QFETCH(bool, publicCallback);
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    int grants = 0;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        if (request.method() == u"oauth.v2.access"_s || request.method() == u"refresh"_s) {
            ++grants;
            const auto fields = request.fields();
            const bool refresh = fields.hasQueryItem(u"grant_type"_s);
            const QJsonObject token { { u"token_type"_s, u"user"_s },
                { u"access_token"_s, refresh ? u"xoxe.xoxp-fixture-new"_s : u"xoxe.xoxp-fixture-old"_s },
                { u"refresh_token"_s, refresh ? u"xoxe-fixture-new"_s : u"xoxe-fixture-old"_s },
                { u"expires_in"_s, 43200 } };
            auto payload = QJsonObject { { u"ok"_s, true } };
            if (refresh) {
                for (auto it = token.constBegin(); it != token.constEnd(); ++it)
                    payload.insert(it.key(), it.value());
            } else {
                auto user = token;
                user.insert(u"id"_s, u"UOWNER"_s);
                payload.insert(u"authed_user"_s, user);
                payload.insert(u"team"_s, QJsonObject { { u"id"_s, u"TFIXTURE"_s } });
            }
            SlackFixture::respond(socket, payload);
        } else if (request.method() == u"auth.test"_s) {
            SlackFixture::respond(socket, identity());
        } else {
            SlackFixture::respond(socket, { { u"ok"_s, true }, { u"channels"_s, QJsonArray() } });
        }
    };

    auto endpoints = fixture.endpoints();
    if (publicCallback) {
        endpoints.oauthOrigin = endpoints.apiBase.adjusted(QUrl::RemovePath);
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        endpoints.callbackPort = probe.serverPort();
    }
    SlackProvider provider(slackAccount(), endpoints);
    QSignalSpy authorization(&provider, &IChatProvider::authorizationUrlRequested);
    QSignalSpy credentials(&provider, &IChatProvider::credentialsUpdated);
    QSignalSpy states(&provider, &IChatProvider::stateChanged);
    provider.connectAccount({});
    QCOMPARE(authorization.size(), 1);
    const auto authUrl = authorization.first().first().toUrl();
    QCOMPARE(authUrl.path(), u"/oauth/v2/authorize"_s);
    const QUrlQuery challenge(authUrl);
    QCOMPARE(challenge.queryItemValue(u"code_challenge_method"_s), u"S256"_s);
    QVERIFY(challenge.queryItemValue(u"user_scope"_s).contains(u"im:history"_s));
    QVERIFY(challenge.queryItemValue(u"user_scope"_s).contains(u"files:write"_s));
    QVERIFY(challenge.queryItemValue(u"scope"_s).isEmpty());
    QVERIFY(!challenge.hasQueryItem(u"client_secret"_s));
    const auto state = challenge.queryItemValue(u"state"_s);
    QVERIFY(state.size() >= 43);
    QUrl callback(challenge.queryItemValue(u"redirect_uri"_s));
    const auto registeredRedirect = callback.toString();
    if (publicCallback) {
        QCOMPARE(callback.adjusted(QUrl::RemovePath), endpoints.oauthOrigin);
        QCOMPARE(callback.path(), u"/phoschat/slack/callback"_s);
        // Simulate the HTTPS relay's 303 target; the browser still returns to the local listener.
        callback.setPort(endpoints.callbackPort);
    } else {
        QCOMPARE(callback.host(), u"localhost"_s);
    }
    callback.setHost(u"127.0.0.1"_s);
    QNetworkAccessManager browser;
    browser.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    callback.setQuery(QUrlQuery { { u"code"_s, u"fixture-code"_s }, { u"state"_s, u"incorrect"_s } });
    auto *invalid = browser.get(QNetworkRequest(callback));
    QSignalSpy invalidFinished(invalid, &QNetworkReply::finished);
    QVERIFY(invalidFinished.wait());
    QCOMPARE(invalid->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 400);
    QCOMPARE(grants, 0);
    callback.setQuery(QUrlQuery { { u"code"_s, u"fixture-code"_s }, { u"state"_s, state } });
    auto *valid = browser.get(QNetworkRequest(callback));
    QSignalSpy validFinished(valid, &QNetworkReply::finished);
    QVERIFY(validFinished.wait());
    QCOMPARE(valid->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QTRY_VERIFY(hasState(states, ConnectionState::Connected));
    QCOMPARE(grants, 1);
    QCOMPARE(credentials.size(), 1);
    const auto exchange = fixture.received[0];
    QCOMPARE(exchange.method(), u"oauth.v2.access"_s);
    QCOMPARE(exchange.verb, QByteArray("POST"));
    QVERIFY(!exchange.headers.contains("authorization"));
    const auto fields = exchange.fields();
    QVERIFY(!fields.hasQueryItem(u"client_secret"_s));
    QCOMPARE(fields.queryItemValue(u"code"_s), u"fixture-code"_s);
    QCOMPARE(fields.queryItemValue(u"redirect_uri"_s, QUrl::FullyDecoded), registeredRedirect);
    const auto verifier = fields.queryItemValue(u"code_verifier"_s).toLatin1();
    QCOMPARE(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                 .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals),
        challenge.queryItemValue(u"code_challenge"_s).toLatin1());
    QCOMPARE(fixture.received[1].headers.value("authorization"), QByteArray("Bearer xoxe.xoxp-fixture-old"));

    auto stored = QJsonDocument::fromJson(credentials.first().first().toByteArray()).object();
    QCOMPARE(stored.value(u"workspace"_s).toString(), u"TFIXTURE"_s);
    QCOMPARE(stored.value(u"user"_s).toString(), u"UOWNER"_s);
    QCOMPARE(stored.value(u"oauth_origin"_s).toString(),
        publicCallback ? endpoints.oauthOrigin.toString() : QString {});
    stored.insert(u"expires_at_ms"_s, QDateTime::currentMSecsSinceEpoch() - 1000);
    provider.disconnectAccount();
    states.clear();
    provider.connectAccount(QJsonDocument(stored).toJson(QJsonDocument::Compact));
    QTRY_VERIFY(hasState(states, ConnectionState::Connected));
    QCOMPARE(grants, 2);
    QCOMPARE(credentials.size(), 2);
    const auto refresh = QJsonDocument::fromJson(credentials.last().first().toByteArray()).object();
    QCOMPARE(refresh.value(u"refresh"_s).toString(), u"xoxe-fixture-new"_s);
    const auto lastExchange
        = std::find_if(fixture.received.crbegin(), fixture.received.crend(), [](const auto &request) {
              return request.method() == u"oauth.v2.access"_s || request.method() == u"refresh"_s;
          });
    QVERIFY(lastExchange != fixture.received.crend());
    QCOMPARE(
        lastExchange->url.path(), publicCallback ? u"/phoschat/slack/refresh"_s : u"/api/oauth.v2.access"_s);
    QCOMPARE(lastExchange->fields().queryItemValue(u"refresh_token"_s), u"xoxe-fixture-old"_s);
    QVERIFY(!lastExchange->fields().hasQueryItem(u"client_secret"_s));
    QVERIFY(!lastExchange->headers.contains("authorization"));

    if (publicCallback) {
        // Reject a changed origin before any credential is sent.
        provider.disconnectAccount();
        states.clear();
        auto mismatched = stored;
        mismatched.insert(u"oauth_origin"_s, u"https://different.example.test"_s);
        provider.connectAccount(QJsonDocument(mismatched).toJson(QJsonDocument::Compact));
        QVERIFY(hasState(states, ConnectionState::Error));
        QCOMPARE(grants, 2);

        // A legacy grant has no public origin and still renews directly with Slack.
        provider.disconnectAccount();
        states.clear();
        stored.remove(u"oauth_origin"_s);
        provider.connectAccount(QJsonDocument(stored).toJson(QJsonDocument::Compact));
        QTRY_VERIFY(hasState(states, ConnectionState::Connected));
        QCOMPARE(grants, 3);
        const auto legacyRefresh = std::find_if(fixture.received.crbegin(), fixture.received.crend(),
            [](const auto &request) { return request.fields().hasQueryItem(u"grant_type"_s); });
        QVERIFY(legacyRefresh != fixture.received.crend());
        QCOMPARE(legacyRefresh->url.path(), u"/api/oauth.v2.access"_s);
    }
}

void SlackTest::listsPollsAndSendsUserMessages()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    int history = 0;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        QJsonObject payload { { u"ok"_s, true } };
        const auto method = request.method();
        if (method == u"auth.test"_s) {
            payload = identity();
        } else if (method == u"conversations.list"_s) {
            const bool second = QUrlQuery(request.url).hasQueryItem(u"cursor"_s);
            if (second) {
                payload.insert(u"channels"_s,
                    QJsonArray {
                        QJsonObject { { u"id"_s, u"DPRIVATE"_s }, { u"is_im"_s, true },
                            { u"user"_s, u"UCOLLEAGUE"_s } },
                        QJsonObject { { u"id"_s, u"COUTSIDE"_s }, { u"name"_s, u"outside"_s } },
                    });
            } else {
                payload.insert(u"channels"_s,
                    QJsonArray {
                        QJsonObject { { u"id"_s, u"CGENERAL"_s }, { u"is_member"_s, true },
                            { u"name"_s, u"general"_s } },
                    });
                payload.insert(u"response_metadata"_s, QJsonObject { { u"next_cursor"_s, u"page-two"_s } });
            }
        } else if (method == u"users.info"_s) {
            payload.insert(u"user"_s, QJsonObject { { u"real_name"_s, u"Colleague"_s } });
        } else if (method == u"conversations.history"_s) {
            ++history;
            if (history == 1) {
                payload.insert(u"messages"_s,
                    QJsonArray {
                        historyMessage(u"1700000001.000001"_s, u"Hello <@UOWNER> &amp; welcome"_s),
                        historyMessage(u"1700000000.000001"_s, u"Earlier message"_s),
                    });
            } else if (history == 2) {
                payload.insert(
                    u"messages"_s, QJsonArray { historyMessage(u"1700000003.000001"_s, u"Live reply"_s) });
            }
        } else if (method == u"chat.postMessage"_s) {
            payload.insert(u"ts"_s, u"1700000002.000001"_s);
        }
        SlackFixture::respond(socket, payload);
    };

    SlackProvider provider(slackAccount(), fixture.endpoints());
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    provider.connectAccount("xoxp-fixture-user");
    QTRY_VERIFY(conversations.size() >= 2);
    QCOMPARE(requestCount(fixture, u"conversations.list"_s), 2);
    QCOMPARE(requestCount(fixture, u"conversations.history"_s), 0);
    const auto channel = conversations[0][0].value<Conversation>();
    QCOMPARE(channel.title, u"#general"_s);
    QCOMPARE(channel.id.remoteId, u"TFIXTURE/UOWNER/CGENERAL"_s);
    QCOMPARE(conversations[1][0].value<Conversation>().kind, ConversationKind::Direct);
    QVERIFY(!provider.sendText({ channel.id.accountId, u"TFIXTURE/UOTHER/CGENERAL"_s }, u"outside"_s));

    provider.requestHistory(channel.id);
    provider.requestHistory(channel.id);
    QTRY_VERIFY(messages.size() >= 2);
    QCOMPARE(messages[0][0].value<Message>().text, u"Earlier message"_s);
    const auto mention = messages[1][0].value<Message>();
    QVERIFY(mention.historical);
    QVERIFY(mention.mention);
    QCOMPARE(mention.text, u"Hello @owner & welcome"_s);

    QVERIFY(provider.sendText(channel.id, u"<@UOWNER> & plain"_s));
    QTRY_VERIFY(requestCount(fixture, u"chat.postMessage"_s) == 1);
    const auto posted = std::find_if(fixture.received.cbegin(), fixture.received.cend(),
        [](const auto &request) { return request.method() == u"chat.postMessage"_s; });
    QVERIFY(posted != fixture.received.cend());
    QCOMPARE(posted->headers.value("authorization"), QByteArray("Bearer xoxp-fixture-user"));
    const auto body = QJsonDocument::fromJson(posted->body).object();
    QCOMPARE(body.value(u"text"_s).toString(), u"&lt;@UOWNER&gt; &amp; plain"_s);
    QCOMPARE(body.value(u"channel"_s).toString(), u"CGENERAL"_s);

    QTRY_VERIFY(messages.size() >= 4);
    bool sawOutgoing = false;
    bool sawLive = false;
    for (const auto &args : messages) {
        const auto message = args[0].value<Message>();
        sawOutgoing |= message.outgoing && message.text == u"<@UOWNER> & plain"_s && !message.mention;
        QCOMPARE(message.delivery, message.outgoing ? DeliveryState::Sent : DeliveryState::None);
        sawLive |= !message.historical && message.text == u"Live reply"_s;
    }
    QVERIFY(sawOutgoing);
    QVERIFY(sawLive);
    const auto secondHistory
        = std::find_if(fixture.received.cbegin(), fixture.received.cend(), [](const auto &request) {
              return request.method() == u"conversations.history"_s
                  && QUrlQuery(request.url).hasQueryItem(u"oldest"_s);
          });
    QVERIFY(secondHistory != fixture.received.cend());
    QCOMPARE(QUrlQuery(secondHistory->url).queryItemValue(u"oldest"_s), u"1700000001.000001"_s);
}

void SlackTest::rejectsBotsAndRedirects()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    bool redirect = false;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        if (redirect && request.method() == u"auth.test"_s) {
            SlackFixture::respond(socket, {}, 302, "Location: /leak\r\n");
        } else {
            auto payload = identity();
            payload.insert(u"bot_id"_s, u"BBOT"_s);
            SlackFixture::respond(socket, payload);
        }
    };
    SlackProvider provider(slackAccount(), fixture.endpoints());
    QSignalSpy failures(&provider, &IChatProvider::errorOccurred);
    provider.connectAccount("xoxb-fixture-bot");
    QCOMPARE(failures.size(), 1);
    QCOMPARE(fixture.received.size(), 0);
    provider.connectAccount("xoxp-fixture-bot-disguised");
    QTRY_COMPARE(failures.size(), 2);
    QCOMPARE(fixture.received.size(), 1);

    redirect = true;
    provider.connectAccount("xoxp-fixture-redirect");
    QTRY_COMPARE(failures.size(), 3);
    QCOMPARE(fixture.received.size(), 2);
    QVERIFY(!failures.last().first().toString().contains(u"fixture-redirect"_s));
}

void SlackTest::waitsAfterRateLimit()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    int history = 0;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        const auto method = request.method();
        if (method == u"auth.test"_s) {
            SlackFixture::respond(socket, identity());
        } else if (method == u"conversations.list"_s) {
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"channels"_s,
                        QJsonArray { QJsonObject { { u"id"_s, u"CGENERAL"_s }, { u"is_member"_s, true },
                            { u"name"_s, u"general"_s } } } } });
        } else if (method == u"conversations.history"_s && ++history == 1) {
            SlackFixture::respond(socket, { { u"ok"_s, false } }, 429, "Retry-After: 1\r\n");
        } else {
            SlackFixture::respond(socket, { { u"ok"_s, true } });
        }
    };
    SlackProvider provider(slackAccount(), fixture.endpoints());
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    provider.connectAccount("xoxp-fixture-user");
    QTRY_COMPARE(conversations.size(), 1);
    const auto channel = conversations.first().first().value<Conversation>();
    provider.requestHistory(channel.id);
    QTRY_COMPARE(history, 1);
    QTest::qWait(250);
    provider.requestHistory(channel.id);
    QCOMPARE(history, 1);
    QTRY_VERIFY_WITH_TIMEOUT(history >= 2, 2000);
}

void SlackTest::profileUsesCachedUserAndConversationDetails()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        if (request.method() == u"auth.test"_s) {
            SlackFixture::respond(socket, identity());
        } else if (request.method() == u"conversations.list"_s) {
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"channels"_s,
                        QJsonArray { QJsonObject { { u"id"_s, u"DPRIVATE"_s }, { u"is_im"_s, true },
                                         { u"user"_s, u"UCOLLEAGUE"_s } },
                            QJsonObject { { u"id"_s, u"CGENERAL"_s }, { u"is_member"_s, true },
                                { u"name"_s, u"general"_s }, { u"num_members"_s, 12 },
                                { u"topic"_s, QJsonObject { { u"value"_s, u"Project updates"_s } } } } } } });
        } else if (request.method() == u"users.info"_s) {
            QCOMPARE(QUrlQuery(request.url).queryItemValue(u"user"_s), u"UCOLLEAGUE"_s);
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"user"_s,
                        QJsonObject { { u"id"_s, u"UCOLLEAGUE"_s }, { u"name"_s, u"colleague"_s },
                            { u"tz_label"_s, u"UTC"_s },
                            { u"profile"_s,
                                QJsonObject { { u"display_name"_s, u"Colleague"_s },
                                    { u"real_name"_s, u"Example Colleague"_s },
                                    { u"status_text"_s, u"Working remotely"_s },
                                    { u"title"_s, u"Engineer"_s } } } } } });
        }
    };
    SlackProvider provider(slackAccount(), fixture.endpoints());
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy profiles(&provider, &IChatProvider::profileReceived);
    provider.connectAccount("xoxp-fixture-user");
    QTRY_VERIFY(conversations.size() >= 2);
    const auto direct = conversations[0][0].value<Conversation>().id;
    const auto channel = conversations[1][0].value<Conversation>().id;
    QVERIFY(provider.requestProfile(direct));
    QTRY_COMPARE(profiles.size(), 1);
    auto details = profiles[0][1].value<ProfileDetails>();
    QVERIFY(details.contains({ u"Full name"_s, u"Example Colleague"_s }));
    QVERIFY(details.contains({ u"Status"_s, u"Working remotely"_s }));
    QVERIFY(!std::ranges::any_of(details, [](const auto &field) { return field.first == u"Email"_s; }));
    QVERIFY(provider.requestProfile(direct));
    QCOMPARE(profiles.size(), 2);
    QCOMPARE(requestCount(fixture, u"users.info"_s), 1);
    QVERIFY(provider.requestProfile(channel));
    QCOMPARE(profiles.size(), 3);
    details = profiles[2][1].value<ProfileDetails>();
    QVERIFY(details.contains({ u"Topic"_s, u"Project updates"_s }));
    QVERIFY(details.contains({ u"Members"_s, u"12"_s }));
    QVERIFY(!provider.requestProfile({ u"other-account"_s, direct.remoteId }));
    QCOMPARE(requestCount(fixture, u"users.info"_s), 1);
}

void SlackTest::uploadsFileWithoutForwardingToken()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(u"clip + sample.mp4"_s);
    const QByteArray content(128 * 1024, '\x35');
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
    file.close();
    bool uploaded = false;
    bool completed = false;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        const auto method = request.method();
        if (method == u"auth.test"_s) {
            SlackFixture::respond(socket, identity(), 200, "X-OAuth-Scopes: chat:write,files:write\r\n");
        } else if (method == u"conversations.list"_s) {
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"channels"_s,
                        QJsonArray { QJsonObject { { u"id"_s, u"CGENERAL"_s }, { u"is_member"_s, true },
                            { u"name"_s, u"general"_s } } } } });
        } else if (method == u"files.getUploadURLExternal"_s) {
            QCOMPARE(request.verb, QByteArray("POST"));
            QCOMPARE(
                request.fields().queryItemValue(u"filename"_s, QUrl::FullyDecoded), u"clip + sample.mp4"_s);
            QCOMPARE(request.fields().queryItemValue(u"length"_s).toInt(), content.size());
            QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer xoxp-fixture-user"));
            SlackFixture::respond(socket,
                { { u"ok"_s, true }, { u"file_id"_s, u"FFILE"_s },
                    { u"upload_url"_s,
                        fixture.endpoints().apiBase.resolved(QUrl(u"/upload/v1/fixture"_s)).toString() } });
        } else if (method == u"fixture"_s) {
            QVERIFY(!request.headers.contains("authorization"));
            QVERIFY(!request.headers.contains("cookie"));
            QCOMPARE(request.verb, QByteArray("POST"));
            QCOMPARE(request.body, content);
            uploaded = true;
            SlackFixture::respond(socket, { { u"ok"_s, true } });
        } else if (method == u"files.completeUploadExternal"_s) {
            QVERIFY(uploaded);
            QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer xoxp-fixture-user"));
            QCOMPARE(request.fields().queryItemValue(u"channel_id"_s), u"CGENERAL"_s);
            const auto files = QJsonDocument::fromJson(
                request.fields().queryItemValue(u"files"_s, QUrl::FullyDecoded).toUtf8())
                                   .array();
            QCOMPARE(files[0].toObject().value(u"id"_s).toString(), u"FFILE"_s);
            completed = true;
            SlackFixture::respond(socket, { { u"ok"_s, true }, { u"files"_s, files } });
        } else if (method == u"conversations.history"_s) {
            QVERIFY(completed);
            auto message = historyMessage(u"1700000004.000001"_s, {});
            message.insert(u"user"_s, u"UOWNER"_s);
            message.insert(u"subtype"_s, u"file_share"_s);
            message.insert(u"files"_s,
                QJsonArray {
                    QJsonObject { { u"id"_s, u"FFILE"_s }, { u"title"_s, u"clip + sample.mp4"_s } } });
            SlackFixture::respond(socket, { { u"ok"_s, true }, { u"messages"_s, QJsonArray { message } } });
        } else {
            SlackFixture::respond(socket, { { u"ok"_s, true } });
        }
    };
    SlackProvider provider(slackAccount(), fixture.endpoints());
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    QSignalSpy notices(&provider, &IChatProvider::notice);
    provider.connectAccount("xoxp-fixture-user");
    QTRY_COMPARE(conversations.size(), 1);
    const auto id = conversations[0][0].value<Conversation>().id;
    QVERIFY(provider.capabilities().testFlag(Capability::Files));
    QVERIFY(provider.sendFile(id, path));
    QCOMPARE(messages.size(), 0);
    QVERIFY(!provider.sendFile(id, path)); // One stream per account; no duplicate upload.
    QTRY_VERIFY(completed);
    QTRY_COMPARE(messages.size(), 1);
    QCOMPARE(messages[0][0].value<Message>().text, u"File: clip + sample.mp4"_s);
    QCOMPARE(messages[0][0].value<Message>().delivery, DeliveryState::Sent);
    QCOMPARE(messages[0][0].value<Message>().id, u"1700000004.000001"_s);
    QVERIFY(std::ranges::any_of(
        notices, [](const auto &args) { return args[0].toString() == u"File sent to Slack."_s; }));
    QCOMPARE(requestCount(fixture, u"files.completeUploadExternal"_s), 1);
}

void SlackTest::rejectsUnsafeUploadsAndMissingPermissions()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(u"sample.txt"_s);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("sample"), 6);
    file.close();
    enum Mode {
        MissingPermission,
        UnsafeUrl,
        MissingScopeResponse,
        Redirect,
        Disconnect
    } mode
        = MissingPermission;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        const auto method = request.method();
        if (method == u"auth.test"_s) {
            SlackFixture::respond(socket, identity(), 200,
                mode == MissingPermission ? QByteArray("X-OAuth-Scopes: chat:write\r\n") : QByteArray());
        } else if (method == u"conversations.list"_s) {
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"channels"_s,
                        QJsonArray { QJsonObject { { u"id"_s, u"CGENERAL"_s }, { u"is_member"_s, true },
                            { u"name"_s, u"general"_s } } } } });
        } else if (method == u"files.getUploadURLExternal"_s) {
            if (mode == MissingScopeResponse) {
                SlackFixture::respond(socket, { { u"ok"_s, false }, { u"error"_s, u"missing_scope"_s } });
            } else {
                const auto url = mode == UnsafeUrl
                    ? u"http://files.slack.com/upload/v1/unsafe"_s
                    : fixture.endpoints().apiBase.resolved(QUrl(u"/upload/v1/fixture"_s)).toString();
                SlackFixture::respond(
                    socket, { { u"ok"_s, true }, { u"file_id"_s, u"FFILE"_s }, { u"upload_url"_s, url } });
            }
        } else if (method == u"fixture"_s) {
            QVERIFY(!request.headers.contains("authorization"));
            if (mode == Redirect)
                SlackFixture::respond(socket, {}, 307, "Location: /leak\r\n");
            // Disconnect mode deliberately leaves the raw upload response pending.
        } else if (method == u"chat.postMessage"_s) {
            SlackFixture::respond(socket, { { u"ok"_s, true }, { u"ts"_s, u"1700000005.000001"_s } });
        } else {
            QFAIL("Unexpected API request or followed upload redirect");
        }
    };
    SlackProvider provider(slackAccount(), fixture.endpoints());
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy notices(&provider, &IChatProvider::notice);
    QSignalSpy authorization(&provider, &IChatProvider::authorizationUrlRequested);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    auto reconnect = [&] {
        provider.disconnectAccount();
        conversations.clear();
        provider.connectAccount("xoxp-fixture-user");
    };
    reconnect();
    QTRY_COMPARE(conversations.size(), 1);
    const auto id = conversations[0][0].value<Conversation>().id;
    QVERIFY(!provider.sendFile(id, path));
    QCOMPARE(requestCount(fixture, u"files.getUploadURLExternal"_s), 0);
    QVERIFY(notices.last()[0].toString().contains(u"files:write"_s));
    QVERIFY(provider.sendText(id, u"Messaging still works"_s));
    QTRY_COMPARE(messages.size(), 1);
    QCOMPARE(messages[0][0].value<Message>().delivery, DeliveryState::Sent);
    QCOMPARE(authorization.size(), 0);

    for (const auto testMode : { UnsafeUrl, MissingScopeResponse, Redirect }) {
        mode = testMode;
        reconnect();
        QTRY_COMPARE(conversations.size(), 1);
        QTest::qWait(660); // Public upload method quota is intentionally retained across reconnects.
        const auto previous = notices.size();
        QVERIFY(provider.sendFile(id, path));
        QTRY_COMPARE(notices.size(), previous + 1);
        QCOMPARE(requestCount(fixture, u"files.completeUploadExternal"_s), 0);
        if (mode == MissingScopeResponse)
            QVERIFY(notices.last()[0].toString().contains(u"files:write"_s));
    }
    mode = Disconnect;
    reconnect();
    QTRY_COMPARE(conversations.size(), 1);
    QTest::qWait(660);
    QVERIFY(provider.sendFile(id, path));
    QTRY_COMPARE(requestCount(fixture, u"fixture"_s), 2);
    provider.disconnectAccount();
    QTest::qWait(50);
    QCOMPARE(requestCount(fixture, u"files.completeUploadExternal"_s), 0);
    QCOMPARE(authorization.size(), 0);
}

void SlackTest::failedSendDoesNotClaimDelivery()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        if (request.method() == u"auth.test"_s) {
            SlackFixture::respond(socket, identity());
        } else if (request.method() == u"conversations.list"_s) {
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"channels"_s,
                        QJsonArray { QJsonObject { { u"id"_s, u"CGENERAL"_s }, { u"is_member"_s, true },
                            { u"name"_s, u"general"_s } } } } });
        } else if (request.method() == u"chat.postMessage"_s) {
            SlackFixture::respond(socket, { { u"ok"_s, false }, { u"error"_s, u"restricted_action"_s } });
        }
    };
    SlackProvider provider(slackAccount(), fixture.endpoints());
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    QSignalSpy notices(&provider, &IChatProvider::notice);
    provider.connectAccount("xoxp-fixture-user");
    QTRY_COMPARE(conversations.size(), 1);
    QVERIFY(provider.sendText(conversations[0][0].value<Conversation>().id, u"Unconfirmed"_s));
    QCOMPARE(messages.size(), 0);
    QTRY_COMPARE(notices.size(), 1);
    QCOMPARE(messages.size(), 0);
}

void SlackTest::presenceOnlyPollsOpenDirectConversations()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    int checks = 0;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        const auto method = request.method();
        if (method == u"auth.test"_s) {
            SlackFixture::respond(socket, identity(), 200, "X-OAuth-Scopes: users:read,chat:write\r\n");
        } else if (method == u"conversations.list"_s) {
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"channels"_s,
                        QJsonArray { QJsonObject { { u"id"_s, u"DPRIVATE"_s }, { u"is_im"_s, true },
                                         { u"user"_s, u"UCOLLEAGUE"_s } },
                            QJsonObject { { u"id"_s, u"DUNOPENED"_s }, { u"is_im"_s, true },
                                { u"user"_s, u"UOTHER"_s } },
                            QJsonObject { { u"id"_s, u"CGENERAL"_s }, { u"is_member"_s, true },
                                { u"name"_s, u"general"_s } } } } });
        } else if (method == u"users.getPresence"_s) {
            QCOMPARE(QUrlQuery(request.url).queryItemValue(u"user"_s), u"UCOLLEAGUE"_s);
            QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer xoxp-fixture-user"));
            ++checks;
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"presence"_s,
                        checks == 1       ? u"active"_s
                            : checks == 2 ? u"away"_s
                                          : u"unexpected"_s } });
        } else {
            SlackFixture::respond(socket, { { u"ok"_s, true } });
        }
    };
    auto endpoints = fixture.endpoints();
    endpoints.presenceIntervalMs = 60;
    SlackProvider provider(slackAccount(), endpoints);
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy presence(&provider, &IChatProvider::presenceChanged);
    provider.connectAccount("xoxp-fixture-user");
    QTRY_VERIFY(conversations.size() >= 3);
    QCOMPARE(checks, 0);
    provider.setConversationOpen({ u"local-slack"_s, u"TFIXTURE/UOWNER/CGENERAL"_s }, true);
    QTest::qWait(100);
    QCOMPARE(checks, 0);
    const ConversationId id { u"local-slack"_s, u"TFIXTURE/UOWNER/DPRIVATE"_s };
    provider.setConversationOpen(id, true);
    QTRY_COMPARE(presence.size(), 1);
    QCOMPARE(presence[0][0].value<ConversationId>(), id);
    QCOMPARE(presence[0][1].value<Presence>(), Presence::Online);
    QTRY_COMPARE(presence.size(), 2);
    QCOMPARE(presence[1][1].value<Presence>(), Presence::Away);
    QTRY_COMPARE(presence.size(), 3);
    QCOMPARE(presence[2][1].value<Presence>(), Presence::Unknown);
    provider.setConversationOpen(id, false);
    QTest::qWait(1450);
    QCOMPARE(checks, 3);
    for (const auto &event : presence)
        QVERIFY(event[1].value<Presence>() != Presence::Offline);
}

void SlackTest::presenceHandlesMissingScopeAndLateReplies()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    bool knownMissingScope = true;
    int checks = 0;
    QPointer<QTcpSocket> heldReply;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        const auto method = request.method();
        if (method == u"auth.test"_s) {
            SlackFixture::respond(socket, identity(), 200,
                knownMissingScope ? QByteArray("X-OAuth-Scopes: chat:write\r\n") : QByteArray());
        } else if (method == u"conversations.list"_s) {
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"channels"_s,
                        QJsonArray { QJsonObject { { u"id"_s, u"DPRIVATE"_s }, { u"is_im"_s, true },
                            { u"user"_s, u"UCOLLEAGUE"_s } } } } });
        } else if (method == u"users.getPresence"_s) {
            ++checks;
            if (checks == 1)
                heldReply = &socket;
            else
                SlackFixture::respond(socket, { { u"ok"_s, false }, { u"error"_s, u"missing_scope"_s } });
        } else {
            SlackFixture::respond(socket, { { u"ok"_s, true } });
        }
    };
    auto endpoints = fixture.endpoints();
    endpoints.presenceIntervalMs = 60;
    SlackProvider provider(slackAccount(), endpoints);
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy presence(&provider, &IChatProvider::presenceChanged);
    QSignalSpy authorization(&provider, &IChatProvider::authorizationUrlRequested);
    const ConversationId id { u"local-slack"_s, u"TFIXTURE/UOWNER/DPRIVATE"_s };
    provider.setConversationOpen(id, true); // Restored windows can precede authentication/discovery.
    provider.connectAccount("xoxp-fixture-user");
    QTRY_VERIFY(!conversations.isEmpty());
    QTest::qWait(100);
    QCOMPARE(checks, 0);
    provider.disconnectAccount();
    knownMissingScope = false;
    provider.connectAccount("xoxp-fixture-user");
    QTRY_VERIFY(heldReply);
    provider.setConversationOpen(id, false);
    SlackFixture::respond(*heldReply, { { u"ok"_s, true }, { u"presence"_s, u"active"_s } });
    QTest::qWait(100);
    QCOMPARE(presence.size(), 0); // Closing while a request is in flight never publishes stale presence.
    provider.setConversationOpen(id, true);
    QTRY_COMPARE(checks, 2);
    QTest::qWait(1450);
    QCOMPARE(checks, 2); // Permission failure stops presence polling without disturbing authorization.
    QCOMPARE(presence.size(), 0);
    QCOMPARE(authorization.size(), 0);
}

void SlackTest::presenceHonorsRateLimit()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    int checks = 0;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        const auto method = request.method();
        if (method == u"auth.test"_s) {
            SlackFixture::respond(socket, identity());
        } else if (method == u"conversations.list"_s) {
            SlackFixture::respond(socket,
                { { u"ok"_s, true },
                    { u"channels"_s,
                        QJsonArray { QJsonObject { { u"id"_s, u"DPRIVATE"_s }, { u"is_im"_s, true },
                            { u"user"_s, u"UCOLLEAGUE"_s } } } } });
        } else if (method == u"users.getPresence"_s) {
            if (++checks == 1)
                SlackFixture::respond(socket, { { u"ok"_s, false } }, 429, "Retry-After: 2\r\n");
            else
                SlackFixture::respond(socket, { { u"ok"_s, true }, { u"presence"_s, u"active"_s } });
        } else {
            SlackFixture::respond(socket, { { u"ok"_s, true } });
        }
    };
    auto endpoints = fixture.endpoints();
    endpoints.presenceIntervalMs = 60;
    SlackProvider provider(slackAccount(), endpoints);
    QSignalSpy presence(&provider, &IChatProvider::presenceChanged);
    provider.setConversationOpen({ u"local-slack"_s, u"TFIXTURE/UOWNER/DPRIVATE"_s }, true);
    provider.connectAccount("xoxp-fixture-user");
    QTRY_COMPARE(checks, 1);
    QTest::qWait(1450);
    QCOMPARE(checks, 1);
    QCOMPARE(presence.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(presence.size(), 1, 1500);
    QCOMPARE(presence[0][1].value<Presence>(), Presence::Online);
    provider.disconnectAccount();
    QTest::qWait(100);
    QCOMPARE(checks, 2);
}

QTEST_GUILESS_MAIN(SlackTest)
#include "test_slack.moc"
