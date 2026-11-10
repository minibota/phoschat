#include "providers/irc/irc_message.h"
#include "providers/irc/irc_provider.h"

#include <QFile>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <algorithm>

using namespace phoschat;

namespace {

class IrcLoopbackServer final : public QTcpServer {
public:
    explicit IrcLoopbackServer(QSslConfiguration configuration)
        : configuration_(std::move(configuration))
    {
        setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    }

    void send(const QByteArray &bytes) { peer_->write(bytes); }

    void hangUp() { peer_->abort(); }

    QString registrationDiagnostic(const QSignalSpy &errors) const
    {
        const auto clientError
            = errors.isEmpty() ? QStringLiteral("none reported") : errors.constLast().constFirst().toString();
        const auto backend
            = configuration_.isNull() ? QStringLiteral("plain TCP") : QSslSocket::activeBackend();
        return QStringLiteral("Received %1 registration commands, accepted peer %2, backend %3, "
                              "peer encrypted %4, client error: %5, peer error: %6")
            .arg(commands.size())
            .arg(peer_ ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(backend)
            .arg(peerEncrypted_ ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(clientError)
            .arg(peerError_.isEmpty() ? QStringLiteral("none reported") : peerError_);
    }

    bool registrationFinishedOrFailed(const QSignalSpy &errors) const
    {
        return commands.size() == 3 || !errors.isEmpty() || !peerError_.isEmpty();
    }

    QList<QByteArray> commands;

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *tlsPeer = configuration_.isNull() ? nullptr : new QSslSocket(this);
        auto *peer = tlsPeer ? static_cast<QTcpSocket *>(tlsPeer) : new QTcpSocket(this);
        if (!peer->setSocketDescriptor(descriptor)) {
            peerError_ = peer->errorString();
            peer->deleteLater();
            return;
        }
        peer_ = peer;
        connect(peer, &QAbstractSocket::errorOccurred, this, [this, peer] {
            if (peerError_.isEmpty()) {
                peerError_ = peer->errorString();
            }
        });
        connect(peer, &QIODevice::readyRead, this, [this, peer] {
            while (peer->canReadLine()) {
                auto line = peer->readLine();
                if (line.endsWith("\r\n")) {
                    line.chop(2);
                }
                commands.append(line);
            }
        });
        if (tlsPeer) {
            connect(tlsPeer, &QSslSocket::encrypted, this, [this] { peerEncrypted_ = true; });
            connect(tlsPeer, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
                if (!errors.isEmpty()) {
                    peerError_ = errors.constFirst().errorString();
                }
            });
            tlsPeer->setSslConfiguration(configuration_);
            tlsPeer->startServerEncryption();
        }
    }

private:
    QSslConfiguration configuration_;
    QTcpSocket *peer_ = nullptr;
    QString peerError_;
    bool peerEncrypted_ = false;
};

QByteArray readFixture(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}

Account loopbackAccount(quint16 port, bool tls = true)
{
    Account account;
    account.id = QStringLiteral("test-network");
    account.host = QStringLiteral("127.0.0.1");
    account.port = port;
    account.nick = QStringLiteral("Tester");
    account.tls = tls;
    return account;
}

ConnectionState latestState(const QSignalSpy &states)
{
    return states.isEmpty() ? ConnectionState::Disconnected
                            : states.constLast().first().value<ConnectionState>();
}

} // namespace

class IrcTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void parsesAndBoundsFrames();
    void foldsNamesAndStripsFormatting();
    void legacyPlaintextRegistration();
    void listsPublicChannels();
    void boundsChannelDirectory();
    void searchesCuratedDirectory();
    void negotiatesTlsSaslAndExchangesMessages();
    void rejectsInvalidCertificates_data();
    void rejectsInvalidCertificates();
    void rejectsPlaintextSasl();
    void reportsUnavailableSasl();
};

