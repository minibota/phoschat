#include "notifications/notification_manager.h"

#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "ui/contacts/MainWindow.hpp"
#include "ui/windows/WindowManager.hpp"
#ifdef Q_OS_LINUX
#include "notifications/linux_notifier.h"
#endif

#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QSignalBlocker>
#include <QUuid>

namespace phoschat {

NotificationManager::NotificationManager(ui::MainWindow &main, ui::WindowManager &windows,
    ConversationManager &conversations, MessageManager &messages, const QIcon &icon)
    : main_(main)
    , windows_(windows)
    , conversations_(conversations)
    , tray_(icon)
    , icon_(icon)
{
    // Receiving notifications must not depend on a visible system tray.
    connect(&messages, &MessageManager::messageAdded, this, &NotificationManager::notify);
    connect(&conversations_, &ConversationManager::conversationsChanged, this,
        &NotificationManager::refreshBadge);
#ifdef Q_OS_LINUX
    native_ = std::make_unique<LinuxNotifier>();
    connect(native_.get(), &LinuxNotifier::activated, this, [this](const QString &id) {
        const auto conversation = notificationConversations_.take(id);
        if (conversations_.conversation(conversation))
            windows_.open(conversation);
    });
    connect(native_.get(), &LinuxNotifier::failed, this, [this](const QString &id) {
        const auto conversation = notificationConversations_.take(id);
        const auto value = conversations_.conversation(conversation);
        if (!value || value->muted || windows_.isActive(conversation)
            || !QSettings().value(QStringLiteral("notifications/enabled"), true).toBool()
            || !tray_.isVisible() || !QSystemTrayIcon::supportsMessages())
            return;
        lastNotification_ = conversation;
        tray_.showMessage(tr("PhosChat"), tr("You have a new message."), icon_);
    });
#endif
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
    connect(menu_, &QMenu::aboutToShow, this, [alerts] {
        const QSignalBlocker blocker(alerts);
        alerts->setChecked(QSettings().value(QStringLiteral("notifications/enabled"), true).toBool());
    });
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
    refreshBadge();
    tray_.show();
    main_.setTrayAvailable(true);
}

NotificationManager::~NotificationManager() = default;

void NotificationManager::notify(const Message &message)
{
    const auto value = conversations_.conversation(message.conversation);
    if (message.historical || message.outgoing || !value || value->muted
        || windows_.isActive(message.conversation)
        || !QSettings().value(QStringLiteral("notifications/enabled"), true).toBool())
        return;
    const auto body = message.mention ? tr("You have a new mention.") : tr("You have a new message.");
#ifdef Q_OS_LINUX
    auto id = notificationConversations_.key(message.conversation);
    if (id.isEmpty()) {
        // Bound activation metadata even if the desktop never dismisses alerts.
        if (notificationConversations_.size() >= 128) {
            const auto obsolete = notificationConversations_.begin().key();
            native_->withdraw(obsolete);
            notificationConversations_.remove(obsolete);
        }
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        notificationConversations_.insert(id, message.conversation);
    }
    native_->post(id, tr("PhosChat"), body);
#else
    if (!tray_.isVisible() || !QSystemTrayIcon::supportsMessages())
        return;
    lastNotification_ = message.conversation;
    tray_.showMessage(tr("PhosChat"), body, icon_);
#endif
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
