#include "providers/irc/irc_provider.h"

#include <QDateTime>
#include <QSslError>
#include <QSslSocket>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace phoschat {
namespace {

    constexpr qsizetype MaximumChannelListings = 20000;
    constexpr qsizetype MaximumWhoisRequests = 32;
    constexpr int PresenceIntervalMs = 30000;
    constexpr qsizetype MaximumIsonPeers = 10;
    // Leave room in the 512-byte reply for the server and recipient prefixes.
    constexpr qsizetype MaximumIsonQueryBytes = 150;

    bool validToken(const QString &token)
    {
        if (token.isEmpty() || token.startsWith(u':')) {
            return false;
        }
        for (const auto character : token) {
            if (character.isSpace() || character.unicode() < 32 || character.unicode() == 127
                || character == u',') {
                return false;
            }
        }
        return true;
    }

    bool nickCharacter(QChar character)
    {
        return character.isLetterOrNumber() || QStringView(u"[]\\`_^{|}-~}").contains(character);
    }

    QString localMessageId()
    {
        return QStringLiteral("local:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    QByteArray capName(const QByteArray &token)
    {
        const auto delimiter = token.indexOf('=');
        return delimiter < 0 ? token : token.left(delimiter);
    }

} // namespace

IrcProvider::IrcProvider(Account account, QObject *parent)
    : IrcProvider(std::move(account), QSslConfiguration::defaultConfiguration(), parent)
{
}

IrcProvider::IrcProvider(Account account, QSslConfiguration tlsConfiguration, QObject *parent)
    : IChatProvider(parent)
    , account_(std::move(account))
    , tlsConfiguration_(std::move(tlsConfiguration))
{
    registrationTimer_.setSingleShot(true);
    connect(&registrationTimer_, &QTimer::timeout, this, [this] {
        if (welcomeReceived_) {
            finishRegistration();
        } else {
            fail(tr("IRC registration timed out."), true);
        }
    });
    keepaliveTimer_.setInterval(15000);
    connect(&keepaliveTimer_, &QTimer::timeout, this, &IrcProvider::checkKeepalive);
    presenceTimer_.setSingleShot(true);
    connect(&presenceTimer_, &QTimer::timeout, this, &IrcProvider::queryPresence);
    channelListTimeout_.setSingleShot(true);
    channelListTimeout_.setInterval(20000);
    connect(&channelListTimeout_, &QTimer::timeout, this,
        [this] { finishChannelList(irc::ChannelListStatus::TimedOut); });
    channelListUpdates_.setSingleShot(true);
    channelListUpdates_.setInterval(100);
    connect(&channelListUpdates_, &QTimer::timeout, this, &IrcProvider::channelListChanged);
}

Capabilities IrcProvider::capabilities() const
{
    return Capability::Text | Capability::DirectMessages | Capability::Channels | Capability::Presence;
}

void IrcProvider::connectAccount(const QByteArray &secret)
{
    if (socket_) {
        return;
    }
    if (account_.host.trimmed().isEmpty() || account_.port == 0 || !validToken(account_.nick)
        || account_.nick.toUtf8().size() > 100) {
        fail(tr("Check the IRC host, port and nickname."), false);
        return;
    }
    if (!account_.saslUser.isEmpty()
        && (!account_.tls || secret.isEmpty() || secret.contains('\0') || secret.size() > 4096
            || account_.saslUser.contains(QChar(u'\0')) || account_.saslUser.toUtf8().size() > 4096)) {
        fail(tr("SASL PLAIN requires TLS and a valid username and password."), false);
        return;
    }
    if (account_.tls && !QSslSocket::supportsSsl()) {
        fail(tr("The Qt TLS backend is unavailable."), false);
        return;
    }

    nick_ = account_.nick;
    channelTypes_ = QStringLiteral("#&");
    prefixSymbols_ = QStringLiteral("@+");
    statusPrefixes_.clear();
    caseMapping_ = irc::CaseMapping::Rfc1459;
    ownSourceBytes_ = 0;
    welcomeReceived_ = false;
    negotiationEnded_ = false;
    authenticating_ = false;
    authenticated_ = false;
    offeredCapabilities_.clear();
    pendingCapabilities_.clear();
    enabledCapabilities_.clear();
    capabilityReply_.clear();
    pingToken_.clear();
    pingSent_.invalidate();
    decoder_.clear();

    if (!account_.saslUser.isEmpty()) {
        secret_ = QByteArray(secret.constData(), secret.size());
    }
    socket_ = new QSslSocket(this);
    socket_->setReadBufferSize(64 * 1024);
    connect(socket_, &QIODevice::readyRead, this, [this] {
        lastReceived_.restart();
        const auto messages = decoder_.append(socket_->readAll());
        for (const auto &message : messages) {
            if (!socket_) {
                break;
            }
            dispatch(message);
        }
    });
    connect(socket_, &QSslSocket::disconnected, this,
        [this] { fail(tr("The IRC server closed the connection."), true); });
    connect(socket_, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        const bool tlsError = error == QAbstractSocket::SslHandshakeFailedError
            || error == QAbstractSocket::SslInternalError
            || error == QAbstractSocket::SslInvalidUserDataError;
        fail(tlsError ? tr("IRC TLS handshake failed.") : tr("The IRC connection was interrupted."),
            !tlsError);
    });

    if (account_.tls) {
        auto configuration = tlsConfiguration_;
        configuration.setProtocol(QSsl::TlsV1_2OrLater);
        configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
        socket_->setSslConfiguration(configuration);
        connect(socket_, &QSslSocket::encrypted, this, &IrcProvider::startRegistration);
        connect(socket_, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &) {
            fail(tr("The IRC server certificate failed verification."), false);
        });
    } else {
        connect(socket_, &QAbstractSocket::connected, this, &IrcProvider::startRegistration);
    }

