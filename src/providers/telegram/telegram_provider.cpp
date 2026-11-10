#include "providers/telegram/telegram_provider.h"

#include "providers/telegram/telegram_transport.h"
#include "security/telegram_app_credentials.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace phoschat {
namespace {

    using namespace Qt::StringLiterals;

    constexpr qint64 MaxTelegramInteger = 9007199254740991;
    constexpr int ChatPageSize = 100;
    constexpr int HistoryLimit = 50;
    constexpr int MaxAvatarBytes = 128 * 1024;
    constexpr int MaxAvatarDownloads = 4;

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

    QString safeFilename(QString name)
    {
        name.replace(u'\\', u'/');
        name = name.section(u'/', -1);
        name.removeIf([](QChar character) {
            return character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Format;
        });
        return name.left(255).trimmed();
    }

    QString displayContent(const QJsonObject &content)
    {
        const auto type = typeOf(content);
        if (type == u"messageText")
            return content.value(u"text"_s).toObject().value(u"text"_s).toString();
        QString label;
        QString field;
        if (type == u"messagePhoto") {
            label = TelegramProvider::tr("Photo");
        } else if (type == u"messageVideo") {
            label = TelegramProvider::tr("Video");
            field = u"video"_s;
        } else if (type == u"messageAudio") {
            label = TelegramProvider::tr("Audio");
            field = u"audio"_s;
        } else if (type == u"messageDocument") {
            label = TelegramProvider::tr("File");
            field = u"document"_s;
        } else if (type == u"messageVoiceNote") {
            label = TelegramProvider::tr("Voice message");
        } else if (type == u"messageVideoNote") {
            label = TelegramProvider::tr("Video message");
        } else if (type == u"messageAnimation") {
            label = TelegramProvider::tr("Animation");
            field = u"animation"_s;
        } else if (type == u"messageSticker") {
            label = TelegramProvider::tr("Sticker");
        } else {
            return {};
        }
        const auto filename = safeFilename(content.value(field).toObject().value(u"file_name"_s).toString());
        if (!filename.isEmpty())
            label += u" · "_s + filename;
        const auto caption = content.value(u"caption"_s).toObject().value(u"text"_s).toString();
        if (!caption.isEmpty())
            label += u'\n' + caption;
        return label;
    }

    QString errorCode(const QJsonObject &response)
    {
        const auto code = response.value(u"code"_s).toInt();
        return code == 0 ? QStringLiteral("unknown") : QString::number(code);
    }

    int smallAvatarId(const QJsonObject &photo)
    {
        const auto file = photo.value(u"small"_s).toObject();
        const auto local = file.value(u"local"_s).toObject();
        const int id = file.value(u"id"_s).toInt();
        const bool available = local.value(u"can_be_downloaded"_s).toBool()
            || local.value(u"is_downloading_completed"_s).toBool();
        return id > 0 && available && file.value(u"size"_s).toInteger() <= MaxAvatarBytes
                && file.value(u"expected_size"_s).toInteger() <= MaxAvatarBytes
            ? id
            : 0;
    }

