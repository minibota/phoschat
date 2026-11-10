#include "providers/slack/slack_provider.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace phoschat {
namespace {

    using namespace Qt::StringLiterals;

    constexpr auto MaxResponseBytes = 4 * 1024 * 1024;
    constexpr auto ListRefreshMs = 300000;
    constexpr auto MaxProfiles = 128;
    const auto FormContent = QByteArrayLiteral("application/x-www-form-urlencoded");
    const auto JsonContent = QByteArrayLiteral("application/json");

    QByteArray formBody(const QUrlQuery &fields)
    {
        // QUrlQuery leaves literal plus signs untouched. Form decoding treats them as spaces.
        return fields.query(QUrl::FullyEncoded).toUtf8().replace("+", "%2B");
    }

    bool validClientId(const QString &id)
    {
        static const QRegularExpression pattern(u"^[0-9]{1,32}\\.[0-9]{1,32}$"_s);
        return pattern.match(id).hasMatch();
    }

    bool validId(const QString &id, QStringView prefixes)
    {
        if (id.size() < 2 || id.size() > 32 || !prefixes.contains(id.front()))
            return false;
        return std::ranges::all_of(
            id, [](QChar c) { return (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9'); });
    }

    bool tokenCharacters(QByteArrayView token)
    {
        return token.size() <= 4096 && std::ranges::all_of(token, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                || c == '.' || c == '_';
        });
    }

    bool validUserToken(QByteArrayView token)
    {
        return token.size() > 6 && (token.startsWith("xoxp-") || token.startsWith("xoxe.xoxp-"))
            && tokenCharacters(token);
    }

    bool validRefreshToken(QByteArrayView token)
    {
        return token.size() > 6 && token.startsWith("xoxe-") && tokenCharacters(token);
    }

    std::optional<quint64> timestampValue(const QString &value)
    {
        static const QRegularExpression pattern(u"^([0-9]{1,12})\\.([0-9]{6})$"_s);
        const auto match = pattern.match(value);
        if (!match.hasMatch())
            return std::nullopt;
        return match.capturedView(1).toULongLong() * 1000000 + match.capturedView(2).toULongLong();
    }

    QString newest(const QString &first, const QString &second)
    {
        return timestampValue(first).value_or(0) >= timestampValue(second).value_or(0) ? first : second;
    }

    QString plainSlackText(QString input, const QHash<QString, QString> &profiles)
    {
        static const QRegularExpression special(
            u"<(@[UW][A-Z0-9]+|#[CG][A-Z0-9]+|(?:https?://|mailto:)[^<>|]+|![a-z]+)(?:\\|([^<>]+))?>"_s);
        auto matches = special.globalMatch(input);
        QString result;
        qsizetype start = 0;
        while (matches.hasNext()) {
            const auto match = matches.next();
            result += QStringView(input).sliced(start, match.capturedStart() - start);
            const auto tag = match.captured(1);
            const auto label = match.captured(2);
            if (tag.startsWith(u'@'))
                result += u"@"_s + profiles.value(tag.sliced(1), tag.sliced(1));
            else if (tag.startsWith(u'#'))
                result += label.isEmpty() ? tag : u"#"_s + label;
            else
                result += label.isEmpty() ? tag : label;
            start = match.capturedEnd();
        }
        result += QStringView(input).sliced(start);
        return result.replace(u"&lt;"_s, u"<"_s).replace(u"&gt;"_s, u">"_s).replace(u"&amp;"_s, u"&"_s);
    }

    QString slackPlainInput(QString input)
    {
        return input.replace(u"&"_s, u"&amp;"_s).replace(u"<"_s, u"&lt;"_s).replace(u">"_s, u"&gt;"_s);
    }

    QString nextCursor(const QJsonObject &data)
    {
        return data.value(u"response_metadata"_s)
            .toObject()
            .value(u"next_cursor"_s)
            .toString()
            .trimmed()
            .left(2048);
    }

    class MethodQuota {
    public:
        int reserve(const QString &key, int interval)
        {
            const QMutexLocker lock(&mutex_);
            const auto now = QDeadlineTimer::current(Qt::PreciseTimer).deadline();
            if (const auto until = deadlines_.value(key); until > now)
                return static_cast<int>(std::min<qint64>(until - now, std::numeric_limits<int>::max()));
            if (interval > 0)
                deadlines_.insert(key, now + interval);
            if (deadlines_.size() > 128)
                deadlines_.removeIf([now](auto entry) { return entry.value() <= now; });
            return 0;
        }

        void backOff(const QString &key, int interval)
        {
            const QMutexLocker lock(&mutex_);
            const auto until = QDeadlineTimer::current(Qt::PreciseTimer).deadline() + interval;
            deadlines_.insert(key, std::max(deadlines_.value(key), until));
        }

    private:
        QMutex mutex_;
        QHash<QString, qint64> deadlines_;
    };

    MethodQuota quota;

    bool validTestEndpoint(const QUrl &url)
    {
        return url.isValid() && url.scheme() == u"http"_s && url.host() == u"127.0.0.1"_s && url.port() > 0
            && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
    }

} // namespace

SlackProvider::SlackProvider(const Account &account, QObject *parent)
    : IChatProvider(parent)
    , account_(account)
    , apiBase_(u"https://slack.com/api/"_s)
    , authorizationEndpoint_(u"https://slack.com/oauth/v2/authorize"_s)
{
    for (auto *timer : { &listTimer_, &historyTimer_, &refreshTimer_, &authTimer_, &profileTimer_ })
        timer->setSingleShot(true);
    connect(&listTimer_, &QTimer::timeout, this, &SlackProvider::refreshConversations);
    connect(&historyTimer_, &QTimer::timeout, this, &SlackProvider::readHistory);
    connect(&refreshTimer_, &QTimer::timeout, this, &SlackProvider::refreshToken);
    connect(&authTimer_, &QTimer::timeout, this, &SlackProvider::validateToken);
    connect(&profileTimer_, &QTimer::timeout, this, &SlackProvider::readProfile);
    connect(&oauth_, &SlackOAuth::authorizationRequested, this, &IChatProvider::authorizationUrlRequested);
    connect(&oauth_, &SlackOAuth::authorized, this, &SlackProvider::exchangeCode);
    connect(&oauth_, &SlackOAuth::failed, this, [this](const QString &error) { fail(error); });
}

SlackProvider::SlackProvider(const Account &account, const TestEndpoints &endpoints, QObject *parent)
    : SlackProvider(account, parent)
{
    if (validTestEndpoint(endpoints.apiBase) && validTestEndpoint(endpoints.authorize)) {
        apiBase_ = endpoints.apiBase;
        authorizationEndpoint_ = endpoints.authorize;
        callbackPort_ = 0;
        historyIntervalMs_ = std::max(1, endpoints.historyIntervalMs);
        listPageIntervalMs_ = std::max(1, endpoints.pageIntervalMs);
        network_.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    }
}

Capabilities SlackProvider::capabilities() const
{
    return Capability::Text | Capability::DirectMessages | Capability::Groups | Capability::Channels
        | Capability::RemoteHistory;
}

void SlackProvider::connectAccount(const QByteArray &secret)
{
    reset();
    setState(ConnectionState::Connecting);
    if (secret.isEmpty()) {
        authorize();
        return;
    }
    if (!restoreCredentials(secret)) {
        fail(tr("Slack requires an authorized user session or a non-rotating xoxp- user token."));
        return;
    }
    setState(ConnectionState::Authenticating);
    if (credentials_.expiresAtMs && credentials_.expiresAtMs - QDateTime::currentMSecsSinceEpoch() <= 60000)
        refreshToken();
    else
        validateToken();
}

void SlackProvider::disconnectAccount()
{
    reset();
    selected_.clear();
    setState(ConnectionState::Disconnected);
}

bool SlackProvider::join(const QString &) { return false; }
bool SlackProvider::part(const QString &) { return false; }
bool SlackProvider::sendCommand(const ConversationId &, const QString &) { return false; }
QString SlackProvider::canonicalTarget(const QString &target) const { return target; }

void SlackProvider::requestHistory(const ConversationId &id)
{
    if (id.accountId != account_.id || id.remoteId.isEmpty())
        return;
    if (selected_ == id.remoteId && (historyTimer_.isActive() || historyInFlight_))
        return;
    selected_ = id.remoteId;
    if (state_ == ConnectionState::Connected && !historyInFlight_)
        historyTimer_.start(0);
}

void SlackProvider::reset()
{
    ++generation_;
    oauth_.stop();
    for (auto *timer : { &listTimer_, &historyTimer_, &refreshTimer_, &authTimer_, &profileTimer_ })
        timer->stop();
    const auto previous = std::exchange(replies_, {});
    for (auto *reply : previous)
        reply->abort();
    credentials_.access.fill('\0');
    credentials_.refresh.fill('\0');
    credentials_ = {};
    credentialsDirty_ = false;
    refreshing_ = false;
    refreshDeferred_ = false;
    historyInFlight_ = false;
    profileInFlight_ = false;
    nextListCursor_.clear();
    listed_.clear();
    channels_.clear();
    directPeers_.clear();
    profiles_.clear();
    pendingProfiles_.clear();
    histories_.clear();
    sending_.clear();
}

void SlackProvider::setState(ConnectionState state)
{
    if (state_ != state) {
        state_ = state;
        emit stateChanged(state);
    }
}

void SlackProvider::fail(const QString &message, bool retryable)
{
    reset();
    setState(ConnectionState::Error);
    emit errorOccurred(message, retryable);
}

bool SlackProvider::restoreCredentials(const QByteArray &secret)
{
    if (secret.size() > 12288)
        return false;
    if (secret.startsWith("xoxp-") && validUserToken(secret)) {
        credentials_.access = secret;
        credentialsDirty_ = true;
        return true;
    }

    const auto stored = QJsonDocument::fromJson(secret).object();
    if (stored.value(u"format"_s).toString() != u"phoschat-slack-v1"_s)
        return false;
    Credentials parsed;
    parsed.access = stored.value(u"access"_s).toString().toLatin1();
    parsed.refresh = stored.value(u"refresh"_s).toString().toLatin1();
    parsed.expiresAtMs = stored.value(u"expires_at_ms"_s).toInteger();
    parsed.workspace = stored.value(u"workspace"_s).toString();
    parsed.user = stored.value(u"user"_s).toString();
    if (!validUserToken(parsed.access) || !validId(parsed.workspace, u"T") || !validId(parsed.user, u"UW")
        || (!parsed.refresh.isEmpty()
            && (!validClientId(account_.providerId) || !validRefreshToken(parsed.refresh)))
        || (parsed.expiresAtMs > 0 && parsed.refresh.isEmpty())
        || (parsed.access.startsWith("xoxe.") && (parsed.refresh.isEmpty() || parsed.expiresAtMs <= 0)))
        return false;
    credentials_ = std::move(parsed);
    return true;
}

void SlackProvider::publishCredentials()
{
    credentialsDirty_ = false;
    const auto stored = QJsonDocument(QJsonObject {
                                          { u"format"_s, u"phoschat-slack-v1"_s },
                                          { u"access"_s, QString::fromLatin1(credentials_.access) },
                                          { u"refresh"_s, QString::fromLatin1(credentials_.refresh) },
                                          { u"expires_at_ms"_s, credentials_.expiresAtMs },
                                          { u"workspace"_s, credentials_.workspace },
                                          { u"user"_s, credentials_.user },
                                      })
                            .toJson(QJsonDocument::Compact);
    emit credentialsUpdated(stored);
}

void SlackProvider::authorize()
{
    if (!validClientId(account_.providerId)) {
        fail(tr("Enter a Slack OAuth Client ID or a Slack user token in account settings."));
        return;
    }
    setState(ConnectionState::Authenticating);
    if (!oauth_.start(account_.providerId, callbackPort_, authorizationEndpoint_))
        fail(tr("PhosChat could not listen on the Slack authorization callback port. Close the other "
                "authorization and retry."));
}

void SlackProvider::exchangeCode(const QString &code, const QString &verifier, const QUrl &redirect)
{
    const QUrlQuery fields {
        { u"client_id"_s, account_.providerId },
        { u"code"_s, code },
        { u"code_verifier"_s, verifier },
        { u"redirect_uri"_s, redirect.toString() },
    };
    request(u"oauth.v2.access"_s, {}, formBody(fields), FormContent, false, [this](const Result &result) {
        if (!result.ok() || !updateCredentials(result.data, true)) {
            fail(tr("Slack could not exchange the authorization code for a user session. Check your app "
                    "settings and try again."));
            return;
        }
        credentialsDirty_ = true;
        validateToken();
    });
}

bool SlackProvider::updateCredentials(const QJsonObject &response, bool exchange)
{
    // Slack puts an initial user grant in authed_user and a user refresh at the top level.
    const auto grant = exchange ? response.value(u"authed_user"_s).toObject() : response;
    const auto token = grant.value(u"access_token"_s).toString().toLatin1();
    const auto refresh = grant.value(u"refresh_token"_s).toString().toLatin1();
    const auto tokenType = grant.value(u"token_type"_s).toString();
    const auto expiresIn = grant.value(u"expires_in"_s).toInteger();
    if (!validUserToken(token) || (!tokenType.isEmpty() && tokenType != u"user"_s)
        || (!refresh.isEmpty() && !validRefreshToken(refresh)) || (expiresIn > 0 && refresh.isEmpty())
        || (!exchange && refresh.isEmpty())
        || (token.startsWith("xoxe.") && (refresh.isEmpty() || expiresIn <= 0)) || expiresIn < 0
        || expiresIn > 86400)
        return false;

    const auto user = exchange ? grant.value(u"id"_s).toString() : grant.value(u"user_id"_s).toString();
    const auto workspace = response.value(u"team"_s).toObject().value(u"id"_s).toString();
    if (exchange) {
        if (!user.isEmpty() && !validId(user, u"UW"))
            return false;
        if (!workspace.isEmpty() && !validId(workspace, u"T"))
            return false;
        credentials_.user = user;
        credentials_.workspace = workspace;
    } else if ((!user.isEmpty() && user != credentials_.user)
        || (!workspace.isEmpty() && workspace != credentials_.workspace)) {
        return false;
    }

    credentials_.access.fill('\0');
    credentials_.refresh.fill('\0');
    credentials_.access = token;
    credentials_.refresh = refresh;
    credentials_.expiresAtMs = expiresIn ? QDateTime::currentMSecsSinceEpoch() + expiresIn * 1000 : 0;
    return true;
}

void SlackProvider::refreshToken()
{
    if (refreshing_ || (refreshDeferred_ && refreshTimer_.isActive()))
        return;
    refreshDeferred_ = false;
    if (credentials_.refresh.isEmpty() || !validClientId(account_.providerId)) {
        fail(tr("This Slack token expired. Authorize PhosChat again."));
        return;
    }
    refreshing_ = true;
    const QUrlQuery fields {
        { u"grant_type"_s, u"refresh_token"_s },
        { u"client_id"_s, account_.providerId },
        { u"refresh_token"_s, QString::fromLatin1(credentials_.refresh) },
    };
    request(u"oauth.v2.access"_s, {}, formBody(fields), FormContent, false, [this](const Result &result) {
        refreshing_ = false;
        if (result.retryAfterMs || result.retryable) {
            refreshDeferred_ = true;
            refreshTimer_.start(result.retryAfterMs ? result.retryAfterMs : 15000);
            return;
        }
        if (!result.ok() || !updateCredentials(result.data, false)) {
            fail(tr("Slack could not refresh the user session. Reauthorize the account."));
            return;
        }
        // The previous refresh token is single-use. Save its replacement before another request.
        publishCredentials();
        if (state_ == ConnectionState::Connected)
            scheduleRefresh();
        else
            validateToken();
    });
}

void SlackProvider::validateToken()
{
    request(u"auth.test"_s, {}, QByteArray(), FormContent, true, [this](const Result &result) {
        if (result.retryAfterMs) {
            authTimer_.start(result.retryAfterMs);
            return;
        }
        if (!result.ok()) {
            if (!handleSessionError(result))
                fail(tr("Slack could not validate the user session."), result.retryable);
            return;
        }
        const auto workspace = result.data.value(u"team_id"_s).toString();
        const auto user = result.data.value(u"user_id"_s).toString();
        if (result.data.contains(u"bot_id"_s) || result.data.value(u"is_enterprise_install"_s).toBool()
            || !validId(workspace, u"T") || !validId(user, u"UW")
            || (!credentials_.workspace.isEmpty() && credentials_.workspace != workspace)
            || (!credentials_.user.isEmpty() && credentials_.user != user)) {
            fail(tr("Slack returned a different identity or an unsupported installation. Use a "
                    "workspace-scoped user grant."));
            return;
        }
        credentials_.workspace = workspace;
        credentials_.user = user;
        const auto nickname = result.data.value(u"user"_s).toString();
        if (!nickname.isEmpty())
            profiles_.insert(user, nickname);
        if (credentialsDirty_)
            publishCredentials();
        setState(ConnectionState::Connected);
        scheduleRefresh();
        listTimer_.start(0);
    });
}

void SlackProvider::scheduleRefresh()
{
    if (!credentials_.expiresAtMs)
        return;
    const auto delay = credentials_.expiresAtMs - QDateTime::currentMSecsSinceEpoch() - 60000;
    refreshTimer_.start(static_cast<int>(std::clamp<qint64>(delay, 1000, std::numeric_limits<int>::max())));
}

ConversationId SlackProvider::conversationId(const QString &channel) const
{
    return { account_.id, credentials_.workspace + u'/' + credentials_.user + u'/' + channel };
}

QString SlackProvider::remoteChannel(const ConversationId &id) const
{
    if (id.accountId != account_.id || credentials_.workspace.isEmpty())
        return {};
    const auto channel = id.remoteId.section(u'/', -1);
    return channels_.contains(channel) && id == conversationId(channel) ? channel : QString();
}

bool SlackProvider::handleSessionError(const Result &result)
{
    if (result.error == u"token_expired"_s && !credentials_.refresh.isEmpty()) {
        refreshToken();
        return true;
    }
    if (result.error == u"invalid_auth"_s || result.error == u"token_expired"_s
        || result.error == u"token_revoked"_s || result.error == u"account_inactive"_s
        || result.error == u"not_authed"_s) {
        fail(tr("Slack authorization is no longer valid. Reauthorize the account."));
        return true;
    }
    return false;
}

void SlackProvider::refreshConversations()
{
    if (state_ != ConnectionState::Connected)
        return;
    const auto key = apiBase_.authority() + u'/' + credentials_.workspace + u"/conversations.list"_s;
    if (const int wait = quota.reserve(key, listPageIntervalMs_)) {
        listTimer_.start(wait);
        return;
    }

    QUrlQuery fields {
        { u"types"_s, u"public_channel,private_channel,im,mpim"_s },
        { u"exclude_archived"_s, u"true"_s },
        { u"limit"_s, u"200"_s },
    };
    if (!nextListCursor_.isEmpty())
        fields.addQueryItem(u"cursor"_s, nextListCursor_);
    else
        listed_.clear();

    request(u"conversations.list"_s, fields, std::nullopt, {}, true, [this, key](const Result &result) {
        if (result.retryAfterMs) {
            quota.backOff(key, result.retryAfterMs);
            listTimer_.start(result.retryAfterMs);
            return;
        }
        if (!result.ok()) {
            if (handleSessionError(result)) {
                if (state_ == ConnectionState::Connected)
                    listTimer_.start(15000);
            } else if (result.retryable) {
                fail(tr("Slack is temporarily unreachable."), true);
            } else {
                emit notice(
                    tr("Slack could not list conversations. Review the app's conversation read scopes."));
                nextListCursor_.clear();
                listTimer_.start(ListRefreshMs);
            }
            return;
        }

        for (const auto &entry : result.data.value(u"channels"_s).toArray()) {
            const auto details = entry.toObject();
            const auto channel = details.value(u"id"_s).toString();
            const bool direct = details.value(u"is_im"_s).toBool();
            const bool group = details.value(u"is_mpim"_s).toBool();
            if (!validId(channel, u"CDG") || details.value(u"is_archived"_s).toBool()
                || (!direct && !group && !details.value(u"is_member"_s).toBool()))
                continue;
            listed_.insert(channel);
            Conversation value;
            value.id = conversationId(channel);
            value.joined = true;
            value.kind = direct ? ConversationKind::Direct
                : group         ? ConversationKind::Group
                                : ConversationKind::Channel;
            auto name = details.value(u"name"_s).toString().left(256);
            if (direct) {
                const auto peer = details.value(u"user"_s).toString();
                if (validId(peer, u"UW")) {
                    directPeers_.insert(channel, peer);
                    queueProfile(peer);
                    name = profiles_.value(peer, peer);
                }
            } else if (!group && !name.isEmpty()) {
                name.prepend(u'#');
            }
            value.title = name.isEmpty() ? channel : name;
            if (!channels_.contains(channel) || channels_.value(channel).title != value.title
                || channels_.value(channel).kind != value.kind) {
                channels_.insert(channel, value);
                emit conversationChanged(value);
            }
        }

        const auto cursor = nextCursor(result.data);
        if (!cursor.isEmpty() && cursor != nextListCursor_) {
            nextListCursor_ = cursor;
            listTimer_.start(listPageIntervalMs_);
        } else {
            nextListCursor_.clear();
            const auto known = channels_.keys();
            for (const auto &channel : known) {
                if (!listed_.contains(channel)) {
                    auto value = channels_.take(channel);
                    value.joined = false;
                    directPeers_.remove(channel);
                    histories_.remove(channel);
                    emit conversationChanged(value);
                }
            }
            listTimer_.start(ListRefreshMs);
        }
        if (!selected_.isEmpty() && !historyTimer_.isActive() && !historyInFlight_)
            historyTimer_.start(0);
    });
}

void SlackProvider::readHistory()
{
    if (state_ != ConnectionState::Connected || historyInFlight_)
        return;
    const auto channel = remoteChannel({ account_.id, selected_ });
    if (channel.isEmpty())
        return;
    const auto key = apiBase_.authority() + u'/' + credentials_.workspace + u"/conversations.history"_s;
    if (const int wait = quota.reserve(key, historyIntervalMs_)) {
        historyTimer_.start(wait);
        return;
    }

    const auto current = histories_.value(channel);
    QUrlQuery fields { { u"channel"_s, channel }, { u"limit"_s, u"15"_s } };
    if (!current.anchor.isEmpty())
        fields.addQueryItem(u"oldest"_s, current.anchor);
    if (!current.cursor.isEmpty())
        fields.addQueryItem(u"cursor"_s, current.cursor);
    if (!current.before.isEmpty())
        fields.addQueryItem(u"latest"_s, current.before);

    historyInFlight_ = true;
    request(u"conversations.history"_s, fields, std::nullopt, {}, true,
        [this, key, channel](const Result &result) {
            historyInFlight_ = false;
            if (result.retryAfterMs)
                quota.backOff(key, result.retryAfterMs);
            else if (!result.ok()) {
                if (!handleSessionError(result)) {
                    if (result.retryable) {
                        fail(tr("Slack is temporarily unreachable."), true);
                        return;
                    }
                    emit notice(tr("Slack could not read this conversation. Review workspace access and "
                                   "history scopes."));
                    if (selected_ == conversationId(channel).remoteId)
                        selected_.clear();
                }
            } else if (channels_.contains(channel)) {
                auto &history = histories_[channel];
                const auto messages = result.data.value(u"messages"_s).toArray();
                QString oldest;
                for (qsizetype index = messages.size(); index > 0; --index) {
                    const auto message = messages.at(index - 1).toObject();
                    const auto timestamp = message.value(u"ts"_s).toString();
                    const auto value = timestampValue(timestamp);
                    if (!value)
                        continue;
                    if (oldest.isEmpty() || *value < timestampValue(oldest).value_or(0))
                        oldest = timestamp;
                    history.newest = newest(history.newest, timestamp);
                    emitText(channel, message, !history.loaded);
                }

                const auto cursor = nextCursor(result.data);
                const bool cursorPage = !cursor.isEmpty() && cursor != history.cursor;
                const bool timePage = result.data.value(u"has_more"_s).toBool() && !oldest.isEmpty()
                    && oldest != history.before;
                if (history.loaded && (cursorPage || timePage) && ++history.pages < 4) {
                    if (cursorPage)
                        history.cursor = cursor;
                    else {
                        history.cursor.clear();
                        history.before = oldest;
                    }
                } else {
                    history.anchor = newest(history.anchor, history.newest);
                    history.loaded = true;
                    history.newest.clear();
                    history.cursor.clear();
                    history.before.clear();
                    history.pages = 0;
                }
            }
            if (state_ == ConnectionState::Connected && !selected_.isEmpty())
                historyTimer_.start(0);
        });
}

void SlackProvider::queueProfile(const QString &id)
{
    if (!validId(id, u"UW") || profiles_.contains(id) || pendingProfiles_.contains(id)
        || profiles_.size() + pendingProfiles_.size() >= MaxProfiles)
        return;
    pendingProfiles_.append(id);
    if (!profileTimer_.isActive() && !profileInFlight_)
        profileTimer_.start(0);
}

void SlackProvider::readProfile()
{
    if (state_ != ConnectionState::Connected || pendingProfiles_.isEmpty() || profileInFlight_)
        return;
    const auto key = apiBase_.authority() + u'/' + credentials_.workspace + u"/users.info"_s;
    if (const int wait = quota.reserve(key, 1050)) {
        profileTimer_.start(wait);
        return;
    }
    const auto user = pendingProfiles_.takeFirst();
    profiles_.insert(user, user);
    profileInFlight_ = true;
    request(u"users.info"_s, { { u"user"_s, user } }, std::nullopt, {}, true,
        [this, key, user](const Result &result) {
            profileInFlight_ = false;
            if (result.retryAfterMs) {
                quota.backOff(key, result.retryAfterMs);
                pendingProfiles_.prepend(user);
            } else if (result.ok()) {
                const auto details = result.data.value(u"user"_s).toObject();
                const auto profile = details.value(u"profile"_s).toObject();
                auto name = profile.value(u"display_name"_s).toString().trimmed().left(256);
                if (name.isEmpty())
                    name = details.value(u"real_name"_s).toString().trimmed().left(256);
                if (name.isEmpty())
                    name = details.value(u"name"_s).toString().trimmed().left(256);
                profiles_.insert(user, name.isEmpty() ? user : name);
                for (auto it = directPeers_.cbegin(); it != directPeers_.cend(); ++it) {
                    if (it.value() != user || !channels_.contains(it.key()) || name.isEmpty())
                        continue;
                    auto &conversation = channels_[it.key()];
                    conversation.title = name;
                    emit conversationChanged(conversation);
                }
            } else {
                // A failed name lookup must not prevent messaging or cause continuous retries.
                profiles_.insert(user, user);
            }
            if (!pendingProfiles_.isEmpty())
                profileTimer_.start(result.retryAfterMs ? result.retryAfterMs : 0);
        });
}

void SlackProvider::emitText(
    const QString &channel, const QJsonObject &object, bool historical, bool outgoing)
{
    if (!channels_.contains(channel))
        return;
    const auto text = object.value(u"text"_s).toString();
    const auto timestamp = object.value(u"ts"_s).toString();
    const auto micros = timestampValue(timestamp);
    const auto type = object.value(u"type"_s).toString();
    const auto subtype = object.value(u"subtype"_s).toString();
    if (text.isEmpty() || !micros || (!type.isEmpty() && type != u"message"_s)
        || object.value(u"hidden"_s).toBool()
        || (!subtype.isEmpty() && subtype != u"bot_message"_s && subtype != u"me_message"_s
            && subtype != u"file_share"_s && subtype != u"thread_broadcast"_s))
        return;

    auto user = object.value(u"user"_s).toString();
    if (user.isEmpty() && outgoing)
        user = credentials_.user;
    queueProfile(user);
    Message message;
    message.conversation = conversationId(channel);
    message.id = timestamp;
    message.sender = user.isEmpty() ? object.value(u"username"_s).toString(u"Slack"_s).left(256)
                                    : profiles_.value(user, user);
    message.text = plainSlackText(text.left(40000), profiles_);
    message.timestamp = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(*micros / 1000), QTimeZone::UTC);
    message.outgoing = outgoing || user == credentials_.user;
    message.mention = text.contains(u"<@"_s + credentials_.user + u'>');
    message.historical = historical;
    emit messageReceived(message);
}

bool SlackProvider::sendText(const ConversationId &id, const QString &text)
{
    const auto channel = remoteChannel(id);
    if (state_ != ConnectionState::Connected || channel.isEmpty() || text.trimmed().isEmpty())
        return false;
    if (text.size() > 4000) {
        emit notice(tr("Slack messages are limited to 4,000 characters in PhosChat."));
        return false;
    }
    const auto key = apiBase_.authority() + u'/' + credentials_.workspace + u"/chat.postMessage"_s;
    if (sending_.contains(channel) || quota.reserve(key, 0) || quota.reserve(key + u'/' + channel, 1050)) {
        emit notice(tr("Slack is limiting message sends. Wait before trying again."));
        return false;
    }

    sending_.insert(channel);
    const QJsonObject fields {
        { u"channel"_s, channel },
        { u"text"_s, slackPlainInput(text) },
        { u"mrkdwn"_s, false },
        { u"unfurl_links"_s, false },
        { u"unfurl_media"_s, false },
    };
    request(u"chat.postMessage"_s, {}, QJsonDocument(fields).toJson(QJsonDocument::Compact), JsonContent,
        true, [this, key, channel, text](const Result &result) {
            sending_.remove(channel);
            if (!result.ok()) {
                if (result.retryAfterMs)
                    quota.backOff(key, result.retryAfterMs);
                if (!handleSessionError(result))
                    emit notice(
                        tr("Slack could not confirm the send. Check the conversation before retrying."));
                return;
            }
            auto message = result.data.value(u"message"_s).toObject();
            if (!message.contains(u"ts"_s))
                message.insert(u"ts"_s, result.data.value(u"ts"_s));
            if (!message.contains(u"text"_s))
                message.insert(u"text"_s, slackPlainInput(text));
            emitText(channel, message, false, true);
        });
    return true;
}

void SlackProvider::request(const QString &method, const QUrlQuery &query, std::optional<QByteArray> body,
    const QByteArray &contentType, bool authenticated, Finished finished)
{
    QUrl url = apiBase_.resolved(QUrl(method));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    request.setRawHeader("Accept", "application/json");
    if (authenticated)
        request.setRawHeader("Authorization", "Bearer " + credentials_.access);

    QNetworkReply *reply;
    if (body) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        reply = network_.post(request, *body);
    } else {
        reply = network_.get(request);
    }
    reply->setReadBufferSize(MaxResponseBytes + 1);
    replies_.insert(reply);
    const unsigned generation = generation_;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        *bytes += reply->read(MaxResponseBytes + 1 - bytes->size());
        if (bytes->size() > MaxResponseBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, bytes, generation, finished = std::move(finished)] {
            replies_.remove(reply);
            reply->deleteLater();
            if (generation != generation_)
                return;
            if (bytes->size() <= MaxResponseBytes && reply->isReadable())
                *bytes += reply->read(MaxResponseBytes + 1 - bytes->size());

            Result result;
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 429) {
                bool valid = false;
                const auto seconds = reply->rawHeader("Retry-After").toLongLong(&valid);
                result.retryAfterMs
                    = static_cast<int>((valid ? std::clamp<qint64>(seconds, 1, 86400) : 60) * 1000);
                result.error = u"ratelimited"_s;
            } else if (bytes->size() > MaxResponseBytes || (status >= 300 && status < 400)) {
                result.error = u"invalid_response"_s;
            } else if (status >= 500 || (status == 0 && reply->error() != QNetworkReply::NoError)) {
                result.error = u"network_error"_s;
                result.retryable = reply->error() != QNetworkReply::SslHandshakeFailedError;
            } else {
                result.data = QJsonDocument::fromJson(*bytes).object();
                if (status < 200 || status >= 300 || !result.data.value(u"ok"_s).toBool()) {
                    result.error = result.data.value(u"error"_s).toString();
                    if (result.error.isEmpty())
                        result.error = u"invalid_response"_s;
                }
            }
            finished(result);
        });
}

} // namespace phoschat