void IrcTest::initTestCase()
{
#ifdef Q_OS_MACOS
    // Secure Transport imports the fixture key. Keep it out of the user's keychain.
    QVERIFY(qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1"));
#endif
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));
}

void IrcTest::parsesAndBoundsFrames()
{
    irc::FrameDecoder decoder;
    QVERIFY(decoder.append("@example.org/data=first;example.org/data=a\\:b\\sc\\\\d\\r\\n\\z\\ ").isEmpty());
    QVERIFY(decoder.append(":nick!u@h PRIVMSG #room :hello wor").isEmpty());
    const auto decoded = decoder.append("ld\r\nPING :cookie\r\n");
    QCOMPARE(decoded.size(), 2);
    QCOMPARE(decoded.first().command, QByteArray("PRIVMSG"));
    QCOMPARE(decoded.first().nickname(), QStringLiteral("nick"));
    QCOMPARE(decoded.first().parameters, QList<QByteArray>({ "#room", "hello world" }));
    QCOMPARE(decoded.first().tags.value("example.org/data"), QStringLiteral("a;b c\\d\r\nz"));
    QCOMPARE(decoded.last().parameters.constFirst(), QByteArray("cookie"));

    const QByteArray maximumBody = "NOTICE nick :" + QByteArray(510 - 13, 'a');
    QCOMPARE(maximumBody.size(), 510);
    QVERIFY(irc::parseMessage(maximumBody));
    QVERIFY(!irc::parseMessage(maximumBody + 'x'));
    const QByteArray maximumTags = "@x=" + QByteArray(8191 - 4, 't') + ' ';
    QCOMPARE(maximumTags.size(), 8191);
    QVERIFY(irc::parseMessage(maximumTags + maximumBody));
    QVERIFY(!irc::parseMessage("@x=z" + maximumTags.mid(3) + maximumBody));
    QVERIFY(!irc::parseMessage(QByteArray("PRIVMSG x :a\0b", 14)));
    QVERIFY(!irc::parseMessage("PRIVMSG x :a\rb"));
    QVERIFY(!irc::parseMessage(":broken"));
    QVERIFY(!irc::parseMessage("!unexpected x"));
    const auto manyParameters = irc::parseMessage("005" + QByteArray(" x").repeated(16) + " :supported");
    QVERIFY(manyParameters);
    QCOMPARE(manyParameters->parameters.size(), 17);
    QCOMPARE(manyParameters->parameters.last(), QByteArray("supported"));
    const auto invalidTagUtf8 = irc::parseMessage("@x=prefix\xe2  PING :test");
    QVERIFY(invalidTagUtf8);
    QVERIFY(invalidTagUtf8->tags.value("x").isEmpty());
    QVERIFY(decoder.append("PING :invalid-lf\n").isEmpty());

    QVERIFY(decoder.append(QByteArray(9000, 'x')).isEmpty());
    QVERIFY(decoder.append("PING :discarded").isEmpty());
    const auto recovered = decoder.append("\r\nPING :kept\r\n");
    QCOMPARE(recovered.size(), 1);
    QCOMPARE(recovered.first().parameters.constFirst(), QByteArray("kept"));
}

void IrcTest::foldsNamesAndStripsFormatting()
{
    const auto name = QStringLiteral("#Example[\\]^");
    QCOMPARE(irc::caseFold(name, irc::CaseMapping::Ascii), QStringLiteral("#example[\\]^"));
    QCOMPARE(irc::caseFold(name, irc::CaseMapping::StrictRfc1459), QStringLiteral("#example{|}^"));
    QCOMPARE(irc::caseFold(name, irc::CaseMapping::Rfc1459), QStringLiteral("#example{|}~"));
    QCOMPARE(irc::plainText(QStringLiteral("\x02"
                                           "bold"
                                           "\x02"
                                           " \x03"
                                           "12,02color \x04"
                                           "ABCDEF,000000hex <b>literal</b>")),
        QStringLiteral("bold color hex <b>literal</b>"));
}