    setState(ConnectionState::Connecting);
    if (!socket_) {
        return;
    }
    registrationTimer_.start(30000);
    if (account_.tls) {
        socket_->connectToHostEncrypted(account_.host, account_.port);
    } else {
        socket_->connectToHost(account_.host, account_.port);
    }
}

void IrcProvider::disconnectAccount()
{
    if (state_ == ConnectionState::Connected && socket_) {
        auto *transport = socket_;
        socket_ = nullptr;
        transport->disconnect(this);
        transport->write(QByteArrayLiteral("QUIT :PhosChat closing\r\n"));
        connect(transport, &QAbstractSocket::disconnected, transport, &QObject::deleteLater);
        QTimer::singleShot(1000, transport, &QObject::deleteLater);
        transport->disconnectFromHost();
    }
    closeTransport();
    setState(ConnectionState::Disconnected);
}

bool IrcProvider::join(const QString &target)
{
    if (state_ != ConnectionState::Connected || !validToken(target) || !isChannel(target)) {
        emit notice(tr("Connect to IRC and enter a valid channel name."));
        return false;
    }
    const auto existing = channels_.constFind(canonicalTarget(target));
    if (existing != channels_.cend()) {
        emit conversationChanged(makeConversation(existing->name, true));
        return true;
    }
    if (!writeLine("JOIN " + target.toUtf8())) {
        emit notice(tr("The IRC channel name could not be sent."));
        return false;
    }
    return true;
}

bool IrcProvider::part(const QString &target)
{
    if (state_ != ConnectionState::Connected || !validToken(target) || !isChannel(target)
        || !channels_.contains(canonicalTarget(target))) {
        emit notice(tr("Join the IRC channel before leaving it."));
        return false;
    }
    if (!writeLine("PART " + target.toUtf8())) {
        emit notice(tr("The IRC channel could not be left."));
        return false;
    }
    return true;
}

bool IrcProvider::sendText(const ConversationId &conversation, const QString &text)
{
    if (conversation.accountId != account_.id) {
        return false;
    }
    return sendPrivmsg(conversation.remoteId, text, false);
}

bool IrcProvider::sendCommand(const ConversationId &conversation, const QString &command)
{
    if (conversation.accountId != account_.id || state_ != ConnectionState::Connected) {
        emit notice(tr("Connect to IRC before sending a command."));
        return false;
    }

    const auto input = command.startsWith(u'/') ? command.sliced(1) : command;
    const auto separator = input.indexOf(u' ');
    const auto verb = (separator < 0 ? input : input.left(separator)).toLower();
    const auto argument = separator < 0 ? QString {} : input.sliced(separator + 1).trimmed();
    if (verb == QStringLiteral("join")) {
        return join(argument);
    }
    if (verb == QStringLiteral("part")) {
        return part(argument.isEmpty() ? conversation.remoteId : argument);
    }
    if (verb == QStringLiteral("me")) {
        return sendPrivmsg(conversation.remoteId, argument, true);
    }
    if (verb == QStringLiteral("msg")) {
        const auto end = argument.indexOf(u' ');
        if (end > 0) {
            return sendPrivmsg(argument.left(end), argument.sliced(end + 1), false);
        }
    }
    if (verb == QStringLiteral("nick") && validToken(argument) && !isChannel(argument)
        && writeLine("NICK " + argument.toUtf8())) {
        return true;
    }
    if (verb == QStringLiteral("names")) {
        const auto target = argument.isEmpty() ? conversation.remoteId : argument;
        if (validToken(target) && channels_.contains(canonicalTarget(target))
            && writeLine("NAMES " + target.toUtf8())) {
            return true;
        }
    }
    emit notice(tr("IRC commands: /join #channel, /part [#channel], /msg nick text, "
                   "/me action, /nick nickname and /names [#channel]."));
    return false;
}

QString IrcProvider::canonicalTarget(const QString &target) const
{
    return irc::caseFold(target, caseMapping_);
}

bool IrcProvider::requestProfile(const ConversationId &id)
{
    const auto &nick = id.remoteId;
    if (state_ != ConnectionState::Connected || id.accountId != account_.id || !canUseTarget(nick)
        || isChannel(nick) || nick.toUtf8().size() > 100) {
        return false;
    }

    const auto key = canonicalTarget(nick);
    if (const auto request = whoisRequests_.constFind(key); request != whoisRequests_.cend()) {
        return request->id == id && !request->expired;
    }
    if (whoisRequests_.size() >= MaximumWhoisRequests || !writeLine("WHOIS " + nick.toUtf8())) {
        return false;
    }

    const auto serial = ++whoisSerial_;
    whoisRequests_.insert(key, WhoisRequest { id, {}, serial });
    QTimer::singleShot(8000, this, [this, key, serial] {
        const auto request = whoisRequests_.find(key);
        if (request == whoisRequests_.end() || request->serial != serial) {
            return;
        }
        // Drain delayed replies before retrying the same nickname. WHOIS replies have no request ID.
        request->expired = true;
        const auto id = request->id;
        const auto details = std::exchange(request->details, {});
        emit profileReceived(id, details);
    });
    return true;
}

void IrcProvider::setConversationOpen(const ConversationId &id, bool open)
{
    const auto &target = id.remoteId;
    if (id.accountId != account_.id || !canUseTarget(target) || isChannel(target)
        || target.toUtf8().size() > 100) {
        return;
    }
    const auto nick = canonicalTarget(target);
    if (!open) {
        if (presencePeers_.take(nick) != Presence::Unknown) {
            emit presenceChanged({ account_.id, nick }, Presence::Unknown);
        }
        return;
    }
    if (state_ != ConnectionState::Connected || !socket_ || presenceUnavailable_
        || presencePeers_.contains(nick)) {
        return;
    }
    presencePeers_.insert(nick, Presence::Unknown);
    if (!presenceTimer_.isActive()) {
        presenceTimer_.start(0);
    }
}

