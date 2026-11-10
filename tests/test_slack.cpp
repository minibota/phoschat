#include "providers/slack/slack_provider.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
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
    void oauthLoopbackValidatesStateAndRefreshes();
    void listsPollsAndSendsUserMessages();
    void rejectsBotsAndRedirects();
    void waitsAfterRateLimit();
};

void SlackTest::oauthLoopbackValidatesStateAndRefreshes()
{
    SlackFixture fixture;
    QVERIFY(fixture.listen());
    int grants = 0;
    fixture.handler = [&](QTcpSocket &socket, const Request &request) {
        if (request.method() == u"oauth.v2.access"_s) {
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

    SlackProvider provider(slackAccount(), fixture.endpoints());
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
    QVERIFY(challenge.queryItemValue(u"scope"_s).isEmpty());
    QVERIFY(!challenge.hasQueryItem(u"client_secret"_s));
    const auto state = challenge.queryItemValue(u"state"_s);
    QVERIFY(state.size() >= 43);
    QUrl callback(challenge.queryItemValue(u"redirect_uri"_s));
    QCOMPARE(callback.host(), u"localhost"_s);
    const auto registeredRedirect = callback.toString();
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
    stored.insert(u"expires_at_ms"_s, QDateTime::currentMSecsSinceEpoch() - 1000);
    provider.disconnectAccount();
    states.clear();
    provider.connectAccount(QJsonDocument(stored).toJson(QJsonDocument::Compact));
    QTRY_VERIFY(hasState(states, ConnectionState::Connected));
    QCOMPARE(grants, 2);
    QCOMPARE(credentials.size(), 2);
    const auto refresh = QJsonDocument::fromJson(credentials.last().first().toByteArray()).object();
    QCOMPARE(refresh.value(u"refresh"_s).toString(), u"xoxe-fixture-new"_s);
    const auto lastExchange = std::find_if(fixture.received.crbegin(), fixture.received.crend(),
        [](const auto &request) { return request.method() == u"oauth.v2.access"_s; });
    QVERIFY(lastExchange != fixture.received.crend());
    QCOMPARE(lastExchange->fields().queryItemValue(u"refresh_token"_s), u"xoxe-fixture-old"_s);
    QVERIFY(!lastExchange->fields().hasQueryItem(u"client_secret"_s));
    QVERIFY(!lastExchange->headers.contains("authorization"));
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

QTEST_GUILESS_MAIN(SlackTest)
#include "test_slack.moc"
