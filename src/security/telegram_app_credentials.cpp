#include "security/telegram_app_credentials.h"

#include "security/telegram_credentials.h"
#include "telegram_build_credentials.h"

namespace phoschat {

int bundledTelegramApiId() { return PHOSCHAT_TELEGRAM_API_ID; }
QByteArray bundledTelegramApiHash() { return QByteArrayLiteral(PHOSCHAT_TELEGRAM_API_HASH); }
bool bundledTelegramCredentialsAvailable()
{
    return bundledTelegramApiId() > 0 && validTelegramApiHash(bundledTelegramApiHash());
}

} // namespace phoschat
