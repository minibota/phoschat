#include "providers/telegram/telegram_provider.h"

#include "providers/telegram/telegram_transport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>

#include <cmath>
#include <utility>

namespace phoschat {
namespace {

    using namespace Qt::StringLiterals;

    constexpr qint64 MaxTelegramInteger = 9007199254740991;
    constexpr int ChatPageSize = 100;
    constexpr int MaxChatPages = 50;
    constexpr int HistoryLimit = 50;

    QString typeOf(const QJsonObject &object) { return object.value(u"@type"_s).toString(); }

    qint64 telegramId(const QJsonValue &value)
    {
        if (value.isString()) {
            bool valid = false;
            const auto id = value.toString().toLongLong(&valid);
            return valid && id >= -MaxTelegramInteger && id <= MaxTelegramInteger ? id : 0;
        }
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > MaxTelegramInteger)
            return 0;
        return static_cast<qint64>(number);
    }

    QString persistentProfile(const QString &accountId)
    {
        const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (data.isEmpty() || accountId.isEmpty())
            return {};
        const auto directory
            = QCryptographicHash::hash(accountId.toUtf8(), QCryptographicHash::Sha256).toHex();
        return QDir(data).absoluteFilePath(u"telegram/"_s + QString::fromLatin1(directory));
    }

    bool makePrivateDirectory(const QString &path)
    {
        const QFileInfo directory(path);
        if (!directory.isAbsolute() || directory.isSymLink() || !QDir().mkpath(path))
            return false;
#ifdef Q_OS_UNIX
        if (!QFile::setPermissions(
                path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            return false;
#endif
        return true;
    }

    bool validApiHash(const QByteArray &secret)
    {
        if (secret.size() != 32)
            return false;
        for (const char value : secret) {
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
                    || (value >= 'A' && value <= 'F')))
                return false;
        }
        return true;
    }

    QString errorCode(const QJsonObject &response)
    {
        const auto code = response.value(u"code"_s).toInt();
        return code == 0 ? QStringLiteral("unknown") : QString::number(code);
    }

} // namespace

TelegramProvider::TelegramProvider(Account account, QObject *parent)
    : TelegramProvider(std::move(account), TelegramTransport::native(), parent)
{
}

TelegramProvider::TelegramProvider(Account account, TelegramTransport &transport, QObject *parent)
    : IChatProvider(parent)
    , account_(std::move(account))
    , transport_(transport)
{
    connect(&transport_, &TelegramTransport::received, this, [this](int id, const QJsonObject &event) {
        if (client_ > 0 && id == client_)
            processEvent(event);
    });
}

TelegramProvider::~TelegramProvider()
{
    if (client_ > 0)
        transport_.closeClient(client_);
    apiHash_.fill('\0');
}

bool TelegramProvider::isAvailable() { return TelegramTransport::native().isAvailable(); }

void TelegramProvider::removeLocalData(const QString &accountId)
{
    const auto profile = persistentProfile(accountId);
    if (!profile.isEmpty())
        TelegramTransport::native().forgetProfile(profile);
}

Capabilities TelegramProvider::capabilities() const
{
    return Capability::Text | Capability::DirectMessages | Capability::Groups | Capability::RemoteHistory;
}

void TelegramProvider::connectAccount(const QByteArray &secret)
{
    if (client_ > 0)
        disconnectAccount();
    reset();
    if (!transport_.isAvailable()) {
        fail(tr("Install TDLib 1.8.38 or newer to connect to Telegram."));
        return;
    }

    bool apiIdValid = false;
    const int apiId = account_.providerId.toInt(&apiIdValid);
    if (!apiIdValid || apiId <= 0 || !validApiHash(secret)) {
        fail(tr("Enter a valid Telegram API ID and API hash in account settings."));
        return;
    }

    std::shared_ptr<QTemporaryDir> temporary;
    if (account_.saveSecret) {
        profile_ = persistentProfile(account_.id);
        if (profile_.isEmpty() || !makePrivateDirectory(QFileInfo(profile_).absolutePath())
            || !makePrivateDirectory(profile_)) {
            fail(tr("Could not create a private Telegram session directory."));
            return;
        }
    } else {
        temporary = std::make_shared<QTemporaryDir>(QDir::temp().filePath(u"phoschat-telegram-XXXXXX"_s));
        if (!temporary->isValid()) {
            fail(tr("Could not create a temporary Telegram session directory."));
            return;
        }
        profile_ = temporary->path();
    }

    apiHash_ = secret.toLower();
    client_ = transport_.createClient(profile_, std::move(temporary));
    if (client_ <= 0) {
        fail(tr("The previous Telegram session is still closing."), true);
        return;
    }

    setState(ConnectionState::Connecting);
    request({ { u"@type"_s, u"getAuthorizationState"_s } }, u"boot"_s);
}