QString IrcProvider::requestChannelList()
{
    if (state_ != ConnectionState::Connected) {
        return tr("Connect to IRC before requesting a channel list.");
    }
    if (awaitingChannelListEnd_) {
        return tr("The server is still handling the previous channel list request.");
    }
    if (lastChannelListRequest_.isValid() && !lastChannelListRequest_.hasExpired(5000)) {
        return tr("Wait a few seconds before refreshing the channel list.");
    }
    if (!writeLine(QByteArrayLiteral("LIST"))) {
        return tr("The channel list request could not be sent.");
    }
    channelList_.clear();
    channelListNames_.clear();
    channelListTruncated_ = false;
    channelListStatus_ = irc::ChannelListStatus::Loading;
    awaitingChannelListEnd_ = true;
    lastChannelListRequest_.start();
    channelListTimeout_.start();
    emit channelListChanged();
    return {};
}

void IrcProvider::setState(ConnectionState state)
{
    if (state_ != state) {
        state_ = state;
        emit stateChanged(state_);
    }
}

void IrcProvider::closeTransport()
{
    registrationTimer_.stop();
    keepaliveTimer_.stop();
    clearSecret();
    decoder_.clear();
    if (auto *socket = std::exchange(socket_, nullptr)) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    clearPresence();
    presenceUnavailable_ = false;
    const auto whoisRequests = std::exchange(whoisRequests_, {});
    for (const auto &request : whoisRequests) {
        if (!request.expired) {
            emit profileReceived(request.id, {});
        }
    }
    lastChannelListRequest_.invalidate();
    if (awaitingChannelListEnd_) {
        awaitingChannelListEnd_ = false;
        finishChannelList(irc::ChannelListStatus::Interrupted);
    }
    const auto previousChannels = std::exchange(channels_, {});
    for (const auto &channel : previousChannels) {
        const auto conversation = makeConversation(channel.name, false);
        emit conversationChanged(conversation);
        emit membersChanged(conversation.id, {});
    }
}

void IrcProvider::fail(const QString &message, bool retryable)
{
    closeTransport();
    setState(ConnectionState::Error);
    emit errorOccurred(message, retryable);
}

void IrcProvider::startRegistration()
{
    lastReceived_.start();
    if (!writeLine("CAP LS 302") || !writeLine("NICK " + nick_.toUtf8())
        || !writeLine("USER phoschat 0 * :PhosChat")) {
        fail(tr("IRC registration could not be started."), true);
    }
}

void IrcProvider::finishRegistration()
{
    if (!socket_ || !welcomeReceived_ || state_ == ConnectionState::Connected) {
        return;
    }
    registrationTimer_.stop();
    keepaliveTimer_.start();
    setState(ConnectionState::Connected);
}

void IrcProvider::clearSecret()
{
    secret_.fill('\0');
    secret_.clear();
}

void IrcProvider::checkKeepalive()
{
    if (!pingToken_.isEmpty() && pingSent_.hasExpired(30000)) {
        fail(tr("The IRC server did not answer the keepalive."), true);
        return;
    }
    if (pingToken_.isEmpty() && lastReceived_.hasExpired(60000)) {
        pingToken_ = QByteArrayLiteral("phoschat-") + QUuid::createUuid().toByteArray(QUuid::Id128);
        pingSent_.start();
        if (!writeLine("PING :" + pingToken_)) {
            fail(tr("The IRC keepalive could not be sent."), true);
        }
    }
}

void IrcProvider::queryPresence()
{
    if (!presencePending_.isEmpty()) {
        // ISON has no request ID. A late response cannot be matched to a new query.
        presenceUnavailable_ = true;
        clearPresence();
        return;
    }
    if (presencePeers_.isEmpty() || state_ != ConnectionState::Connected || presenceUnavailable_) {
        return;
    }

    auto peers = presencePeers_.keys();
    std::ranges::sort(peers);
    nextPresencePeer_ %= peers.size();
    QByteArray line("ISON");
    QStringList queried;
    while (queried.size() < std::min(MaximumIsonPeers, peers.size())) {
        const auto &nick = peers.at(nextPresencePeer_);
        const auto encoded = nick.toUtf8();
        if (line.size() + encoded.size() + 1 > MaximumIsonQueryBytes) {
            break;
        }
        line += ' ' + encoded;
        queried.append(nick);
        nextPresencePeer_ = (nextPresencePeer_ + 1) % peers.size();
    }
    if (writeLine(line)) {
        presencePending_ = std::move(queried);
    } else {
        for (const auto &nick : queried) {
            if (auto peer = presencePeers_.find(nick); peer != presencePeers_.end()
                && std::exchange(peer.value(), Presence::Unknown) != Presence::Unknown) {
                emit presenceChanged({ account_.id, nick }, Presence::Unknown);
            }
        }
    }
    presenceTimer_.start(PresenceIntervalMs);
}

void IrcProvider::clearPresence()
{
    presenceTimer_.stop();
    presencePending_.clear();
    nextPresencePeer_ = 0;
    const auto peers = std::exchange(presencePeers_, {});
    for (auto peer = peers.cbegin(); peer != peers.cend(); ++peer) {
        if (peer.value() != Presence::Unknown) {
            emit presenceChanged({ account_.id, peer.key() }, Presence::Unknown);
        }
    }
}