void IrcTest::legacyPlaintextRegistration()
{
    IrcLoopbackServer server({});
    QVERIFY(server.listen(QHostAddress::LocalHost));
    IrcProvider provider(loopbackAccount(server.serverPort(), false));
    QSignalSpy states(&provider, &IChatProvider::stateChanged);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);

    provider.connectAccount({});
    QTRY_VERIFY2_WITH_TIMEOUT(server.commands.size() == 3 || !errors.isEmpty(),
        qPrintable(server.registrationDiagnostic(errors)), 3000);
    QVERIFY2(errors.isEmpty(), qPrintable(server.registrationDiagnostic(errors)));
    QCOMPARE(server.commands.at(0), QByteArray("CAP LS 302"));
    QCOMPARE(server.commands.at(1), QByteArray("NICK Tester"));
    server.send(":irc 421 Tester CAP :Unknown command\r\n"
                ":irc 001 Tester :Welcome\r\n"
                ":irc 422 Tester :No MOTD\r\n"
                "PING :while-online\r\n");
    QTRY_COMPARE(latestState(states), ConnectionState::Connected);
    QTRY_VERIFY(server.commands.contains("PONG :while-online"));
    QVERIFY(errors.isEmpty());
    QVERIFY(!provider.capabilities().testFlag(Capability::RemoteHistory));
    QVERIFY(!provider.capabilities().testFlag(Capability::Presence));
    provider.disconnectAccount();
    QCOMPARE(latestState(states), ConnectionState::Disconnected);
    QTRY_VERIFY(server.commands.contains("QUIT :PhosChat closing"));
}

void IrcTest::listsPublicChannels()
{
    IrcLoopbackServer server({});
    QVERIFY(server.listen(QHostAddress::LocalHost));
    IrcProvider provider(loopbackAccount(server.serverPort(), false));
    QSignalSpy states(&provider, &IChatProvider::stateChanged);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);
    QSignalSpy updates(&provider, &IrcProvider::channelListChanged);
    QVERIFY(!provider.requestChannelList().isEmpty());

    provider.connectAccount({});
    QTRY_VERIFY2_WITH_TIMEOUT(server.commands.size() == 3 || !errors.isEmpty(),
        qPrintable(server.registrationDiagnostic(errors)), 3000);
    QVERIFY2(errors.isEmpty(), qPrintable(server.registrationDiagnostic(errors)));
    server.send(":irc 421 Tester CAP :Unknown command\r\n"
                ":irc 001 Tester :Welcome\r\n"
                ":irc 422 Tester :No MOTD\r\n"
                ":irc 322 Tester #unsolicited 9 :Outside a request\r\n");
    QTRY_COMPARE(latestState(states), ConnectionState::Connected);
    QVERIFY(provider.channelList().isEmpty());
    QVERIFY(provider.requestChannelList().isEmpty());
    QCOMPARE(provider.channelListStatus(), irc::ChannelListStatus::Loading);
    QVERIFY(provider.channelListBusy());
    QVERIFY(!provider.requestChannelList().isEmpty());
    QTRY_VERIFY(server.commands.contains("LIST"));
    QCOMPARE(server.commands.count("LIST"), 1);
    server.send(":irc 322 Tester #opensource 91 :Open ");
    server.send("source \x02"
                "tools\x02"
                "\r\n"
                ":irc 322 Tester #OPENSOURCE 2 :Duplicate\r\n"
                ":irc 322 Other #other 8 :Different recipient\r\n"
                ":irc 322 Tester #invalid negative :Bad count\r\n"
                ":irc 322 Tester * 5 :Anonymous entry\r\n"
                ":irc 322 Tester #music 15 :Independent artists\r\n");
    QTRY_VERIFY(updates.size() >= 2);
    QCOMPARE(provider.channelList().size(), 2);
    QCOMPARE(provider.channelListStatus(), irc::ChannelListStatus::Loading);
    QCOMPARE(provider.channelList().first().name, QStringLiteral("#opensource"));
    QCOMPARE(provider.channelList().first().users, 91);
    QCOMPARE(provider.channelList().first().topic, QStringLiteral("Open source tools"));
    server.send(":irc 323 Tester :End of LIST\r\n"
                ":irc 322 Tester #late 12 :Outside a request\r\n");
    QTRY_COMPARE(provider.channelListStatus(), irc::ChannelListStatus::Ready);
    QVERIFY(!provider.channelListBusy());
    QCOMPARE(provider.channelList().size(), 2);
    QVERIFY(!provider.requestChannelList().isEmpty());
}

