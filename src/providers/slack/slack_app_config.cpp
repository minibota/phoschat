#include "providers/slack/slack_app_config.h"

#include "slack_build_config.h"

namespace phoschat {

QString bundledSlackClientId() { return QStringLiteral(PHOSCHAT_SLACK_CLIENT_ID); }

QUrl bundledSlackOAuthOrigin() { return QUrl(QStringLiteral(PHOSCHAT_SLACK_OAUTH_ORIGIN)); }

bool bundledSlackApplicationAvailable() { return !bundledSlackClientId().isEmpty(); }

} // namespace phoschat