bool IrcProvider::writeLine(const QByteArray &line)
{
    if (!socket_ || (account_.tls && !socket_->isEncrypted())
        || socket_->state() != QAbstractSocket::ConnectedState || socket_->bytesToWrite() > 64 * 1024
        || line.size() > irc::MaximumMessageBytes - 2 || line.contains('\r') || line.contains('\n')
        || line.contains('\0')) {
        return false;
    }
    const auto frame = line + "\r\n";
    return socket_->write(frame) == frame.size();
}

bool IrcProvider::canUseTarget(const QString &target) const
{
    if (!validToken(target)) {
        return false;
    }
    return isChannel(target)
        || (!target.startsWith(u'$') && !target.contains(u'@') && !target.contains(u'!')
            && !target.contains(u'*') && !target.contains(u'?'));
}

bool IrcProvider::isChannel(const QString &target) const
{
    return !target.isEmpty() && channelTypes_.contains(target.front());
}

bool IrcProvider::isSelf(const QString &nick) const
{
    return canonicalTarget(nick) == canonicalTarget(nick_);
}

bool IrcProvider::isMention(const QString &text) const
{
    const auto folded = canonicalTarget(text);
    const auto nick = canonicalTarget(nick_);
    qsizetype start = 0;
    for (auto position = folded.indexOf(nick, start); position >= 0; position = folded.indexOf(nick, start)) {
        const auto end = position + nick.size();
        if ((position == 0 || !nickCharacter(folded.at(position - 1)))
            && (end == folded.size() || !nickCharacter(folded.at(end)))) {
            return true;
        }
        start = position + 1;
    }
    return false;
}

Conversation IrcProvider::makeConversation(const QString &target, bool joined) const
{
    Conversation conversation;
    conversation.id = { account_.id, canonicalTarget(target) };
    conversation.title = target;
    conversation.kind = isChannel(target) ? ConversationKind::Channel : ConversationKind::Direct;
    conversation.joined = joined;
    return conversation;
}

bool IrcProvider::sendPrivmsg(const QString &target, const QString &text, bool action)
{
    const auto channel = isChannel(target);
    const auto key = canonicalTarget(target);
    if (state_ != ConnectionState::Connected || !canUseTarget(target)
        || (channel && !channels_.contains(key))) {
        emit notice(tr("Connect to IRC and join the channel before sending messages."));
        return false;
    }
    if (text.trimmed().isEmpty() || text.contains(u'\0') || text.contains(u'\r') || text.contains(u'\n')
        || text.contains(QChar(1))) {
        emit notice(tr("IRC messages must be a single line of text."));
        return false;
    }

    auto payload = text.toUtf8();
    if (action) {
        payload = QByteArrayLiteral("\x01"
                                    "ACTION ")
            + payload + '\x01';
    }
    const auto line = "PRIVMSG " + target.toUtf8() + " :" + payload;
    // The forwarded IRC message includes a source prefix, unlike the client command.
    const auto prefixBudget = ownSourceBytes_ > 0 ? ownSourceBytes_ + 2 : nick_.toUtf8().size() + 70;
    if (line.size() + prefixBudget > irc::MaximumMessageBytes - 2) {
        emit notice(tr("This IRC message is too long. Shorten it and try again."));
        return false;
    }
    if (!writeLine(line)) {
        emit notice(tr("The IRC message could not be sent."));
        return false;
    }
    if (enabledCapabilities_.contains("echo-message")) {
        // The server echoes its final text. A local copy would create a duplicate.
        return true;
    }

    auto conversation = makeConversation(channel ? channels_.value(key).name : target, true);
    emit conversationChanged(conversation);
    emit messageReceived(Message { conversation.id, localMessageId(), nick_,
        (action ? QStringLiteral("* ") : QString {}) + irc::plainText(text), QDateTime::currentDateTimeUtc(),
        true, false });
    return true;
}