void IrcTest::boundsChannelDirectory()
{
    IrcLoopbackServer server({});
    QVERIFY(server.listen(QHostAddress::LocalHost));
    IrcProvider provider(loopbackAccount(server.serverPort(), false));
    QSignalSpy states(&provider, &IChatProvider::stateChanged);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);
    provider.connectAccount({});
    QTRY_VERIFY2_WITH_TIMEOUT(server.commands.size() == 3 || !errors.isEmpty(),
        qPrintable(server.registrationDiagnostic(errors)), 3000);
    QVERIFY2(errors.isEmpty(), qPrintable(server.registrationDiagnostic(errors)));
    server.send(":irc 421 Tester CAP :Unknown command\r\n"
                ":irc 001 Tester :Welcome\r\n"
                ":irc 422 Tester :No MOTD\r\n");
    QTRY_COMPARE(latestState(states), ConnectionState::Connected);
    QVERIFY(provider.requestChannelList().isEmpty());
    QTRY_VERIFY(server.commands.contains("LIST"));

    QByteArray listing;
    for (int index = 0; index < 20001; ++index) {
        listing += ":irc 322 Tester #room" + QByteArray::number(index) + " 1 :topic\r\n";
    }
    listing += ":irc 323 Tester :End of LIST\r\n";
    server.send(listing);
    QTRY_COMPARE(provider.channelListStatus(), irc::ChannelListStatus::Truncated);
    QCOMPARE(provider.channelList().size(), 20000);
    QVERIFY(!provider.channelListBusy());
}

void IrcTest::searchesCuratedDirectory()
{
    const auto networks = irc::recommendedNetworks();
    QCOMPARE(networks.size(), 3);
    const auto suggestions = irc::recommendedChannels();
    const auto spanish = std::ranges::find(suggestions, QStringView(u"#pyar"), &irc::ChannelSuggestion::name);
    QVERIFY(spanish != suggestions.end());
    QVERIFY(irc::matchesDirectorySearch(
        u"  ESPAÑOL\tpython ", { spanish->name, spanish->category, spanish->description }));
    QVERIFY(!irc::matchesDirectorySearch(
        u"python French", { spanish->name, spanish->category, spanish->description }));
    QVERIFY(irc::matchesDirectorySearch(u"  ", { spanish->name }));
    for (const auto &suggestion : suggestions) {
        QVERIFY(std::ranges::find(networks, suggestion.networkHost, &irc::NetworkSuggestion::host)
            != networks.end());
        QVERIFY(suggestion.source.startsWith(u"https://"));
    }
}

