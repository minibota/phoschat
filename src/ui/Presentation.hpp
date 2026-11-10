#pragma once

#include "models/types.h"

#include <QCoreApplication>

namespace phoschat::ui {

inline QString connectionLabel(ConnectionState state)
{
    switch (state) {
    case ConnectionState::Disconnected:
        return QCoreApplication::translate("ConnectionState", "Offline");
    case ConnectionState::Connecting:
        return QCoreApplication::translate("ConnectionState", "Connecting…");
    case ConnectionState::Authenticating:
        return QCoreApplication::translate("ConnectionState", "Authenticating…");
    case ConnectionState::Connected:
        return QCoreApplication::translate("ConnectionState", "Connected");
    case ConnectionState::Reconnecting:
        return QCoreApplication::translate("ConnectionState", "Reconnecting…");
    case ConnectionState::Error:
        return QCoreApplication::translate("ConnectionState", "Connection error");
    }
    return {};
}

inline QString protocolLabel(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Irc:
        return QStringLiteral("IRC");
    case Protocol::Telegram:
        return QStringLiteral("Telegram");
    case Protocol::Matrix:
        return QStringLiteral("Matrix");
    case Protocol::Slack:
        return QStringLiteral("Slack");
    }
    return {};
}

inline QString accountLabel(const Account &account)
{
    return account.label.isEmpty() ? account.host : account.label;
}

} // namespace phoschat::ui