void IrcProvider::dispatch(const irc::IrcMessage &message)
{
    const auto &command = message.command;
    const auto &parameters = message.parameters;
    if (command == "PING" && !parameters.isEmpty()) {
        if (!writeLine("PONG :" + parameters.constLast())) {
            fail(tr("The IRC keepalive could not be answered."), true);
        }
        return;
    }
    if (command == "PONG" && !pingToken_.isEmpty() && !parameters.isEmpty()
        && parameters.constLast() == pingToken_) {
        pingToken_.clear();
        return;
    }
    if (command == "CAP") {
        handleCapability(parameters);
        return;
    }
    if (command == "AUTHENTICATE") {
        handleAuthenticate(parameters);
        return;
    }
    if (authenticating_ && (command == "903" || command == "907")) {
        authenticating_ = false;
        authenticated_ = true;
        clearSecret();
        endCapabilityNegotiation();
        return;
    }
    if (authenticating_ && (command == "902" || command == "904" || command == "905" || command == "906")) {
        fail(tr("IRC SASL authentication failed. Check your credentials."), false);
        return;
    }
    if (command == "001" && !parameters.isEmpty() && !welcomeReceived_) {
        if (!account_.saslUser.isEmpty() && !authenticated_) {
            fail(tr("The IRC server did not complete SASL authentication."), false);
            return;
        }
        nick_ = QString::fromUtf8(parameters.constFirst());
        if (!validToken(nick_)) {
            fail(tr("The IRC server returned an invalid nickname."), false);
            return;
        }
        welcomeReceived_ = true;
        // Most servers publish ISUPPORT after 001, before the end of MOTD. Wait for
        // those details before the account manager sends its automatic JOINs.
        registrationTimer_.start(1200);
        return;
    }
    if (command == "005" && welcomeReceived_) {
        handleSupport(parameters);
        return;
    }
    if (command == "376" || command == "422") {
        finishRegistration();
        return;
    }
    if (command == "465" || command == "464") {
        fail(tr("The IRC server refused registration or access (%1).").arg(QString::fromLatin1(command)),
            false);
        return;
    }
    if (command == "ERROR") {
        fail(tr("The IRC server closed the connection."), true);
        return;
    }

    const auto numeric = command.toInt();
    if (numeric == 431 || numeric == 432 || numeric == 433 || numeric == 436 || numeric == 437) {
        if (!welcomeReceived_) {
            fail(tr("The IRC nickname is invalid or already in use."), false);
        } else {
            emit notice(tr("The requested IRC nickname or channel is unavailable."));
        }
        return;
    }
    if (!welcomeReceived_) {
        return;
    }
    if (numeric == 303) {
        handleIson(parameters);
        return;
    }
    if ((numeric == 421 || numeric == 461) && parameters.size() >= 2 && parameters.at(1) == "ISON"
        && isSelf(QString::fromUtf8(parameters.at(0))) && !presencePending_.isEmpty()) {
        presenceUnavailable_ = true;
        clearPresence();
        return;
    }
    if (handleWhois(message, numeric) || handleJoinFailure(message, numeric)) {
        return;
    }

    const bool listFailure = ((numeric == 263 || numeric == 421 || numeric == 461) && parameters.size() >= 2
                                 && parameters.at(1) == "LIST")
        || (command == "FAIL" && !parameters.isEmpty() && parameters.constFirst() == "LIST");
    if (numeric == 322 || numeric == 323 || listFailure) {
        handleChannelList(message);
    } else if (command == "PRIVMSG" && parameters.size() >= 2) {
        handlePrivmsg(message);
    } else if (command == "JOIN" || command == "PART" || command == "KICK" || command == "QUIT"
        || command == "NICK") {
        handleMembership(message);
    } else if (command == "353" || command == "366") {
        handleNames(message);
    } else if (numeric >= 400 && numeric <= 599) {
        // IRC error descriptions are untrusted and can echo a private message or secret.
        switch (numeric) {
        case 401:
            emit notice(tr("That IRC nickname is unavailable."));
            break;
        case 404:
            emit notice(tr("The IRC server refused to send to this channel."));
            break;
        default:
            emit notice(tr("The IRC server returned error %1.").arg(numeric));
            break;
        }
    }
}

void IrcProvider::handleIson(const QList<QByteArray> &parameters)
{
    if (presencePending_.isEmpty() || parameters.size() != 2
        || !isSelf(QString::fromUtf8(parameters.at(0)))) {
        return;
    }
    QSet<QString> online;
    for (const auto &nick : parameters.at(1).split(' ')) {
        if (nick.isEmpty()) {
            continue;
        }
        const auto key = canonicalTarget(QString::fromUtf8(nick));
        if (!presencePending_.contains(key)) {
            return;
        }
        online.insert(key);
    }
    const auto queried = std::exchange(presencePending_, {});
    for (const auto &nick : queried) {
        const auto presence = online.contains(nick) ? Presence::Online : Presence::Offline;
        if (auto peer = presencePeers_.find(nick);
            peer != presencePeers_.end() && std::exchange(peer.value(), presence) != presence) {
            emit presenceChanged({ account_.id, nick }, presence);
        }
    }
}

bool IrcProvider::handleWhois(const irc::IrcMessage &message, int numeric)
{
    switch (numeric) {
    case 301:
    case 311:
    case 312:
    case 318:
    case 330:
    case 401:
        break;
    default:
        return false;
    }
    const auto &parameters = message.parameters;
    if (parameters.size() < 2 || !isSelf(QString::fromUtf8(parameters.at(0)))) {
        return false;
    }
    const auto key = canonicalTarget(QString::fromUtf8(parameters.at(1)));
    const auto request = whoisRequests_.find(key);
    if (request == whoisRequests_.end()) {
        return false;
    }
    if (numeric == 318 || numeric == 401) {
        const auto finished = std::move(request.value());
        whoisRequests_.erase(request);
        if (!finished.expired) {
            emit profileReceived(finished.id, numeric == 318 ? finished.details : ProfileDetails {});
        }
        return true;
    }
    if (request->expired) {
        return true;
    }

    const auto addField = [&details = request->details](const QString &label, const QByteArray &value) {
        const auto text = irc::plainText(QString::fromUtf8(value)).trimmed();
        if (text.isEmpty()) {
            return;
        }
        const auto field = std::ranges::find(details, label, &QPair<QString, QString>::first);
        if (field == details.end()) {
            details.append({ label, text });
        } else {
            field->second = text;
        }
    };
    if (numeric == 311 && parameters.size() >= 6) {
        addField(tr("Real name"), parameters.at(5));
        addField(tr("Username"), parameters.at(2));
    } else if (parameters.size() >= 3) {
        if (numeric == 301) {
            addField(tr("Away"), parameters.at(2));
        } else if (numeric == 312) {
            addField(tr("IRC server"), parameters.at(2));
        } else if (numeric == 330 && parameters.at(2) != "*") {
            addField(tr("IRC account"), parameters.at(2));
        }
    }
    return true;
}