void TelegramProvider::disconnectAccount()
{
    if (client_ > 0)
        transport_.closeClient(std::exchange(client_, 0));
    reset();
    setState(ConnectionState::Disconnected);
}

bool TelegramProvider::join(const QString &) { return false; }
bool TelegramProvider::part(const QString &) { return false; }

bool TelegramProvider::sendText(const ConversationId &id, const QString &text)
{
    const auto remoteId = chatId(id);
    const auto chat = chats_.constFind(remoteId);
    if (state_ != ConnectionState::Connected || chat == chats_.cend() || !chat->canSend
        || chat->protectedContent || chat->autoDeletes) {
        emit notice(tr("This Telegram conversation is unavailable for sending."));
        return false;
    }
    if (text.trimmed().isEmpty() || text.size() > 4096) {
        emit notice(tr("Telegram messages must contain between 1 and 4096 characters."));
        return false;
    }
    QJsonObject formatted { { u"@type"_s, u"formattedText"_s }, { u"text"_s, text },
        { u"entities"_s, QJsonArray {} } };
    QJsonObject preview { { u"@type"_s, u"linkPreviewOptions"_s }, { u"is_disabled"_s, true } };
    QJsonObject content { { u"@type"_s, u"inputMessageText"_s }, { u"text"_s, formatted },
        { u"link_preview_options"_s, preview }, { u"clear_draft"_s, false } };
    request({ { u"@type"_s, u"sendMessage"_s }, { u"chat_id"_s, remoteId },
                { u"input_message_content"_s, content } },
        u"send"_s, remoteId);
    return true;
}

bool TelegramProvider::sendCommand(const ConversationId &, const QString &)
{
    emit notice(tr("IRC commands are unavailable in Telegram conversations."));
    return false;
}

QString TelegramProvider::canonicalTarget(const QString &target) const
{
    const ConversationId id { account_.id, target.trimmed() };
    const auto remoteId = chatId(id);
    return remoteId ? QString::number(owner_) + u'/' + QString::number(remoteId) : QString {};
}

void TelegramProvider::submitAuthentication(AuthenticationKind kind, const QString &value)
{
    if (!client_ || challenge_ != kind || authenticationPending_)
        return;

    QJsonObject authentication;
    switch (kind) {
    case AuthenticationKind::Phone: {
        static const QRegularExpression separator(u"[ ()-]"_s);
        static const QRegularExpression phoneNumber(u"^\\+[1-9][0-9]{5,14}$"_s);
        QString phone = value.trimmed();
        phone.remove(separator);
        if (phoneNumber.match(phone).hasMatch())
            authentication
                = { { u"@type"_s, u"setAuthenticationPhoneNumber"_s }, { u"phone_number"_s, phone } };
        break;
    }
    case AuthenticationKind::Code:
        if (!value.trimmed().isEmpty())
            authentication = { { u"@type"_s, u"checkAuthenticationCode"_s }, { u"code"_s, value.trimmed() } };
        break;
    case AuthenticationKind::Password:
        if (!value.isEmpty())
            authentication = { { u"@type"_s, u"checkAuthenticationPassword"_s }, { u"password"_s, value } };
        break;
    case AuthenticationKind::EmailAddress:
        if (value.trimmed().contains(u'@'))
            authentication = { { u"@type"_s, u"setAuthenticationEmailAddress"_s },
                { u"email_address"_s, value.trimmed() } };
        break;
    case AuthenticationKind::EmailCode:
        if (!value.trimmed().isEmpty()) {
            authentication = { { u"@type"_s, u"checkAuthenticationEmailCode"_s },
                { u"code"_s,
                    QJsonObject { { u"@type"_s, u"emailAddressAuthenticationCode"_s },
                        { u"code"_s, value.trimmed() } } } };
        }
        break;
    case AuthenticationKind::Registration:
    case AuthenticationKind::Unsupported:
        return;
    }

    if (authentication.isEmpty() || value.size() > 1024) {
        QTimer::singleShot(0, this, [this, kind] {
            if (challenge_ == kind)
                emit authenticationRequired(kind, tr("Enter a valid value for this Telegram sign-in step."));
        });
        return;
    }

    authenticationPending_ = true;
    request(authentication, u"authentication"_s);
}

