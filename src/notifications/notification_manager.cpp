#include "notifications/notification_manager.h"

#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "ui/contacts/MainWindow.hpp"
#include "ui/windows/WindowManager.hpp"

#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QSettings>

namespace phoschat {

NotificationManager::NotificationManager(ui::MainWindow &main, ui::WindowManager &windows,
    ConversationManager &conversations, MessageManager &messages, const QIcon &icon)
    : main_(main)
    , windows_(windows)
    , conversations_(conversations)
    , tray_(icon)
    , icon_(icon)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    menu_ = new QMenu(&main_);
    menu_->addAction(tr("Show PhosChat"), &main_, [this] {
        main_.showNormal();
        main_.raise();
        main_.activateWindow();
    });
    auto *alerts = menu_->addAction(tr("Message notifications"));
    alerts->setCheckable(true);
    alerts->setChecked(QSettings().value(QStringLiteral("notifications/enabled"), true).toBool());
    connect(alerts, &QAction::toggled, this,
        [](bool enabled) { QSettings().setValue(QStringLiteral("notifications/enabled"), enabled); });
    menu_->addSeparator();
    menu_->addAction(tr("Quit"), &main_, &ui::MainWindow::quitRequested);
    tray_.setContextMenu(menu_);
    connect(&tray_, &QSystemTrayIcon::activated, &main_, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason != QSystemTrayIcon::Trigger && reason != QSystemTrayIcon::DoubleClick)
            return;
        main_.showNormal();
        main_.raise();
        main_.activateWindow();
    });
    connect(&tray_, &QSystemTrayIcon::messageClicked, this, [this] {
        if (lastNotification_ && conversations_.conversation(*lastNotification_))
            windows_.open(*lastNotification_);
    });
    connect(&messages, &MessageManager::messageAdded, this, &NotificationManager::notify);
    connect(&conversations_, &ConversationManager::conversationsChanged, this,
        &NotificationManager::refreshBadge);
    refreshBadge();
    tray_.show();
    main_.setTrayAvailable(true);
}

NotificationManager::~NotificationManager() = default;

void NotificationManager::notify(const Message &message)
{
    const auto value = conversations_.conversation(message.conversation);
    if (message.historical || message.outgoing || !value || value->muted
        || windows_.isActive(message.conversation) || !QSystemTrayIcon::supportsMessages()
        || !QSettings().value(QStringLiteral("notifications/enabled"), true).toBool())
        return;
    lastNotification_ = message.conversation;
    tray_.showMessage(tr("PhosChat"),
        message.mention ? tr("You have a new mention.") : tr("You have a new message."), icon_);
}

void NotificationManager::refreshBadge()
{
    qint64 unread = 0;
    for (const auto &value : conversations_.conversations())
        unread += value.unread;
    tray_.setToolTip(unread ? tr("PhosChat · %1 unread").arg(unread) : tr("PhosChat"));
    if (unread == 0) {
        tray_.setIcon(icon_);
        return;
    }
    // Paint a legible activity dot. The exact count is available in the tooltip and list.
    QPixmap image = icon_.pixmap(48, 48);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#18232c")), 2));
    painter.setBrush(QColor(QStringLiteral("#ffbe79")));
    painter.drawEllipse(QRectF(33, 2, 13, 13));
    painter.end();
    tray_.setIcon(QIcon(image));
}

} // namespace phoschat