bool IrcProvider::handleJoinFailure(const irc::IrcMessage &message, int numeric)
{
    const auto &parameters = message.parameters;
    QString channel;
    QString detail;
    if (message.command == "FAIL" && parameters.size() >= 3 && parameters.at(0) == "JOIN") {
        detail = tr("The IRC server refused to join the channel.");
        if (parameters.size() >= 4) {
            channel = QString::fromUtf8(parameters.at(2));
        }
    } else {
        switch (numeric) {
        case 403:
            detail = tr("The IRC channel does not exist.");
            break;
        case 405:
            detail = tr("This IRC account has joined too many channels.");
            break;
        case 471:
            detail = tr("The IRC channel is full.");
            break;
        case 473:
            detail = tr("The IRC channel requires an invitation.");
            break;
        case 474:
            detail = tr("This IRC account is banned from the channel.");
            break;
        case 475:
            detail = tr("The IRC channel requires a valid key.");
            break;
        case 476:
            detail = tr("The IRC server rejected the channel name.");
            break;
        case 477:
        case 489:
            detail = tr("The IRC server refused to join the channel (%1).").arg(numeric);
            break;
        default:
            return false;
        }
        if (!parameters.isEmpty() && !isSelf(QString::fromUtf8(parameters.at(0)))) {
            return true;
        }
        if (parameters.size() >= 2) {
            channel = QString::fromUtf8(parameters.at(1));
        }
    }
    if (validToken(channel) && isChannel(channel)) {
        emit joinFailed({ account_.id, canonicalTarget(channel) }, detail);
    } else {
        emit notice(detail);
    }
    return true;
}

void IrcProvider::handleChannelList(const irc::IrcMessage &message)
{
    const auto &parameters = message.parameters;
    if (!awaitingChannelListEnd_ || parameters.isEmpty()
        || (message.command != "FAIL" && !isSelf(QString::fromUtf8(parameters.constFirst())))) {
        return;
    }
    if (message.command != "322") {
        awaitingChannelListEnd_ = false;
        if (channelListStatus_ == irc::ChannelListStatus::TimedOut) {
            // IRC has no request ID here. Drain late replies before permitting a new LIST.
            emit channelListChanged();
        } else if (message.command == "323") {
            finishChannelList(
                channelListTruncated_ ? irc::ChannelListStatus::Truncated : irc::ChannelListStatus::Ready);
        } else {
            finishChannelList(irc::ChannelListStatus::Rejected);
        }
        return;
    }
    if (channelListStatus_ != irc::ChannelListStatus::Loading || parameters.size() < 3) {
        return;
    }
    const QString name = QString::fromUtf8(parameters.at(1));
    bool validCount = false;
    const int users = parameters.at(2).toInt(&validCount);
    if (!validCount || users < 0 || !validToken(name) || !isChannel(name)) {
        return;
    }
    const auto key = canonicalTarget(name);
    if (channelListNames_.contains(key)) {
        return;
    }
    if (channelList_.size() >= MaximumChannelListings) {
        channelListTruncated_ = true;
        return;
    }
    channelListNames_.insert(key);
    const auto topic = irc::plainText(QString::fromUtf8(parameters.value(3)));
    channelList_.append({ name, topic, users });
    if (!channelListUpdates_.isActive()) {
        channelListUpdates_.start();
    }
}

void IrcProvider::finishChannelList(irc::ChannelListStatus status)
{
    channelListTimeout_.stop();
    channelListUpdates_.stop();
    channelListNames_.clear();
    channelListStatus_ = status;
    emit channelListChanged();
}

void IrcProvider::handleCapability(const QList<QByteArray> &parameters)
{
    if (parameters.size() < 3) {
        return;
    }
    const auto &subcommand = parameters.at(1);
    const auto tokens = parameters.constLast().split(' ');
    const bool continuation = parameters.size() > 3 && parameters.at(2) == "*";

    if (subcommand == "LS" || subcommand == "NEW") {
        if (subcommand == "LS" && negotiationEnded_) {
            return;
        }
        for (const auto &token : tokens) {
            const auto name = capName(token);
            // Keep only the small set we know how to use. A faulty server can
            // otherwise send arbitrarily many LS continuation lines.
            if (name == "message-tags" || name == "server-time" || name == "multi-prefix"
                || name == "echo-message" || name == "sasl") {
                offeredCapabilities_.insert(name, token.mid(name.size() + 1));
            }
        }
        if (!continuation) {
            requestCapabilities(subcommand == "LS" && !account_.saslUser.isEmpty());
        }
        return;
    }

    if (subcommand == "DEL") {
        for (const auto &token : tokens) {
            offeredCapabilities_.remove(token);
            enabledCapabilities_.remove(token);
            pendingCapabilities_.remove(token);
            capabilityReply_.remove(token);
            if (token == "sasl" && !account_.saslUser.isEmpty() && !authenticated_) {
                fail(tr("The IRC server withdrew SASL during registration."), false);
                return;
            }
        }
        endCapabilityNegotiation();
        return;
    }

    if (subcommand != "ACK" && subcommand != "NAK") {
        return;
    }

    for (const auto &token : tokens) {
        const auto capability = token.startsWith('-') ? token.mid(1) : token;
        if (pendingCapabilities_.contains(capability)) {
            capabilityReply_.insert(capability, subcommand == "ACK" && !token.startsWith('-'));
        }
    }
    if (continuation) {
        return;
    }

    const auto reply = std::exchange(capabilityReply_, {});
    for (auto entry = reply.cbegin(); entry != reply.cend(); ++entry) {
        const auto &capability = entry.key();
        const auto accepted = entry.value();
        pendingCapabilities_.remove(capability);
        if (accepted) {
            enabledCapabilities_.insert(capability);
        }
        if (capability == "sasl") {
            if (!accepted || !writeLine("AUTHENTICATE PLAIN")) {
                fail(tr("The IRC server did not enable SASL PLAIN."), false);
                return;
            }
            authenticating_ = true;
            setState(ConnectionState::Authenticating);
        }
    }

    endCapabilityNegotiation();
}