void TelegramProvider::markRead(const ConversationId &id)
{
    const auto remoteId = chatId(id);
    if (!remoteId || state_ != ConnectionState::Connected)
        return;
    readPending_.insert(remoteId);
    sendRead(remoteId);
}

void TelegramProvider::requestHistory(const ConversationId &id)
{
    const auto remoteId = chatId(id);
    if (!remoteId || state_ != ConnectionState::Connected || historyLoaded_.contains(remoteId)
        || historyPending_.contains(remoteId))
        return;
    historyPending_.insert(remoteId);
    if (chats_.contains(remoteId))
        fetchHistory(remoteId);
    else if (!chatPending_.contains(remoteId)) {
        chatPending_.insert(remoteId);
        request({ { u"@type"_s, u"getChat"_s }, { u"chat_id"_s, remoteId } }, u"chat"_s, remoteId);
    }
}

void TelegramProvider::setState(ConnectionState state)
{
    if (std::exchange(state_, state) != state)
        emit stateChanged(state);
}

void TelegramProvider::reset()
{
    profile_.clear();
    apiHash_.fill('\0');
    apiHash_.clear();
    owner_ = 0;
    challenge_.reset();
    challengeDetail_.clear();
    authenticationPending_ = false;
    authorized_ = false;
    networkReady_ = false;
    pagesRequested_ = 0;
    chats_.clear();
    users_.clear();
    historyPending_.clear();
    historyLoaded_.clear();
    chatPending_.clear();
    readPending_.clear();
}

void TelegramProvider::fail(const QString &message, bool retryable)
{
    if (client_ > 0)
        transport_.closeClient(std::exchange(client_, 0));
    reset();
    setState(ConnectionState::Error);
    emit errorOccurred(message, retryable);
}

void TelegramProvider::request(QJsonObject object, const QString &operation, qint64 chatId)
{
    if (client_ <= 0)
        return;
    object.insert(u"@extra"_s, QJsonObject { { u"operation"_s, operation }, { u"chat"_s, chatId } });
    transport_.send(client_, object);
}

void TelegramProvider::processEvent(const QJsonObject &event)
{
    const auto type = typeOf(event);
    if (event.value(u"@extra"_s).isObject()) {
        processResult(event);
    } else if (type == u"updateAuthorizationState") {
        processAuthorization(event.value(u"authorization_state"_s).toObject());
    } else if (type == u"updateConnectionState") {
        networkReady_ = typeOf(event.value(u"state"_s).toObject()) == u"connectionStateReady";
        updateConnectionState();
    } else if (type == u"updateUser") {
        updateUser(event.value(u"user"_s).toObject());
    } else if (type == u"updateNewChat") {
        acceptChat(event.value(u"chat"_s).toObject());
    } else if (type == u"updateNewMessage" || type == u"updateMessageSendSucceeded") {
        acceptMessage(event.value(u"message"_s).toObject(), false);
    } else if (type == u"updateMessageSendFailed") {
        emit notice(tr("Telegram could not send the message (error %1).")
                .arg(errorCode(event.value(u"error"_s).toObject())));
    } else if (type == u"updateDeleteMessages" && owner_ && !event.value(u"from_cache"_s).toBool()) {
        const auto chat = telegramId(event.value(u"chat_id"_s));
        if (!chats_.contains(chat))
            return;
        const ConversationId id { account_.id, QString::number(owner_) + u'/' + QString::number(chat) };
        const auto messages = event.value(u"message_ids"_s).toArray();
        for (const auto message : messages) {
            if (const auto messageId = telegramId(message); messageId > 0)
                emit messageRemoved(id, QString::number(messageId));
        }
    } else {
        const auto id = telegramId(event.value(u"chat_id"_s));
        const auto chat = chats_.find(id);
        if (chat == chats_.end())
            return;
        if (type == u"updateChatTitle") {
            chat->title = event.value(u"title"_s).toString();
        } else if (type == u"updateChatPermissions") {
            chat->canSend
                = event.value(u"permissions"_s).toObject().value(u"can_send_basic_messages"_s).toBool();
        } else if (type == u"updateChatHasProtectedContent") {
            chat->protectedContent = event.value(u"has_protected_content"_s).toBool();
        } else if (type == u"updateChatMessageAutoDeleteTime") {
            chat->autoDeletes = event.value(u"message_auto_delete_time"_s).toInt() > 0;
        } else {
            return;
        }
        announce(id, *chat);
    }
}

