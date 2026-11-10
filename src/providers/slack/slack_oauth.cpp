#include "providers/slack/slack_oauth.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrlQuery>

namespace phoschat {
namespace {

    constexpr auto CallbackPath = "/phoschat/slack/callback";
    constexpr auto UserScopes = "channels:read,channels:history,groups:read,groups:history,"
                                "im:read,im:history,mpim:read,mpim:history,chat:write,users:read,files:write";

    QString randomUrlKey()
    {
        QByteArray bytes(32, Qt::Uninitialized);
        for (char &byte : bytes)
            byte = static_cast<char>(QRandomGenerator::system()->generate() & 0xff);
        return QString::fromLatin1(
            bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    bool matchesState(const QString &received, const QString &expected)
    {
        if (received.size() != expected.size())
            return false;
        ushort difference = 0;
        for (qsizetype i = 0; i < received.size(); ++i)
            difference |= received[i].unicode() ^ expected[i].unicode();
        return difference == 0;
    }

} // namespace

SlackOAuth::SlackOAuth(QObject *parent)
    : QObject(parent)
{
    server_.setMaxPendingConnections(4);
    timeout_.setSingleShot(true);
    connect(&server_, &QTcpServer::newConnection, this, &SlackOAuth::acceptConnections);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        stop();
        emit failed(tr("Slack authorization timed out. Connect again to open a new request."));
    });
}

bool SlackOAuth::start(
    const QString &clientId, quint16 port, const QUrl &authorizationEndpoint, const QUrl &callbackOrigin)
{
    stop();
    if (!server_.listen(QHostAddress::LocalHost, port))
        return false;

    redirect_ = QUrl(QStringLiteral("http://localhost:%1%2")
            .arg(server_.serverPort())
            .arg(QString::fromLatin1(CallbackPath)));
    if (!callbackOrigin.isEmpty())
        redirect_ = callbackOrigin.resolved(QUrl(QString::fromLatin1(CallbackPath)));
    state_ = randomUrlKey();
    verifier_ = randomUrlKey();
    const auto challenge = QCryptographicHash::hash(verifier_.toLatin1(), QCryptographicHash::Sha256)
                               .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QUrl url = authorizationEndpoint;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), clientId);
    query.addQueryItem(QStringLiteral("scope"), QStringLiteral(""));
    query.addQueryItem(QStringLiteral("user_scope"), QString::fromLatin1(UserScopes));
    query.addQueryItem(QStringLiteral("redirect_uri"), redirect_.toString());
    query.addQueryItem(QStringLiteral("state"), state_);
    query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    url.setQuery(query);
    timeout_.start(180000);
    emit authorizationRequested(url);
    return true;
}

void SlackOAuth::stop() { stopListening(nullptr); }

void SlackOAuth::stopListening(QTcpSocket *completingSocket)
{
    server_.close();
    timeout_.stop();
    state_.clear();
    verifier_.clear();
    redirect_.clear();
    const auto sockets = requests_.keys();
    requests_.clear();
    for (auto *socket : sockets) {
        if (socket != completingSocket)
            socket->abort();
    }
}

void SlackOAuth::acceptConnections()
{
    while (auto *socket = server_.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        if (requests_.size() >= 4) {
            socket->abort();
            continue;
        }

        socket->setReadBufferSize(8193);
        requests_.insert(socket, {});
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { requests_.remove(socket); });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readRequest(*socket); });
        QTimer::singleShot(5000, socket, [socket] { socket->abort(); });
        if (socket->bytesAvailable())
            readRequest(*socket);
    }
}

void SlackOAuth::readRequest(QTcpSocket &socket)
{
    auto it = requests_.find(&socket);
    if (it == requests_.end())
        return;
    it.value() += socket.readAll();
    if (it.value().size() > 8192) {
        respond(socket, "431 Request Header Fields Too Large", "Request too large.");
        return;
    }
    if (!it.value().contains("\r\n\r\n"))
        return;

    const auto line = it.value().first(it.value().indexOf("\r\n")).split(' ');
    if (line.size() != 3 || line[0] != "GET" || !line[1].startsWith('/') || line[1].startsWith("//")) {
        respond(socket, "400 Bad Request", "Invalid request.");
        return;
    }

    const QUrl target = QUrl::fromEncoded(line[1], QUrl::StrictMode);
    if (!target.isValid() || target.hasFragment() || target.path() != QLatin1StringView(CallbackPath)) {
        respond(socket, "404 Not Found", "PhosChat Slack callback not found.");
        return;
    }

    const QUrlQuery query(target);
    const auto states = query.allQueryItemValues(QStringLiteral("state"), QUrl::FullyDecoded);
    if (states.size() != 1 || state_.isEmpty() || !matchesState(states.front(), state_)) {
        respond(socket, "400 Bad Request", "Authorization state did not match. Return to PhosChat.");
        return;
    }
    if (query.hasQueryItem(QStringLiteral("error"))) {
        respond(socket, "200 OK", "Slack authorization was cancelled. You can close this tab.");
        stopListening(&socket);
        emit failed(tr("Slack authorization was declined or cancelled."));
        return;
    }

    const auto codes = query.allQueryItemValues(QStringLiteral("code"), QUrl::FullyDecoded);
    if (codes.size() != 1 || codes.front().isEmpty() || codes.front().size() > 2048
        || codes.front().contains(QChar::Null)) {
        respond(socket, "400 Bad Request", "No authorization code was provided.");
        return;
    }
    const QString verifier = verifier_;
    const QUrl redirect = redirect_;
    respond(socket, "200 OK", "Authorization received. You can close this tab and return to PhosChat.");
    stopListening(&socket);
    emit authorized(codes.front(), verifier, redirect);
}

void SlackOAuth::respond(QTcpSocket &socket, QByteArrayView status, QByteArrayView message)
{
    requests_.remove(&socket);
    QByteArray response("HTTP/1.1 ");
    response += status;
    response += "\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n"
                "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n"
                "Content-Length: ";
    response += QByteArray::number(message.size());
    response += "\r\n\r\n";
    response += message;
    socket.write(response);
    socket.disconnectFromHost();
}

} // namespace phoschat
