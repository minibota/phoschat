#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace phoschat {

struct TelegramCredentials {
    QByteArray apiHash; // Empty for the bundled application identity.
    QByteArray databaseKey;
    QByteArray legacyDatabaseKey; // Crash-safe fallback only while upgrading a v0.2.0 profile.
};

bool validTelegramApiHash(const QByteArray &value);
TelegramCredentials newTelegramCredentials(const QByteArray &apiHash = {});
TelegramCredentials migrateTelegramCredentials(const QByteArray &apiHash, const QString &accountId);
QByteArray encodeTelegramCredentials(const TelegramCredentials &credentials);
std::optional<TelegramCredentials> decodeTelegramCredentials(const QByteArray &value);

} // namespace phoschat