    QByteArray readAvatar(const QJsonObject &file, const QString &profile)
    {
        const auto local = file.value(u"local"_s).toObject();
        if (!local.value(u"is_downloading_completed"_s).toBool())
            return {};
        const auto root = QFileInfo(profile).canonicalFilePath();
        const QFileInfo photo(local.value(u"path"_s).toString());
        const auto path = photo.canonicalFilePath();
        if (root.isEmpty() || path.isEmpty() || !photo.isAbsolute() || !photo.isFile() || photo.size() <= 0
            || photo.size() > MaxAvatarBytes)
            return {};
        const auto relative = QDir(root).relativeFilePath(path);
        if (QDir::isAbsolutePath(relative) || relative == u".." || relative.startsWith(u"../"))
            return {};
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly))
            return {};
        const auto jpeg = input.read(MaxAvatarBytes + 1);
        if (jpeg.size() > MaxAvatarBytes || !jpeg.startsWith(QByteArrayView("\xff\xd8\xff", 3)))
            return {};
        return jpeg;
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
    reset();
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
    return Capability::Text | Capability::DirectMessages | Capability::Groups | Capability::RemoteHistory
        | Capability::PaginatedHistory | Capability::ReadReceipts | Capability::Files | Capability::Presence;
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

    auto decoded = decodeTelegramCredentials(secret);
    // Direct temporary provider clients may pass their own API hash. Persistent
    // profiles must receive a keychain-backed envelope from AccountManager.
    if (!decoded && !account_.saveSecret && validTelegramApiHash(secret))
        decoded = newTelegramCredentials(secret);
    if (!decoded) {
        fail(tr("The saved Telegram session credential is invalid. Check account settings."));
        return;
    }
    credentials_ = *decoded;
    bool apiIdValid = false;
    apiId_ = account_.providerId.isEmpty() ? bundledTelegramApiId() : account_.providerId.toInt(&apiIdValid);
    apiHash_ = account_.providerId.isEmpty() ? bundledTelegramApiHash() : credentials_.apiHash;
    if (apiId_ <= 0 || (!account_.providerId.isEmpty() && !apiIdValid) || !validTelegramApiHash(apiHash_)) {
        fail(tr("This build needs a Telegram application ID and hash in advanced account settings."));
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
    if (chat != chats_.cend() && (chat->protectedContent || chat->autoDeletes)) {
        emit notice(tr("This Telegram chat uses protected or auto-deleting messages. Open it in an "
                       "official Telegram client; PhosChat does not yet support these messages."));
        return false;
    }
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

bool TelegramProvider::canSendFiles(const ConversationId &id) const
{
    const auto chat = chats_.constFind(chatId(id));
    if (state_ != ConnectionState::Connected || chat == chats_.cend() || chat->protectedContent
        || chat->autoDeletes)
        return false;
    for (const auto &permission :
        { u"can_send_documents"_s, u"can_send_photos"_s, u"can_send_videos"_s, u"can_send_audios"_s }) {
        if (chat->permissions.value(permission).toBool(true))
            return true;
    }
    return false;
}

bool TelegramProvider::sendFile(const ConversationId &id, const QString &path)
{
    const auto remoteId = chatId(id);
    const auto chat = chats_.constFind(remoteId);
    if (!canSendFiles(id)) {
        emit notice(tr("This Telegram conversation is unavailable for sending attachments."));
        return false;
    }
    const QFileInfo file(path);
    const auto canonicalPath = file.canonicalFilePath();
    if (!file.isAbsolute() || !file.isFile() || !file.isReadable() || file.size() <= 0
        || canonicalPath.isEmpty()) {
        emit notice(tr("The attachment could not be opened."));
        return false;
    }

    const auto mime = QMimeDatabase().mimeTypeForFile(canonicalPath, QMimeDatabase::MatchContent).name();
    auto type = u"inputMessageDocument"_s;
    auto field = u"document"_s;
    auto permission = u"can_send_documents"_s;
    if ((mime == u"image/jpeg" || mime == u"image/png") && file.size() <= 10 * 1024 * 1024) {
        type = u"inputMessagePhoto"_s;
        field = u"photo"_s;
        permission = u"can_send_photos"_s;
    } else if (mime == u"video/mp4") {
        type = u"inputMessageVideo"_s;
        field = u"video"_s;
        permission = u"can_send_videos"_s;
    } else if (mime == u"audio/mpeg" || mime == u"audio/mp4" || mime == u"audio/x-m4a") {
        type = u"inputMessageAudio"_s;
        field = u"audio"_s;
        permission = u"can_send_audios"_s;
    }
    if (!chat->permissions.value(permission).toBool(true)) {
        emit notice(tr("This Telegram chat does not allow that type of attachment."));
        return false;
    }

    const QJsonObject input { { u"@type"_s, u"inputFileLocal"_s }, { u"path"_s, canonicalPath } };
    QJsonObject content { { u"@type"_s, type }, { field, input } };
    if (field == u"document")
        content.insert(u"disable_content_type_detection"_s, true);
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

bool TelegramProvider::requestProfile(const ConversationId &id)
{
    const auto remoteId = chatId(id);
    const auto chat = chats_.constFind(remoteId);
    if (state_ != ConnectionState::Connected || chat == chats_.cend() || chat->peer <= 0)
        return false;
    if (pendingProfiles_.contains(remoteId))
        return true;
    pendingProfiles_.insert(remoteId, {});
    if (chat->kind == ConversationKind::Direct) {
        request({ { u"@type"_s, u"getUser"_s }, { u"user_id"_s, chat->peer } }, u"profile-user"_s, remoteId);
    } else if (chat->supergroup) {
        request({ { u"@type"_s, u"getSupergroupFullInfo"_s }, { u"supergroup_id"_s, chat->peer } },
            u"profile-details"_s, remoteId);
    } else {
        request({ { u"@type"_s, u"getBasicGroupFullInfo"_s }, { u"basic_group_id"_s, chat->peer } },
            u"profile-details"_s, remoteId);
    }
    return true;
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
    if (!remoteId || histories_.value(remoteId).initialFinished)
        return;
    beginHistory(remoteId);
}

void TelegramProvider::requestOlderHistory(const ConversationId &id)
{
    if (const auto remoteId = chatId(id))
        beginHistory(remoteId);
}

HistoryState TelegramProvider::historyState(const ConversationId &id) const
{
    const auto remoteId = chatId(id);
    const auto chat = chats_.constFind(remoteId);
    if (!remoteId || state_ != ConnectionState::Connected
        || (chat != chats_.cend() && (chat->protectedContent || chat->autoDeletes)))
        return HistoryState::Unavailable;
    const auto history = histories_.value(remoteId);
    if (history.complete)
        return HistoryState::Complete;
    return history.active ? HistoryState::Loading : HistoryState::Ready;
}

void TelegramProvider::setConversationOpen(const ConversationId &id, bool open)
{
    if (id.accountId != account_.id || id.remoteId.isEmpty())
        return;
    if (open)
        openConversations_.insert(id.remoteId);
    else
        openConversations_.remove(id.remoteId);
    if (const auto remoteId = chatId(id))
        syncOpenChat(remoteId);
}

void TelegramProvider::beginHistory(qint64 id)
{
    const auto chat = chats_.constFind(id);
    if (chat != chats_.cend() && (chat->protectedContent || chat->autoDeletes)) {
        emit notice(tr("This Telegram chat uses protected or auto-deleting messages. Open it in an "
                       "official Telegram client; PhosChat does not yet support these messages."));
        return;
    }
    auto &history = histories_[id];
    if (history.active || history.complete)
        return;
    history.active = true;
    history.remaining = HistoryLimit;
    notifyHistory(id);
    fetchHistory(id);
}

void TelegramProvider::notifyHistory(qint64 id)
{
    if (owner_)
        emit historyStateChanged({ account_.id, QString::number(owner_) + u'/' + QString::number(id) });
}

void TelegramProvider::syncOpenChat(qint64 id)
{
    const auto chat = chats_.constFind(id);
    const bool wanted = owner_
        && openConversations_.contains(QString::number(owner_) + u'/' + QString::number(id))
        && chat != chats_.cend() && !chat->protectedContent && !chat->autoDeletes;
    if (!wanted && openedChats_.remove(id)) {
        clearPeerPresence(id);
        request({ { u"@type"_s, u"closeChat"_s }, { u"chat_id"_s, id } }, u"close-chat"_s, id);
    } else if (wanted && state_ == ConnectionState::Connected && !openedChats_.contains(id)) {
        openedChats_.insert(id);
        requestPeerPresence(id);
        request({ { u"@type"_s, u"openChat"_s }, { u"chat_id"_s, id } }, u"open-chat"_s, id);
    }
}

void TelegramProvider::refreshChatHistoryAccess(qint64 id)
{
    syncOpenChat(id);
    const auto chat = chats_.constFind(id);
    if (chat != chats_.cend() && (chat->protectedContent || chat->autoDeletes)) {
        // Invalidate in-flight history and remove already displayed messages.
        histories_.remove(id);
        readPending_.remove(id);
        if (owner_)
            emit historyCleared({ account_.id, QString::number(owner_) + u'/' + QString::number(id) });
    }
    notifyHistory(id);
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
    apiId_ = 0;
    credentials_.apiHash.fill('\0');
    credentials_.databaseKey.fill('\0');
    credentials_.legacyDatabaseKey.fill('\0');
    credentials_ = {};
    pendingAuthorization_ = {};
    legacyKeyInUse_ = false;
    parametersPending_ = false;
    parametersConfigured_ = false;
    keyRotationPending_ = false;
    owner_ = 0;
    challenge_.reset();
    challengeDetail_.clear();
    authenticationPending_ = false;
    authorized_ = false;
    networkReady_ = false;
    chatLists_ = {};
    chats_.clear();
    avatars_.clear();
    avatarQueue_.clear();
    pendingAvatars_ = 0;
    pendingProfiles_.clear();
    users_.clear();
    pendingPeerPresence_.clear();
    onlinePeerExpiries_.clear();
    histories_.clear();
    openedChats_.clear();
    settledTemporaryMessages_.clear();
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

void TelegramProvider::request(QJsonObject object, const QString &operation, qint64 chatId, quint64 token)
{
    if (client_ <= 0)
        return;
    object.insert(u"@extra"_s,
        QJsonObject { { u"operation"_s, operation }, { u"chat"_s, chatId },
            { u"token"_s, static_cast<qint64>(token) } });
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
    } else if (type == u"updateUserStatus") {
        updatePeerPresence(telegramId(event.value(u"user_id"_s)), event.value(u"status"_s).toObject());
    } else if (type == u"updateNewChat") {
        acceptChat(event.value(u"chat"_s).toObject());
    } else if (type == u"updateNewMessage") {
        acceptMessage(event.value(u"message"_s).toObject(), false);
    } else if (type == u"updateMessageSendSucceeded" || type == u"updateMessageSendFailed") {
        acceptSendResult(event, type == u"updateMessageSendFailed");
    } else if (type == u"updateChatReadOutbox") {
        updateReadOutbox(
            telegramId(event.value(u"chat_id"_s)), telegramId(event.value(u"last_read_outbox_message_id"_s)));
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
        if (type == u"updateChatPhoto") {
            const auto fileId = smallAvatarId(event.value(u"photo"_s).toObject());
            if (chat->avatarFile != fileId) {
                setAvatar(id, *chat, fileId);
                announce(id, *chat);
                loadAvatars();
            }
            return;
        }
        if (type == u"updateChatTitle") {
            chat->title = event.value(u"title"_s).toString();
        } else if (type == u"updateChatPermissions") {
            chat->permissions = event.value(u"permissions"_s).toObject();
            chat->canSend = chat->permissions.value(u"can_send_basic_messages"_s).toBool();
        } else if (type == u"updateChatHasProtectedContent") {
            chat->protectedContent = event.value(u"has_protected_content"_s).toBool();
        } else if (type == u"updateChatMessageAutoDeleteTime") {
            chat->autoDeletes = event.value(u"message_auto_delete_time"_s).toInt() > 0;
        } else {
            return;
        }
        announce(id, *chat);
        refreshChatHistoryAccess(id);
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
        parametersPending_ = false;
        if (error && event.value(u"code"_s).toInt() == 401 && !credentials_.legacyDatabaseKey.isEmpty()
            && !legacyKeyInUse_) {
            legacyKeyInUse_ = true;
            sendParameters();
        } else if (error) {
            fail(tr("Telegram could not open the session (error %1). Check the API credentials.")
                    .arg(errorCode(event)));
        } else if (legacyKeyInUse_) {
            parametersConfigured_ = true;
            rotateSessionKeyIfReady();
        } else {
            parametersConfigured_ = true;
            apiHash_.fill('\0');
            apiHash_.clear();
            if (!credentials_.legacyDatabaseKey.isEmpty())
                finishKeyMigration();
        }
    } else if (operation == u"database-key") {
        if (error)
            fail(tr("Could not upgrade Telegram session protection. Reconnect to retry (error %1).")
                    .arg(errorCode(event)));
        else
            finishKeyMigration();
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
        updateConnectionState();
    } else if (operation == u"chats-main" || operation == u"chats-archive") {
        const bool archived = operation == u"chats-archive";
        auto &list = chatLists_[archived ? 1 : 0];
        list.pending = false;
        if (!error) {
            loadMoreChats(archived);
        } else if (event.value(u"code"_s).toInt() == 404) {
            list.complete = true;
        } else {
            emit notice(tr("Telegram could not finish loading chats (error %1).").arg(errorCode(event)));
        }
    } else if (operation == u"chat") {
        chatPending_.remove(id);
        if (!error)
            acceptChat(event);
        if (!error && chats_.contains(id)) {
            fetchHistory(id);
        } else {
            histories_[id].active = false;
            notifyHistory(id);
            emit notice(tr("Telegram could not open this conversation's history."));
        }
    } else if (operation == u"history") {
        auto history = histories_.find(id);
        if (history == histories_.end() || !history->pending
            || tag.value(u"token"_s).toInteger() != static_cast<qint64>(history->requestToken))
            return;
        history->pending = false;
        if (error) {
            history->active = false;
            notifyHistory(id);
            emit notice(tr("Telegram could not load messages (error %1). Try loading earlier messages again.")
                    .arg(errorCode(event)));
            return;
        }
        const auto messages = event.value(u"messages"_s).toArray();
        if (messages.isEmpty()) {
            history->complete = true;
            history->active = false;
            history->initialFinished = true;
            notifyHistory(id);
            return;
        }
        // Advance using raw messages, including media the preview cannot display.
        // Repeated page boundaries are excluded before emitting historical text.
        qint64 oldest = history->cursor;
        QHash<qint64, QJsonObject> page;
        for (const auto &value : messages) {
            const auto message = value.toObject();
            const auto messageId = telegramId(message.value(u"id"_s));
            if (telegramId(message.value(u"chat_id"_s)) != id || messageId <= 0
                || (history->cursor && messageId >= history->cursor))
                continue;
            page.insert(messageId, message);
            if (!oldest || messageId < oldest)
                oldest = messageId;
        }
        if (page.isEmpty()) {
            history->active = false;
            notifyHistory(id);
            emit notice(tr(
                "Telegram returned no earlier messages in this page. Try loading earlier messages again."));
            return;
        }
        history->cursor = oldest;
        history->remaining -= static_cast<int>(page.size());
        const bool more = history->remaining > 0;
        if (!more) {
            history->active = false;
            history->initialFinished = true;
        }
        const auto token = history->requestToken;
        auto ids = page.keys();
        std::sort(ids.begin(), ids.end());
        for (const auto messageId : ids)
            acceptMessage(page.value(messageId), true);
        sendRead(id);
        notifyHistory(id);
        if (more) {
            QTimer::singleShot(0, this, [this, id, token] {
                const auto current = histories_.constFind(id);
                if (current != histories_.cend() && current->active && current->requestToken == token)
                    fetchHistory(id);
            });
        }
    } else if (operation == u"open-chat" && error) {
        openedChats_.remove(id);
        clearPeerPresence(id);
        emit notice(tr("Telegram could not open chat updates (error %1).").arg(errorCode(event)));
    } else if (operation == u"avatar") {
        finishAvatar(static_cast<int>(id), type == u"file" ? event : QJsonObject {});
    } else if (operation == u"presence-user") {
        const auto chat = chats_.constFind(id);
        if (pendingPeerPresence_.remove(id) && type == u"user" && chat != chats_.cend()
            && telegramId(event.value(u"id"_s)) == chat->peer)
            updateUser(event);
    } else if (operation == u"profile-user") {
        const auto chat = chats_.constFind(id);
        auto details = pendingProfiles_.find(id);
        if (chat == chats_.cend() || details == pendingProfiles_.end())
            return;
        if (type == u"user" && telegramId(event.value(u"id"_s)) == chat->peer) {
            updateUser(event);
            const auto usernames
                = event.value(u"usernames"_s).toObject().value(u"active_usernames"_s).toArray();
            const auto username = usernames.isEmpty() ? QString {} : usernames.first().toString().left(64);
            if (!username.isEmpty())
                details->append({ tr("Username"), u'@' + username });
        }
        request(
            { { u"@type"_s, u"getUserFullInfo"_s }, { u"user_id"_s, chat->peer } }, u"profile-details"_s, id);
    } else if (operation == u"profile-details") {
        finishProfile(id, event);
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
    // TDLib permits rekeying only after authorization. Existing profiles may
    // require phone/code/password again; allow those steps, then protect the
    // session before publishing any conversations or messages.
    if (!credentials_.legacyDatabaseKey.isEmpty() && type == u"authorizationStateReady") {
        pendingAuthorization_ = state;
        rotateSessionKeyIfReady();
        return;
    }
    if (type == u"authorizationStateWaitTdlibParameters") {
        if (!apiHash_.isEmpty() && !parametersPending_ && !parametersConfigured_)
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
    parametersPending_ = true;
    const auto &encryptionKey = legacyKeyInUse_ ? credentials_.legacyDatabaseKey : credentials_.databaseKey;
    const auto version = QCoreApplication::applicationVersion();
    request({ { u"@type"_s, u"setTdlibParameters"_s }, { u"use_test_dc"_s, false },
                { u"database_directory"_s, profile_ }, { u"files_directory"_s, profile_ },
                { u"database_encryption_key"_s, QString::fromLatin1(encryptionKey.toBase64()) },
                { u"use_file_database"_s, false }, { u"use_chat_info_database"_s, false },
                { u"use_message_database"_s, false }, { u"use_secret_chats"_s, false },
                { u"api_id"_s, apiId_ }, { u"api_hash"_s, QString::fromLatin1(apiHash_) },
                { u"system_language_code"_s, u"en"_s }, { u"device_model"_s, u"PhosChat Desktop"_s },
                { u"system_version"_s, QSysInfo::prettyProductName() },
                { u"application_version"_s, version.isEmpty() ? u"0.2.0"_s : version } },
        u"parameters"_s);
}

void TelegramProvider::finishKeyMigration()
{
    credentials_.legacyDatabaseKey.fill('\0');
    credentials_.legacyDatabaseKey.clear();
    legacyKeyInUse_ = false;
    apiHash_.fill('\0');
    apiHash_.clear();
    emit credentialsUpdated(encodeTelegramCredentials(credentials_));
    const auto pending = std::exchange(pendingAuthorization_, {});
    if (!pending.isEmpty())
        processAuthorization(pending);
}

void TelegramProvider::rotateSessionKeyIfReady()
{
    if (!parametersConfigured_ || !legacyKeyInUse_ || keyRotationPending_
        || typeOf(pendingAuthorization_) != u"authorizationStateReady")
        return;
    keyRotationPending_ = true;
    request({ { u"@type"_s, u"setDatabaseEncryptionKey"_s },
                { u"new_encryption_key"_s, QString::fromLatin1(credentials_.databaseKey.toBase64()) } },
        u"database-key"_s);
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
    const bool becameConnected = networkReady_ && state_ != ConnectionState::Connected;
    if (!networkReady_)
        onlinePeerExpiries_.clear();
    setState(networkReady_ ? ConnectionState::Connected : ConnectionState::Reconnecting);
    if (becameConnected) {
        // The core clears send availability while offline. Restore it for every
        // known chat, even when TDLib has no changed chat metadata to publish.
        for (auto chat = chats_.cbegin(); chat != chats_.cend(); ++chat)
            announce(chat.key(), *chat);
        loadMoreChats(false);
        loadMoreChats(true);
        loadAvatars();
        for (const auto id : chats_.keys()) {
            if (openedChats_.contains(id))
                requestPeerPresence(id);
            syncOpenChat(id);
        }
        for (const auto id : histories_.keys()) {
            notifyHistory(id);
            fetchHistory(id);
        }
    }
}

void TelegramProvider::updateUser(const QJsonObject &user)
{
    const auto id = telegramId(user.value(u"id"_s));
    const auto name
        = (user.value(u"first_name"_s).toString() + u' ' + user.value(u"last_name"_s).toString()).trimmed();
    if (id > 0 && !name.isEmpty())
        users_.insert(id, name);
    if (user.contains(u"status"_s))
        updatePeerPresence(id, user.value(u"status"_s).toObject());
}

void TelegramProvider::requestPeerPresence(qint64 id)
{
    const auto chat = chats_.constFind(id);
    if (state_ != ConnectionState::Connected || chat == chats_.cend()
        || chat->kind != ConversationKind::Direct || chat->peer <= 0 || chat->peer == owner_
        || pendingPeerPresence_.contains(id))
        return;
    pendingPeerPresence_.insert(id);
    request({ { u"@type"_s, u"getUser"_s }, { u"user_id"_s, chat->peer } }, u"presence-user"_s, id);
}

void TelegramProvider::updatePeerPresence(qint64 userId, const QJsonObject &status)
{
    if (state_ != ConnectionState::Connected || userId <= 0 || userId == owner_)
        return;

    auto presence = Presence::Unknown;
    const auto type = typeOf(status);
    const auto expiry = qint64(status.value(u"expires"_s).toInt()) * 1000;
    const auto delay = expiry - QDateTime::currentMSecsSinceEpoch();
    if (type == u"userStatusOnline" && delay > 0 && delay <= std::numeric_limits<int>::max())
        presence = Presence::Online;
    else if (type == u"userStatusOffline" && status.value(u"was_online"_s).toInt() > 0)
        presence = Presence::Offline;

    for (const auto id : std::as_const(openedChats_)) {
        const auto chat = chats_.constFind(id);
        if (chat == chats_.cend() || chat->kind != ConversationKind::Direct || chat->peer != userId)
            continue;
        const ConversationId conversation { account_.id,
            QString::number(owner_) + u'/' + QString::number(id) };
        if (presence != Presence::Online) {
            onlinePeerExpiries_.remove(id);
        } else if (onlinePeerExpiries_.value(id) != expiry) {
            onlinePeerExpiries_.insert(id, expiry);
            QTimer::singleShot(
                static_cast<int>(delay), Qt::PreciseTimer, this, [this, id, expiry, conversation] {
                    if (onlinePeerExpiries_.value(id) == expiry) {
                        onlinePeerExpiries_.remove(id);
                        emit presenceChanged(conversation, Presence::Unknown);
                    }
                });
        }
        emit presenceChanged(conversation, presence);
    }
}

void TelegramProvider::clearPeerPresence(qint64 id)
{
    onlinePeerExpiries_.remove(id);
    const auto chat = chats_.constFind(id);
    if (owner_ && chat != chats_.cend() && chat->kind == ConversationKind::Direct && chat->peer > 0
        && chat->peer != owner_)
        emit presenceChanged(
            { account_.id, QString::number(owner_) + u'/' + QString::number(id) }, Presence::Unknown);
}

void TelegramProvider::loadMoreChats(bool archived)
{
    auto &list = chatLists_[archived ? 1 : 0];
    if (!owner_ || state_ != ConnectionState::Connected || list.pending || list.complete)
        return;
    list.pending = true;
    // TDLib may return fewer chats than requested. Only its 404 result marks
    // the end of each list; an arbitrary page cap can hide existing groups.
    request({ { u"@type"_s, u"loadChats"_s },
                { u"chat_list"_s,
                    QJsonObject { { u"@type"_s, archived ? u"chatListArchive"_s : u"chatListMain"_s } } },
                { u"limit"_s, ChatPageSize } },
        archived ? u"chats-archive"_s : u"chats-main"_s);
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
    const auto title = object.value(u"title"_s).toString();
    const auto conversationKind
        = type == u"chatTypePrivate" ? ConversationKind::Direct : ConversationKind::Group;
    const bool supergroup = type == u"chatTypeSupergroup";
    const auto peerField = conversationKind == ConversationKind::Direct ? u"user_id"_s
        : supergroup                                                    ? u"supergroup_id"_s
                                                                        : u"basic_group_id"_s;
    const auto peer = telegramId(kind.value(peerField));
    const auto permissions = object.value(u"permissions"_s).toObject();
    const bool canSend = permissions.value(u"can_send_basic_messages"_s).toBool(true);
    const bool protectedContent = object.value(u"has_protected_content"_s).toBool();
    const bool autoDeletes = object.value(u"message_auto_delete_time"_s).toInt() > 0;
    const auto avatarFile = smallAvatarId(object.value(u"photo"_s).toObject());
    const auto readOutbox = telegramId(object.value(u"last_read_outbox_message_id"_s));
    updateReadOutbox(id, readOutbox);
    const auto known = chats_.constFind(id);
    if (known != chats_.cend() && known->title == title && known->kind == conversationKind
        && known->canSend == canSend && known->protectedContent == protectedContent
        && known->autoDeletes == autoDeletes && known->avatarFile == avatarFile && known->peer == peer
        && known->supergroup == supergroup && known->permissions == permissions)
        return;
    auto &chat = chats_[id];
    chat.title = title;
    chat.kind = conversationKind;
    chat.peer = peer;
    chat.supergroup = supergroup;
    chat.permissions = permissions;
    updateReadOutbox(id, readOutbox);
    chat.canSend = canSend;
    chat.protectedContent = protectedContent;
    chat.autoDeletes = autoDeletes;
    setAvatar(id, chat, avatarFile);
    announce(id, chat);
    loadAvatars();
    refreshChatHistoryAccess(id);
}

void TelegramProvider::announce(qint64 chatId, const Chat &chat)
{
    if (!owner_)
        return;
    Conversation conversation;
    conversation.id = { account_.id, QString::number(owner_) + u'/' + QString::number(chatId) };
    conversation.title = chat.title.isEmpty() ? tr("Telegram conversation") : chat.title;
    conversation.kind = chat.kind;
    conversation.joined
        = state_ == ConnectionState::Connected && chat.canSend && !chat.protectedContent && !chat.autoDeletes;
    if (const auto avatar = avatars_.constFind(chat.avatarFile); avatar != avatars_.cend())
        conversation.avatar = avatar->data;
    emit conversationChanged(conversation);
}

void TelegramProvider::setAvatar(qint64 chatId, Chat &chat, int fileId)
{
    if (chat.avatarFile == fileId)
        return;
    if (auto previous = avatars_.find(chat.avatarFile); previous != avatars_.end()) {
        previous->chats.remove(chatId);
        if (previous->chats.isEmpty() && !previous->pending) {
            avatarQueue_.removeAll(chat.avatarFile);
            avatars_.erase(previous);
        }
    }
    chat.avatarFile = fileId;
    if (fileId <= 0)
        return;
    if (!avatars_.contains(fileId))
        avatarQueue_.enqueue(fileId);
    avatars_[fileId].chats.insert(chatId);
}

void TelegramProvider::loadAvatars()
{
    if (!owner_ || state_ != ConnectionState::Connected)
        return;
    while (pendingAvatars_ < MaxAvatarDownloads && !avatarQueue_.isEmpty()) {
        const int fileId = avatarQueue_.dequeue();
        avatars_[fileId].pending = true;
        ++pendingAvatars_;
        // The TDLib result waits for the download, while td_send remains asynchronous.
        request({ { u"@type"_s, u"downloadFile"_s }, { u"file_id"_s, fileId }, { u"priority"_s, 1 },
                    { u"offset"_s, 0 }, { u"limit"_s, MaxAvatarBytes + 1 }, { u"synchronous"_s, true } },
            u"avatar"_s, fileId);
    }
}

void TelegramProvider::finishAvatar(int fileId, const QJsonObject &file)
{
    auto avatar = avatars_.find(fileId);
    if (avatar == avatars_.end() || !avatar->pending)
        return;
    avatar->pending = false;
    --pendingAvatars_;
    if (avatar->chats.isEmpty()) {
        avatars_.erase(avatar);
    } else if (file.value(u"id"_s).toInt() == fileId) {
        avatar->data = readAvatar(file, profile_);
        if (!avatar->data.isEmpty()) {
            const auto chats = avatar->chats;
            for (const auto id : chats) {
                const auto chat = chats_.constFind(id);
                if (chat != chats_.cend() && chat->avatarFile == fileId)
                    announce(id, *chat);
            }
        }
    }
    loadAvatars();
}

void TelegramProvider::finishProfile(qint64 id, const QJsonObject &event)
{
    if (!pendingProfiles_.contains(id))
        return;
    auto details = pendingProfiles_.take(id);
    const auto type = typeOf(event);
    if (type == u"userFullInfo") {
        const auto bio = event.value(u"bio"_s).toObject().value(u"text"_s).toString().trimmed().left(1024);
        if (!bio.isEmpty())
            details.append({ tr("Bio"), bio });
    } else if (type == u"basicGroupFullInfo" || type == u"supergroupFullInfo") {
        const auto description = event.value(u"description"_s).toString().trimmed().left(1024);
        if (!description.isEmpty())
            details.append({ tr("Description"), description });
        const auto count = type == u"supergroupFullInfo" ? event.value(u"member_count"_s).toInt()
                                                         : event.value(u"members"_s).toArray().size();
        if (count > 0)
            details.append({ tr("Members"), QString::number(count) });
    }
    emit profileReceived({ account_.id, QString::number(owner_) + u'/' + QString::number(id) }, details);
}

void TelegramProvider::acceptSendResult(const QJsonObject &event, bool failed)
{
    auto message = event.value(u"message"_s).toObject();
    const auto remote = telegramId(message.value(u"chat_id"_s));
    const auto oldId = telegramId(event.value(u"old_message_id"_s));
    const auto newId = telegramId(message.value(u"id"_s));
    if (owner_ && remote && chats_.contains(remote) && oldId > 0) {
        settledTemporaryMessages_.insert(QString::number(remote) + u'/' + QString::number(oldId));
        if (oldId != newId)
            emit messageRemoved({ account_.id, QString::number(owner_) + u'/' + QString::number(remote) },
                QString::number(oldId));
    }
    if (failed)
        message.insert(u"sending_state"_s, QJsonObject { { u"@type"_s, u"messageSendingStateFailed"_s } });
    else
        message.remove(u"sending_state"_s);
    acceptMessage(message, false);
    if (failed)
        emit notice(tr("Telegram could not send the message (error %1).")
                .arg(errorCode(event.value(u"error"_s).toObject())));
}

void TelegramProvider::updateReadOutbox(qint64 id, qint64 sequence)
{
    auto chat = chats_.find(id);
    if (chat == chats_.end() || chat->kind != ConversationKind::Direct || sequence <= chat->readOutbox)
        return;
    chat->readOutbox = sequence;
    if (owner_)
        emit messagesReadThrough(
            { account_.id, QString::number(owner_) + u'/' + QString::number(id) }, sequence);
}

void TelegramProvider::acceptMessage(const QJsonObject &message, bool historical)
{
    const auto id = telegramId(message.value(u"id"_s));
    const auto remote = telegramId(message.value(u"chat_id"_s));
    auto chat = chats_.find(remote);
    const bool outgoing = message.value(u"is_outgoing"_s).toBool();
    const auto sending = typeOf(message.value(u"sending_state"_s).toObject());
    if (!owner_ || id <= 0 || chat == chats_.end() || chat->protectedContent || chat->autoDeletes
        || message.value(u"self_destruct_type"_s).isObject()
        || message.value(u"scheduling_state"_s).isObject()
        || message.value(u"auto_delete_in"_s).toDouble() > 0
        || message.value(u"self_destruct_in"_s).toDouble() > 0
        || !message.value(u"can_be_saved"_s).toBool(true) || (!sending.isEmpty() && !outgoing)
        || (sending == u"messageSendingStatePending"
            && settledTemporaryMessages_.contains(QString::number(remote) + u'/' + QString::number(id))))
        return;
    const auto text = displayContent(message.value(u"content"_s).toObject());
    if (text.isEmpty())
        return;

    const auto sender = message.value(u"sender_id"_s).toObject();
    const auto userId = telegramId(sender.value(u"user_id"_s));
    QString senderName = users_.value(userId);
    if (senderName.isEmpty()) {
        if (outgoing)
            senderName = tr("You");
        else if (chat->kind == ConversationKind::Direct && !chat->title.isEmpty())
            senderName = chat->title;
        else
            senderName = tr("Telegram participant");
    }
    if (sending.isEmpty())
        chat->latest = qMax(chat->latest, id);

    Message value;
    value.conversation = { account_.id, QString::number(owner_) + u'/' + QString::number(remote) };
    value.id = QString::number(id);
    value.sequence = id;
    value.sender = senderName;
    value.text = text;
    value.timestamp = QDateTime::fromSecsSinceEpoch(message.value(u"date"_s).toInteger(), QTimeZone::UTC);
    value.outgoing = outgoing;
    value.mention = message.value(u"contains_unread_mention"_s).toBool();
    value.historical = historical;
    if (outgoing) {
        if (sending == u"messageSendingStatePending")
            value.delivery = DeliveryState::Pending;
        else if (sending == u"messageSendingStateFailed")
            value.delivery = DeliveryState::Failed;
        else if (sending.isEmpty())
            value.delivery = chat->kind == ConversationKind::Direct && chat->readOutbox >= id
                ? DeliveryState::Read
                : DeliveryState::Sent;
    }
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
    auto history = histories_.find(id);
    if (history == histories_.end() || !history->active || history->pending
        || state_ != ConnectionState::Connected)
        return;
    const auto chat = chats_.constFind(id);
    if (chat == chats_.cend()) {
        if (!chatPending_.contains(id)) {
            chatPending_.insert(id);
            request({ { u"@type"_s, u"getChat"_s }, { u"chat_id"_s, id } }, u"chat"_s, id);
        }
        return;
    }
    if (chat->protectedContent || chat->autoDeletes) {
        histories_.remove(id);
        notifyHistory(id);
        return;
    }
    history->pending = true;
    history->requestToken = ++historyToken_;
    request({ { u"@type"_s, u"getChatHistory"_s }, { u"chat_id"_s, id },
                { u"from_message_id"_s, history->cursor }, { u"offset"_s, 0 },
                { u"limit"_s, history->remaining + (history->cursor ? 1 : 0) }, { u"only_local"_s, false } },
        u"history"_s, id, history->requestToken);
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
