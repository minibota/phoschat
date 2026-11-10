#pragma once

#include "models/types.h"

#include <QHash>
#include <QMainWindow>
#include <optional>

class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace phoschat {
class AccountManager;
class ConversationManager;
}

namespace phoschat::ui {

class WindowManager;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(AccountManager &accounts, ConversationManager &conversations, WindowManager &windows,
        QWidget *parent = nullptr);

    void setTrayAvailable(bool available);
    void showStatusMessage(const QString &message);
    void prepareToQuit();

signals:
    void quitRequested();
    void hiddenToTray();

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class Grouping { Account, Protocol, Custom, Recent };

    void saveWindowGeometry() const;
    void showAccounts();
    void showIrcDirectory();
    void showStartConversation(bool channel);
    void reloadConversations();
    void refreshProfile();
    void refreshStatus();
    void refreshNetworks();
    void showConversationMenu(const QPoint &position);
    void setGrouping(Grouping grouping);
    std::optional<ConversationId> itemConversation(const QTreeWidgetItem *item) const;

    AccountManager &m_accounts;
    ConversationManager &m_conversations;
    WindowManager &m_windows;
    QLineEdit *m_search {};
    QTreeWidget *m_tree {};
    QWidget *m_empty {};
    QLabel *m_emptyHeading {};
    QLabel *m_emptyDescription {};
    QPushButton *m_emptyAction {};
    QLabel *m_avatar {};
    QLabel *m_profileName {};
    QLabel *m_profileState {};
    QLabel *m_status {};
    QLabel *m_unread {};
    QTimer *m_statusTimer {};
    QTimer *m_reloadTimer {};
    QString m_lastFilter;
    QMenu *m_networks {};
    QAction *m_closeToTray {};
    QAction *m_minimizeToTray {};
    QHash<QString, bool> m_expanded;
    Grouping m_grouping { Grouping::Account };
    bool m_trayAvailable {};
    bool m_quitting {};
};

} // namespace phoschat::ui