void IrcTest::negotiatesTlsSaslAndExchangesMessages()
{
    QVERIFY2(QSslSocket::supportsSsl(), "A working Qt TLS backend is required for this integration test.");
    const auto ca = QSslCertificate(readFixture(QFINDTESTDATA("fixtures/irc-test-ca.pem")));
    const auto certificate = QSslCertificate(readFixture(QFINDTESTDATA("fixtures/irc-test-server.pem")));
    const auto key = QSslKey(readFixture(QFINDTESTDATA("fixtures/irc-test-server-key.pem")), QSsl::Rsa);
    QVERIFY(!ca.isNull());
    QVERIFY(!certificate.isNull());
    QVERIFY(!key.isNull());

    auto serverTls = QSslConfiguration::defaultConfiguration();
    serverTls.setLocalCertificate(certificate);
    serverTls.setPrivateKey(key);
    serverTls.setPeerVerifyMode(QSslSocket::VerifyNone);
    IrcLoopbackServer server(serverTls);
    QVERIFY(server.listen(QHostAddress::LocalHost));
    auto account = loopbackAccount(server.serverPort());
    account.saslUser = QStringLiteral("demo");

    auto trustedTls = QSslConfiguration::defaultConfiguration();
    trustedTls.setCaCertificates({ ca });
    IrcProvider provider(account, trustedTls);
    QSignalSpy states(&provider, &IChatProvider::stateChanged);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);
    QSignalSpy notices(&provider, &IChatProvider::notice);
    QSignalSpy conversations(&provider, &IChatProvider::conversationChanged);
    QSignalSpy messages(&provider, &IChatProvider::messageReceived);
    QSignalSpy members(&provider, &IChatProvider::membersChanged);

    const QByteArray secret(294, 'x'); // 300-byte PLAIN payload becomes exactly one 400-byte chunk.
    provider.connectAccount(secret);
    QTRY_VERIFY2_WITH_TIMEOUT(
        server.registrationFinishedOrFailed(errors), qPrintable(server.registrationDiagnostic(errors)), 5000);
    QVERIFY2(
        server.commands.size() == 3 && errors.isEmpty(), qPrintable(server.registrationDiagnostic(errors)));
    QCOMPARE(server.commands.at(0), QByteArray("CAP LS 302"));
    QCOMPARE(server.commands.at(2), QByteArray("USER phoschat 0 * :PhosChat"));
    server.send(":irc CAP * LS * :message-tags sasl=PLAIN,EXTERNAL\r\n");
    QTest::qWait(30);
    QCOMPARE(server.commands.size(), 3);
    server.send(":irc CAP * LS :server-time multi-prefix draft/chathistory\r\n");
    QTRY_VERIFY(server.commands.contains("CAP REQ :sasl"));
    QTRY_VERIFY(server.commands.contains("CAP REQ :message-tags server-time multi-prefix"));
    QVERIFY(!server.commands.contains("CAP END"));

    server.send(":irc CAP * ACK * :message-tags server-time\r\n"
                ":irc CAP * ACK :multi-prefix\r\n"
                ":irc CAP * ACK :sasl\r\n"
                "PING :registration\r\n");
    QTRY_VERIFY(server.commands.contains("AUTHENTICATE PLAIN"));
    QTRY_VERIFY(server.commands.contains("PONG :registration"));
    QCOMPARE(latestState(states), ConnectionState::Authenticating);
    server.send("AUTHENTICATE +\r\n");
    QTRY_VERIFY(server.commands.contains("AUTHENTICATE +"));
    const auto chunk = server.commands.at(server.commands.indexOf("AUTHENTICATE +") - 1);
    QVERIFY(chunk.startsWith("AUTHENTICATE "));
    QCOMPARE(chunk.sliced(13).size(), 400);
    QCOMPARE(QByteArray::fromBase64(chunk.sliced(13)), QByteArray("\0demo\0", 6) + secret);
    QVERIFY(!server.commands.contains("CAP END"));

    server.send(
        ":irc 903 Tester :SASL successful\r\n"
        ":irc 001 Tester :Welcome\r\n"
        ":irc 005 Tester CASEMAPPING=ascii CHANTYPES=#& PREFIX=(qaohv)~&@%+ STATUSMSG=@ :supported\r\n"
        ":irc 376 Tester :End of MOTD\r\n");
    QTRY_VERIFY(server.commands.contains("CAP END"));
    QTRY_COMPARE(latestState(states), ConnectionState::Connected);
    QCOMPARE(provider.canonicalTarget(QStringLiteral("#Room[")), QStringLiteral("#room["));
    QVERIFY(provider.join(QStringLiteral("#Room")));
    QVERIFY(provider.join(QStringLiteral("#Other")));
    QTRY_VERIFY(server.commands.contains("JOIN #Room"));
    QTRY_VERIFY(server.commands.contains("JOIN #Other"));

    server.send(":Tester!u@host JOIN #Room\r\n"
                ":Tester!u@host JOIN #Other\r\n"
                ":irc 353 Tester = #Room :@+Tester %Alice\r\n"
                ":irc 353 Tester = #Room :Bob\r\n"
                ":irc 366 Tester #Room :End of NAMES\r\n"
                "@msgid=chat-1;time=2024-07-05T09:01:02.123Z :Alice!u@host PRIVMSG @#ROOM :Hi Tes");
    QTRY_COMPARE(conversations.size(), 2);
    QTRY_COMPARE(members.constLast().at(1).toStringList(),
        QStringList({ QStringLiteral("Alice"), QStringLiteral("Bob"), QStringLiteral("Tester") }));
    QVERIFY(messages.isEmpty());
    server.send("ter!\r\n:Bob!u@host PRIVMSG Tester :<b>private</b>\r\n"
                ":Bob!u@host PRIVMSG Tester :\x01"
                "VERSION\x01\r\n"
                ":Bob!u@host NICK :Robert\r\n");
    QTRY_COMPARE(messages.size(), 2);
    const auto channelMessage = messages.at(0).at(0).value<Message>();
    QCOMPARE(channelMessage.conversation, ConversationId({ account.id, QStringLiteral("#room") }));
    QCOMPARE(channelMessage.id, QStringLiteral("remote:chat-1"));
    QCOMPARE(channelMessage.text, QStringLiteral("Hi Tester!"));
    QCOMPARE(channelMessage.timestamp,
        QDateTime::fromString(QStringLiteral("2024-07-05T09:01:02.123Z"), Qt::ISODateWithMs));
    QVERIFY(channelMessage.mention);
    const auto directMessage = messages.at(1).at(0).value<Message>();
    QCOMPARE(directMessage.conversation.remoteId, QStringLiteral("bob"));
    QCOMPARE(directMessage.text, QStringLiteral("<b>private</b>"));
    QTRY_COMPARE(members.constLast().at(1).toStringList(),
        QStringList({ QStringLiteral("Alice"), QStringLiteral("Robert"), QStringLiteral("Tester") }));

    QVERIFY(provider.sendText(channelMessage.conversation, QStringLiteral("Hello from PhosChat")));
    QTRY_VERIFY(server.commands.contains("PRIVMSG #room :Hello from PhosChat"));
    QCOMPARE(messages.size(), 3);
    const auto outgoing = messages.constLast().at(0).value<Message>();
    QVERIFY(outgoing.outgoing);
    QVERIFY(outgoing.id.startsWith(QStringLiteral("local:")));
    QVERIFY(!provider.sendText(channelMessage.conversation, QStringLiteral("x\r\nJOIN #other")));
    QVERIFY(!provider.sendText(channelMessage.conversation, QString(300, u'é')));
    QCOMPARE(notices.size(), 2);
    QVERIFY(provider.sendCommand(channelMessage.conversation, QStringLiteral("/me waves")));
    QTRY_VERIFY(server.commands.contains("PRIVMSG #room :\x01"
                                         "ACTION waves\x01"));

    QVERIFY(provider.part(QStringLiteral("#Room")));
    QTRY_VERIFY(server.commands.contains("PART #Room"));
    server.send(":Tester!u@host PART #Room :Leaving\r\n");
    QTRY_VERIFY(!conversations.constLast().at(0).value<Conversation>().joined);
    QVERIFY(!provider.sendText(channelMessage.conversation, QStringLiteral("Cannot send after PART")));
    QVERIFY(errors.isEmpty());

    server.hangUp();
    QTRY_COMPARE(latestState(states), ConnectionState::Error);
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.constFirst().at(1).toBool(), true);
}