void IrcProvider::requestCapabilities(bool includeSasl)
{
    if (includeSasl) {
        const auto mechanisms = offeredCapabilities_.value("sasl");
        if (!offeredCapabilities_.contains("sasl")
            || (!mechanisms.isEmpty() && !mechanisms.toUpper().split(',').contains("PLAIN"))) {
            fail(tr("The IRC server does not offer SASL PLAIN."), false);
            return;
        }
        if (!pendingCapabilities_.contains("sasl") && !authenticating_ && !authenticated_) {
            if (!writeLine("CAP REQ :sasl")) {
                fail(tr("IRC capability negotiation could not continue."), true);
                return;
            }
            pendingCapabilities_.insert("sasl");
        }
    }

    QList<QByteArray> requested;
    for (const auto &name : { QByteArrayLiteral("message-tags"), QByteArrayLiteral("server-time"),
             QByteArrayLiteral("multi-prefix"), QByteArrayLiteral("echo-message") }) {
        if (offeredCapabilities_.contains(name) && !enabledCapabilities_.contains(name)
            && !pendingCapabilities_.contains(name)) {
            requested.append(name);
        }
    }
    if (!requested.isEmpty()) {
        QByteArray command("CAP REQ :");
        for (const auto &name : requested) {
            if (!command.endsWith(':')) {
                command.append(' ');
            }
            command.append(name);
            pendingCapabilities_.insert(name);
        }
        if (!writeLine(command)) {
            fail(tr("IRC capability negotiation could not continue."), true);
            return;
        }
    }

    endCapabilityNegotiation();
}

void IrcProvider::endCapabilityNegotiation()
{
    if (negotiationEnded_ || welcomeReceived_ || !pendingCapabilities_.isEmpty()
        || (!account_.saslUser.isEmpty() && !authenticated_)) {
        return;
    }
    negotiationEnded_ = true;
    if (!writeLine("CAP END")) {
        fail(tr("IRC capability negotiation could not finish."), true);
    }
}

void IrcProvider::handleAuthenticate(const QList<QByteArray> &parameters)
{
    if (!authenticating_) {
        return;
    }
    if (parameters.size() != 1 || parameters.constFirst() != "+" || secret_.isEmpty()) {
        fail(tr("The IRC server returned an invalid SASL PLAIN challenge."), false);
        return;
    }
    QByteArray plain(1, '\0');
    plain.append(account_.saslUser.toUtf8());
    plain.append('\0');
    plain.append(secret_);
    clearSecret();
    auto encoded = plain.toBase64();
    plain.fill('\0');
    bool sent = true;
    for (qsizetype offset = 0; sent && offset < encoded.size(); offset += 400) {
        sent = writeLine("AUTHENTICATE " + encoded.mid(offset, 400));
    }
    if (sent && encoded.size() % 400 == 0) {
        sent = writeLine("AUTHENTICATE +");
    }
    encoded.fill('\0');
    if (!sent) {
        fail(tr("IRC SASL authentication could not continue."), true);
    }
}

void IrcProvider::handleSupport(const QList<QByteArray> &parameters)
{
    for (qsizetype index = 1; index + 1 < parameters.size(); ++index) {
        const auto &token = parameters.at(index);
        const auto delimiter = token.indexOf('=');
        const auto key = delimiter < 0 ? token : token.left(delimiter);
        const auto value = delimiter < 0 ? QByteArray {} : token.mid(delimiter + 1);
        if (key == "CASEMAPPING" && channels_.isEmpty()) {
            if (value == "ascii") {
                caseMapping_ = irc::CaseMapping::Ascii;
            } else if (value == "rfc1459-strict" || value == "strict-rfc1459") {
                caseMapping_ = irc::CaseMapping::StrictRfc1459;
            } else if (value == "rfc1459" || value.isEmpty()) {
                caseMapping_ = irc::CaseMapping::Rfc1459;
            }
        } else if (key == "CHANTYPES") {
            channelTypes_ = QString::fromLatin1(value);
        } else if (key == "PREFIX") {
            const auto separator = value.indexOf(')');
            if (value.isEmpty() || separator >= 0) {
                prefixSymbols_ = separator < 0 ? QString {} : QString::fromLatin1(value.mid(separator + 1));
            }
        } else if (key == "STATUSMSG") {
            statusPrefixes_ = QString::fromLatin1(value);
        }
    }
}

void IrcProvider::handlePrivmsg(const irc::IrcMessage &message)
{
    const auto sender = message.nickname();
    if (!validToken(sender)) {
        return;
    }
    auto target = QString::fromUtf8(message.parameters.at(0));
    qsizetype statusLength = 0;
    while (statusLength < target.size() && statusPrefixes_.contains(target.at(statusLength))) {
        ++statusLength;
    }
    if (statusLength > 0 && isChannel(target.sliced(statusLength))) {
        target = target.sliced(statusLength);
    }
    if (!canUseTarget(target)) {
        return;
    }
    const auto outgoing = isSelf(sender);
    const auto channel = isChannel(target);
    if (!channel && !outgoing) {
        if (!isSelf(target)) {
            return;
        }
        target = sender;
    }

    auto payload = QString::fromUtf8(message.parameters.at(1));
    if (payload.startsWith(QChar(1))) {
        const auto actionPrefix = QStringLiteral("\x01"
                                                 "ACTION ");
        if (!payload.startsWith(actionPrefix) || !payload.endsWith(QChar(1))) {
            return;
        }
        payload = QStringLiteral("* ")
            + payload.sliced(actionPrefix.size(), payload.size() - actionPrefix.size() - 1);
    }
    const auto text = irc::plainText(payload);
    if (text.isEmpty()) {
        return;
    }

    const auto key = canonicalTarget(target);
    const auto joined = !channel || channels_.contains(key);
    auto conversation = makeConversation(target, joined);
    if (channel && joined) {
        conversation.title = channels_.value(key).name;
    }
    const auto timestampTag = message.tags.value("time");
    auto timestamp = QDateTime::fromString(timestampTag, Qt::ISODateWithMs);
    if (!timestamp.isValid() && timestampTag.size() > 19
        && timestampTag.sliced(17, 2) == QStringLiteral("60")) {
        auto normalized = timestampTag;
        normalized.replace(17, 2, QStringLiteral("59"));
        timestamp = QDateTime::fromString(normalized, Qt::ISODateWithMs).addSecs(1);
    }
    if (!timestamp.isValid()) {
        timestamp = QDateTime::currentDateTimeUtc();
    }
    const auto remoteId = message.tags.value("msgid");
    const auto messageId = remoteId.isEmpty() ? localMessageId() : QStringLiteral("remote:") + remoteId;
    Message received { conversation.id, messageId, sender, text, timestamp, outgoing,
        channel && !outgoing && isMention(text) };
    if (outgoing && enabledCapabilities_.contains("echo-message")) {
        received.delivery = DeliveryState::Sent;
    }
    emit conversationChanged(conversation);
    emit messageReceived(received);
}