void TelegramProvider::processResult(const QJsonObject &event)
{
    const auto tag = event.value(u"@extra"_s).toObject();
    const auto operation = tag.value(u"operation"_s).toString();
    const auto id = telegramId(tag.value(u"chat"_s));
    const auto type = typeOf(event);
    const bool error = type == u"error";
    if (operation == u"boot") {
        if (error)
            fail(tr("Telegram could not initialize (error %1).").arg(errorCode(event)));
        else
            processAuthorization(event);
    } else if (operation == u"parameters") {
        if (error)
            fail(tr("Telegram could not open the session (error %1). Check the API credentials.")
                    .arg(errorCode(event)));
    } else if (operation == u"authentication" && error && challenge_) {
        authenticationPending_ = false;
        emit authenticationRequired(*challenge_,
            tr("Telegram did not accept the previous value (error %1). %2")
                .arg(errorCode(event), challengeDetail_));
    } else if (operation == u"me") {
        owner_ = telegramId(event.value(u"id"_s));
        if (error || owner_ <= 0) {
            fail(tr("Telegram could not verify the signed-in account."));
            return;
        }
        updateUser(event);
        for (auto chat = chats_.cbegin(); chat != chats_.cend(); ++chat)
            announce(chat.key(), *chat);
        updateConnectionState();
        loadMoreChats();
    } else if (operation == u"chats") {
        if (!error)
            loadMoreChats();
        else if (event.value(u"code"_s).toInt() != 404)
            emit notice(tr("Telegram could not finish loading chats (error %1).").arg(errorCode(event)));
    } else if (operation == u"chat") {
        chatPending_.remove(id);
        if (!error)
            acceptChat(event);
        if (chats_.contains(id))
            fetchHistory(id);
        else
            historyPending_.remove(id);
    } else if (operation == u"history") {
        historyPending_.remove(id);
        if (error) {
            emit notice(tr("Telegram could not load recent messages (error %1).").arg(errorCode(event)));
            return;
        }
        historyLoaded_.insert(id);
        const auto messages = event.value(u"messages"_s).toArray();
        for (auto index = messages.size(); index > 0; --index)
            acceptMessage(messages.at(index - 1).toObject(), true);
        sendRead(id);
    } else if (operation == u"send") {
        if (error)
            emit notice(tr("Telegram could not send the message (error %1).").arg(errorCode(event)));
        else
            acceptMessage(event, false);
    }
}

void TelegramProvider::processAuthorization(const QJsonObject &state)
{
    const auto type = typeOf(state);
    if (type == u"authorizationStateWaitTdlibParameters") {
        if (!apiHash_.isEmpty())
            sendParameters();
    } else if (type == u"authorizationStateWaitPhoneNumber") {
        prompt(AuthenticationKind::Phone, tr("Enter your phone number with its country code."));
    } else if (type == u"authorizationStateWaitCode") {
        prompt(AuthenticationKind::Code, tr("Enter the sign-in code from Telegram."));
    } else if (type == u"authorizationStateWaitPassword") {
        prompt(AuthenticationKind::Password, tr("Enter your Telegram two-step verification password."));
    } else if (type == u"authorizationStateWaitEmailAddress") {
        prompt(AuthenticationKind::EmailAddress, tr("Enter the email address requested by Telegram."));
    } else if (type == u"authorizationStateWaitEmailCode") {
        prompt(
            AuthenticationKind::EmailCode, tr("Enter the verification code sent by Telegram to your email."));
    } else if (type == u"authorizationStateWaitRegistration") {
        prompt(AuthenticationKind::Registration,
            tr("Register your account with an official Telegram client first."));
    } else if (type == u"authorizationStateWaitOtherDeviceConfirmation") {
        prompt(AuthenticationKind::Unsupported,
            tr("Telegram requires sign-in confirmation on another device. Retry from an existing account."));
    } else if (type == u"authorizationStateReady" && !authorized_) {
        authorized_ = true;
        challenge_.reset();
        challengeDetail_.clear();
        authenticationPending_ = false;
        request({ { u"@type"_s, u"getMe"_s } }, u"me"_s);
    } else if (type == u"authorizationStateClosed") {
        fail(tr("The Telegram session closed. Connect again to continue."));
    }
}