void IrcTest::rejectsInvalidCertificates_data()
{
    QTest::addColumn<bool>("trustFixture");
    QTest::newRow("untrusted-ca") << false;
    QTest::newRow("wrong-hostname") << true;
}

void IrcTest::rejectsInvalidCertificates()
{
    QFETCH(bool, trustFixture);
    QVERIFY(QSslSocket::supportsSsl());
    const auto certificatePath = trustFixture ? QFINDTESTDATA("fixtures/irc-test-wrong-host.pem")
                                              : QFINDTESTDATA("fixtures/irc-test-server.pem");
    auto serverTls = QSslConfiguration::defaultConfiguration();
    serverTls.setLocalCertificate(QSslCertificate(readFixture(certificatePath)));
    serverTls.setPrivateKey(
        QSslKey(readFixture(QFINDTESTDATA("fixtures/irc-test-server-key.pem")), QSsl::Rsa));
    serverTls.setPeerVerifyMode(QSslSocket::VerifyNone);
    QVERIFY(!serverTls.localCertificate().isNull());
    QVERIFY(!serverTls.privateKey().isNull());

    IrcLoopbackServer server(serverTls);
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto account = loopbackAccount(server.serverPort());
    auto clientTls = QSslConfiguration::defaultConfiguration();
    if (trustFixture) {
        const auto ca = QSslCertificate(readFixture(QFINDTESTDATA("fixtures/irc-test-ca.pem")));
        QVERIFY(!ca.isNull());
        clientTls.setCaCertificates({ ca });
    }
    IrcProvider provider(account, clientTls);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);
    provider.connectAccount({});
    QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 5000);
    QCOMPARE(errors.constFirst().at(1).toBool(), false);
    QVERIFY(server.commands.isEmpty());
}

