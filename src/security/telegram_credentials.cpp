#include "security/telegram_credentials.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

namespace phoschat {

bool validTelegramApiHash(const QByteArray &value)
{
    if (value.size() != 32)
        return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')
                || (character >= 'A' && character <= 'F')))
            return false;
    }
    return true;
}

TelegramCredentials newTelegramCredentials(const QByteArray &apiHash)
{
    QByteArray key(32, Qt::Uninitialized);
    // Use the OS generator directly. A shared API hash cannot protect a local session.
    for (auto &byte : key)
        byte = static_cast<char>(QRandomGenerator::system()->generate() & 0xff);
    return { apiHash.toLower(), key, {} };
}

TelegramCredentials migrateTelegramCredentials(const QByteArray &apiHash, const QString &accountId)
{
    auto credentials = newTelegramCredentials(apiHash);
    credentials.legacyDatabaseKey = QMessageAuthenticationCode::hash(
        QByteArrayLiteral("PhosChat TDLib profile v1\0") + accountId.toUtf8(), QByteArray::fromHex(apiHash),
        QCryptographicHash::Sha256);
    return credentials;
}

QByteArray encodeTelegramCredentials(const TelegramCredentials &credentials)
{
    using namespace Qt::StringLiterals;
    return QJsonDocument(
        QJsonObject { { u"phoschat_telegram"_s, 2 },
            { u"api_hash"_s, QString::fromLatin1(credentials.apiHash) },
            { u"database_key"_s, QString::fromLatin1(credentials.databaseKey.toBase64()) },
            { u"legacy_key"_s, QString::fromLatin1(credentials.legacyDatabaseKey.toBase64()) } })
        .toJson(QJsonDocument::Compact);
}

std::optional<TelegramCredentials> decodeTelegramCredentials(const QByteArray &value)
{
    using namespace Qt::StringLiterals;
    if (value.size() > 2048)
        return {};
    const auto document = QJsonDocument::fromJson(value);
    const auto object = document.object();
    if (!document.isObject() || object.value(u"phoschat_telegram"_s).toInt() != 2)
        return {};
    const auto decodeKey = [&object](const QString &field) {
        return QByteArray::fromBase64(
            object.value(field).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    };
    TelegramCredentials credentials { object.value(u"api_hash"_s).toString().toLatin1(),
        decodeKey(u"database_key"_s), decodeKey(u"legacy_key"_s) };
    if (credentials.databaseKey.size() != 32
        || (!credentials.apiHash.isEmpty() && !validTelegramApiHash(credentials.apiHash))
        || (!object.value(u"legacy_key"_s).toString().isEmpty()
            && credentials.legacyDatabaseKey.size() != 32))
        return {};
    return credentials;
}

} // namespace phoschat