void IrcProvider::handleMembership(const irc::IrcMessage &message)
{
    const auto &command = message.command;
    const auto &parameters = message.parameters;
    const auto sender = message.nickname();
    if (command == "QUIT" || command == "NICK") {
        if (!validToken(sender) || (command == "NICK" && parameters.isEmpty())) {
            return;
        }
        const auto newNick = command == "NICK" ? QString::fromUtf8(parameters.constFirst()) : QString {};
        if (command == "NICK" && !validToken(newNick)) {
            return;
        }
        if (isSelf(sender) && command == "NICK") {
            ownSourceBytes_ = message.source.size() - sender.toUtf8().size() + newNick.toUtf8().size();
            nick_ = newNick;
        }
        for (auto channel = channels_.begin(); channel != channels_.end(); ++channel) {
            if (channel->members.contains(canonicalTarget(sender))
                || channel->names.contains(canonicalTarget(sender))) {
                updateMember(channel.value(), sender, newNick);
                publishMembers(channel.key());
            }
        }
        return;
    }

    if (parameters.isEmpty() || (command == "KICK" && parameters.size() < 2)) {
        return;
    }
    const auto name = QString::fromUtf8(parameters.constFirst());
    if (!validToken(name) || !isChannel(name)) {
        return;
    }

    const auto key = canonicalTarget(name);
    const auto affected = command == "KICK" ? QString::fromUtf8(parameters.at(1)) : sender;
    if (!validToken(affected)) {
        return;
    }
    if (isSelf(affected)) {
        if (command == "JOIN") {
            ownSourceBytes_ = message.source.size();
            Channel channel;
            channel.name = name;
            channel.members.insert(canonicalTarget(nick_), nick_);
            channels_.insert(key, std::move(channel));
            emit conversationChanged(makeConversation(name, true));
            publishMembers(key);
        } else {
            removeChannel(name);
        }
        return;
    }

    if (auto channel = channels_.find(key); channel != channels_.end()) {
        updateMember(channel.value(), affected, command == "JOIN" ? affected : QString {});
        publishMembers(key);
    }
}

void IrcProvider::handleNames(const irc::IrcMessage &message)
{
    const auto &parameters = message.parameters;
    const auto isChunk = message.command == "353";
    const qsizetype targetIndex = isChunk ? 2 : 1;
    if (parameters.size() < targetIndex + 2) {
        return;
    }
    const auto key = canonicalTarget(QString::fromUtf8(parameters.at(targetIndex)));
    auto channel = channels_.find(key);
    if (channel == channels_.end()) {
        return;
    }
    if (!isChunk) {
        if (channel->readingNames) {
            channel->members = std::exchange(channel->names, {});
            channel->readingNames = false;
        }
        publishMembers(key);
        return;
    }

    if (!channel->readingNames) {
        channel->names.clear();
        channel->readingNames = true;
    }
    for (const auto &token : parameters.constLast().split(' ')) {
        const auto separator = token.indexOf('!');
        auto nick = QString::fromUtf8(separator < 0 ? token : token.left(separator));
        qsizetype begin = 0;
        while (begin < nick.size() && prefixSymbols_.contains(nick.at(begin))) {
            ++begin;
        }
        nick = nick.sliced(begin);
        if (validToken(nick) && channel->names.size() < 25000) {
            channel->names.insert(canonicalTarget(nick), nick);
        }
    }
}

void IrcProvider::publishMembers(const QString &key)
{
    if (const auto channel = channels_.constFind(key); channel != channels_.cend()) {
        auto names = channel->members.values();
        names.sort(Qt::CaseInsensitive);
        emit membersChanged(ConversationId { account_.id, key }, names);
    }
}

void IrcProvider::removeChannel(const QString &name)
{
    const auto key = canonicalTarget(name);
    if (auto channel = channels_.find(key); channel != channels_.end()) {
        const auto conversation = makeConversation(channel->name, false);
        channels_.erase(channel);
        emit conversationChanged(conversation);
        emit membersChanged(conversation.id, {});
    }
}

void IrcProvider::updateMember(Channel &channel, const QString &oldNick, const QString &newNick)
{
    const auto oldKey = canonicalTarget(oldNick);
    const auto newKey = canonicalTarget(newNick);
    const auto update = [&oldKey, &newKey, &newNick](QHash<QString, QString> &members) {
        members.remove(oldKey);
        if (!newNick.isEmpty() && members.size() < 25000) {
            members.insert(newKey, newNick);
        }
    };
    update(channel.members);
    if (channel.readingNames) {
        update(channel.names);
    }
}

} // namespace phoschat