void TelegramProvider::sendParameters()
{
    const auto encryptionKey = QMessageAuthenticationCode::hash(
        QByteArrayLiteral("PhosChat TDLib profile v1\0") + account_.id.toUtf8(),
        QByteArray::fromHex(apiHash_), QCryptographicHash::Sha256);
    const auto version = QCoreApplication::applicationVersion();
    request(
        { { u"@type"_s, u"setTdlibParameters"_s }, { u"use_test_dc"_s, false },
            { u"database_directory"_s, profile_ }, { u"files_directory"_s, profile_ },
            { u"database_encryption_key"_s, QString::fromLatin1(encryptionKey.toBase64()) },
            { u"use_file_database"_s, false }, { u"use_chat_info_database"_s, false },
            { u"use_message_database"_s, false }, { u"use_secret_chats"_s, false },
            { u"api_id"_s, account_.providerId.toInt() }, { u"api_hash"_s, QString::fromLatin1(apiHash_) },
            { u"system_language_code"_s, u"en"_s }, { u"device_model"_s, u"PhosChat Desktop"_s },
            { u"system_version"_s, QSysInfo::prettyProductName() },
            { u"application_version"_s, version.isEmpty() ? u"0.2.0"_s : version } },
        u"parameters"_s);
    apiHash_.fill('\0');
    apiHash_.clear();
}

void TelegramProvider::prompt(AuthenticationKind kind, const QString &detail)
{
    if (challenge_ == kind && !authenticationPending_)
        return;
    challenge_ = kind;
    challengeDetail_ = detail;
    authenticationPending_ = false;
    setState(ConnectionState::Authenticating);
    emit authenticationRequired(kind, detail);
}

void TelegramProvider::updateConnectionState()
{
    if (!authorized_ || !owner_)
        return;
    setState(networkReady_ ? ConnectionState::Connected : ConnectionState::Reconnecting);
}

void TelegramProvider::updateUser(const QJsonObject &user)
{
    const auto id = telegramId(user.value(u"id"_s));
    const auto name
        = (user.value(u"first_name"_s).toString() + u' ' + user.value(u"last_name"_s).toString()).trimmed();
    if (id > 0 && !name.isEmpty())
        users_.insert(id, name);
}

void TelegramProvider::loadMoreChats()
{
    if (!owner_ || pagesRequested_ >= MaxChatPages)
        return;
    ++pagesRequested_;
    request({ { u"@type"_s, u"loadChats"_s },
                { u"chat_list"_s, QJsonObject { { u"@type"_s, u"chatListMain"_s } } },
                { u"limit"_s, ChatPageSize } },
        u"chats"_s);
}

void TelegramProvider::acceptChat(const QJsonObject &object)
{
    const auto id = telegramId(object.value(u"id"_s));
    const auto kind = object.value(u"type"_s).toObject();
    const auto type = typeOf(kind);
    if (!id
        || (type != u"chatTypePrivate" && type != u"chatTypeBasicGroup"
            && (type != u"chatTypeSupergroup" || kind.value(u"is_channel"_s).toBool())))
        return;
    auto &chat = chats_[id];
    chat.title = object.value(u"title"_s).toString();
    chat.kind = type == u"chatTypePrivate" ? ConversationKind::Direct : ConversationKind::Group;
    chat.canSend = object.value(u"permissions"_s).toObject().value(u"can_send_basic_messages"_s).toBool(true);
    chat.protectedContent = object.value(u"has_protected_content"_s).toBool();
    chat.autoDeletes = object.value(u"message_auto_delete_time"_s).toInt() > 0;
    announce(id, chat);
}

