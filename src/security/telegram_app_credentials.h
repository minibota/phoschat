#pragma once

#include <QByteArray>

namespace phoschat {

// Application identity, shared by official installations; never an encryption secret.
bool bundledTelegramCredentialsAvailable();
int bundledTelegramApiId();
QByteArray bundledTelegramApiHash();

} // namespace phoschat
