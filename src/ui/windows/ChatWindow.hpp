#pragma once

#include "models/types.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

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
    void closing(const QByteArray &geometry);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void send();
    void refreshComposer();

    ConversationController &m_controller;
    MessageView *m_timeline {};
    QLabel *m_title {};
    QLabel *m_subtitle {};
    QLabel *m_status {};
    QLabel *m_composerHint {};
    QLineEdit *m_composer {};
    QPushButton *m_send {};
    QPushButton *m_membersButton {};
    QListWidget *m_members {};
    ConnectionState m_state { ConnectionState::Disconnected };
    Protocol m_protocol { Protocol::Irc };
};

} // namespace phoschat::ui
