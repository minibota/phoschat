#pragma once

#include <QString>
#include <QUrl>

namespace phoschat {

// Public OAuth application identity; saved accounts retain their original Client ID.
QString bundledSlackClientId();
QUrl bundledSlackOAuthOrigin();
bool bundledSlackApplicationAvailable();

} // namespace phoschat
