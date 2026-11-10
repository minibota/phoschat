#pragma once

#include "models/types.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;

namespace phoschat::ui {

class ConversationController;
class MessageView;

class ChatWindow final : public QWidget {
    Q_OBJECT
public:
    explicit ChatWindow(ConversationController &controller);

    void refresh(const Conversation &conversation, const QString &accountName, ConnectionState state,
        const QString &statusDetail, Protocol protocol);
    void setMembers(const QStringList &members);
    void focusComposer();

signals:
    void activated();
    void profileRequested();
    void closing(const QByteArray &geometry);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void send();
    void attach();
    void refreshComposer();
    void refreshHistory();
    void refreshPresence(Presence presence);

    ConversationController &m_controller;
    MessageView *m_timeline {};
    QToolButton *m_title {};
    QToolButton *m_avatar {};
    QLabel *m_subtitle {};
    QLabel *m_presenceDot {};
    QLabel *m_composerHint {};
    QLineEdit *m_composer {};
    QPushButton *m_send {};
    QPushButton *m_attach {};
    QPushButton *m_historyButton {};
    QPushButton *m_membersButton {};
    QListWidget *m_members {};
    ConnectionState m_state { ConnectionState::Disconnected };
    Presence m_presence { Presence::Unknown };
    Protocol m_protocol { Protocol::Irc };
};

} // namespace phoschat::ui
