#pragma once

#include "models/types.h"

#include <QIcon>
#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>

#include <optional>

class QMenu;

namespace phoschat {

class ConversationManager;
class MessageManager;
namespace ui {
    class MainWindow;
    class WindowManager;
}

class NotificationManager final : public QObject {
public:
    NotificationManager(ui::MainWindow &main, ui::WindowManager &windows, ConversationManager &conversations,
        MessageManager &messages, const QIcon &icon);
    ~NotificationManager() override;

private:
    void notify(const Message &message);
    void refreshBadge();

    ui::MainWindow &main_;
    ui::WindowManager &windows_;
    ConversationManager &conversations_;
    QSystemTrayIcon tray_;
    QPointer<QMenu> menu_;
    QIcon icon_;
    std::optional<ConversationId> lastNotification_;
};

} // namespace phoschat