void IrcTest::rejectsPlaintextSasl()
{
    Account account = loopbackAccount(6667, false);
    account.saslUser = QStringLiteral("demo");
    IrcProvider provider(account);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);
    provider.connectAccount("test-only");
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.constFirst().at(1).toBool(), false);
}

void IrcTest::reportsUnavailableSasl()
{
    QVERIFY(QSslSocket::supportsSsl());
    auto serverTls = QSslConfiguration::defaultConfiguration();
    serverTls.setLocalCertificate(
        QSslCertificate(readFixture(QFINDTESTDATA("fixtures/irc-test-server.pem"))));
    serverTls.setPrivateKey(
        QSslKey(readFixture(QFINDTESTDATA("fixtures/irc-test-server-key.pem")), QSsl::Rsa));
    serverTls.setPeerVerifyMode(QSslSocket::VerifyNone);
    IrcLoopbackServer server(serverTls);
    QVERIFY(server.listen(QHostAddress::LocalHost));
    auto account = loopbackAccount(server.serverPort());
    account.saslUser = QStringLiteral("demo");
    auto trustedTls = QSslConfiguration::defaultConfiguration();
    trustedTls.setCaCertificates({ QSslCertificate(readFixture(QFINDTESTDATA("fixtures/irc-test-ca.pem"))) });

    IrcProvider provider(account, trustedTls);
    QSignalSpy errors(&provider, &IChatProvider::errorOccurred);
    provider.connectAccount("test-only");
    QTRY_VERIFY2_WITH_TIMEOUT(
        server.registrationFinishedOrFailed(errors), qPrintable(server.registrationDiagnostic(errors)), 5000);
    QVERIFY2(
        server.commands.size() == 3 && errors.isEmpty(), qPrintable(server.registrationDiagnostic(errors)));
    server.send(":irc CAP * LS :sasl=EXTERNAL\r\n");
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(errors.constFirst().at(1).toBool(), false);
    QCOMPARE(server.commands.size(), 3);
}

QTEST_GUILESS_MAIN(IrcTest)
#include "test_irc.moc"