void TelegramProvider::announce(qint64 chatId, const Chat &chat)
{
    if (!owner_ || chat.protectedContent || chat.autoDeletes)
        return;
    Conversation conversation;
    conversation.id = { account_.id, QString::number(owner_) + u'/' + QString::number(chatId) };
    conversation.title = chat.title.isEmpty() ? tr("Telegram conversation") : chat.title;
    conversation.kind = chat.kind;
    conversation.joined = chat.canSend;
    emit conversationChanged(conversation);
}

void TelegramProvider::acceptMessage(const QJsonObject &message, bool historical)
{
    const auto id = telegramId(message.value(u"id"_s));
    const auto remote = telegramId(message.value(u"chat_id"_s));
    auto chat = chats_.find(remote);
    if (!owner_ || id <= 0 || chat == chats_.end() || chat->protectedContent || chat->autoDeletes
        || message.value(u"sending_state"_s).isObject() || message.value(u"self_destruct_type"_s).isObject()
        || message.value(u"auto_delete_in"_s).toDouble() > 0
        || message.value(u"self_destruct_in"_s).toDouble() > 0
        || !message.value(u"can_be_saved"_s).toBool(true))
        return;
    const auto content = message.value(u"content"_s).toObject();
    if (typeOf(content) != u"messageText")
        return;
    const auto text = content.value(u"text"_s).toObject().value(u"text"_s).toString();
    if (text.isEmpty())
        return;

    const auto sender = message.value(u"sender_id"_s).toObject();
    const auto userId = telegramId(sender.value(u"user_id"_s));
    const bool outgoing = message.value(u"is_outgoing"_s).toBool();
    QString senderName = users_.value(userId);
    if (senderName.isEmpty()) {
        if (outgoing)
            senderName = tr("You");
        else if (chat->kind == ConversationKind::Direct && !chat->title.isEmpty())
            senderName = chat->title;
        else
            senderName = tr("Telegram participant");
    }
    chat->latest = qMax(chat->latest, id);

    Message value;
    value.conversation = { account_.id, QString::number(owner_) + u'/' + QString::number(remote) };
    value.id = QString::number(id);
    value.sender = senderName;
    value.text = text;
    value.timestamp = QDateTime::fromSecsSinceEpoch(message.value(u"date"_s).toInteger(), QTimeZone::UTC);
    value.outgoing = outgoing;
    value.mention = message.value(u"contains_unread_mention"_s).toBool();
    value.historical = historical;
    emit messageReceived(value);
}

qint64 TelegramProvider::chatId(const ConversationId &id) const
{
    if (!owner_ || id.accountId != account_.id)
        return 0;
    const auto parts = id.remoteId.split(u'/');
    if (parts.size() != 2 || parts.at(0) != QString::number(owner_))
        return 0;
    return telegramId(QJsonValue(parts.at(1)));
}

void TelegramProvider::fetchHistory(qint64 id)
{
    const auto chat = chats_.constFind(id);
    if (chat == chats_.cend() || chat->protectedContent || chat->autoDeletes) {
        historyPending_.remove(id);
        return;
    }
    request({ { u"@type"_s, u"getChatHistory"_s }, { u"chat_id"_s, id }, { u"from_message_id"_s, 0 },
                { u"offset"_s, 0 }, { u"limit"_s, HistoryLimit }, { u"only_local"_s, false } },
        u"history"_s, id);
}

void TelegramProvider::sendRead(qint64 id)
{
    if (!readPending_.contains(id))
        return;
    const auto chat = chats_.constFind(id);
    if (chat == chats_.cend() || !chat->latest || chat->protectedContent || chat->autoDeletes)
        return;
    readPending_.remove(id);
    request({ { u"@type"_s, u"viewMessages"_s }, { u"chat_id"_s, id },
                { u"message_ids"_s, QJsonArray { chat->latest } },
                { u"source"_s, QJsonObject { { u"@type"_s, u"messageSourceChatHistory"_s } } },
                { u"force_read"_s, true } },
        u"read"_s, id);
}

} // namespace phoschat
