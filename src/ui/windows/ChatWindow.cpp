#include "ui/windows/ChatWindow.hpp"

#include "ui/Presentation.hpp"
#include "ui/conversations/ConversationController.hpp"
#include "ui/conversations/MessageView.hpp"
#include "ui/styles/Theme.hpp"

#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace phoschat::ui {

ChatWindow::ChatWindow(ConversationController &controller)
    : m_controller(controller)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(520, 490);
    setMinimumSize(340, 290);
    setWindowIcon(brandIcon());

    auto *content = new QVBoxLayout(this);
    content->setContentsMargins(15, 13, 15, 13);
    content->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->setSpacing(11);
    auto *titleBlock = new QVBoxLayout;
    titleBlock->setSpacing(1);
    m_title = new QLabel;
    m_title->setProperty("role", QStringLiteral("heading"));
    m_title->setTextFormat(Qt::PlainText);
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_subtitle = new QLabel;
    m_subtitle->setProperty("role", QStringLiteral("muted"));
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setMinimumWidth(0);
    m_subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleBlock->addWidget(m_title);
    titleBlock->addWidget(m_subtitle);
    header->addLayout(titleBlock, 1);
    m_status = new QLabel;
    m_status->setProperty("role", QStringLiteral("badge"));
    m_status->setTextFormat(Qt::PlainText);
    header->addWidget(m_status, 0, Qt::AlignTop);
    m_membersButton = new QPushButton(tr("People"));
    m_membersButton->setCheckable(true);
    m_membersButton->setToolTip(tr("Show the current channel member list"));
    header->addWidget(m_membersButton, 0, Qt::AlignTop);
    content->addLayout(header);

    auto *divider = new QFrame;
    divider->setProperty("role", QStringLiteral("rule"));
    divider->setFixedHeight(1);
    content->addWidget(divider);

    auto *body = new QHBoxLayout;
    m_timeline = new MessageView(m_controller.model());
    body->addWidget(m_timeline, 1);
    m_members = new QListWidget;
    m_members->setAccessibleName(tr("Channel members"));
    m_members->setFixedWidth(114);
    m_members->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_members->hide();
    body->addWidget(m_members);
    connect(m_membersButton, &QPushButton::toggled, m_members, &QListWidget::setVisible);
    content->addLayout(body, 1);

    auto *composer = new QHBoxLayout;
    m_composer = new QLineEdit(m_controller.draft());
    m_composer->setAccessibleName(tr("Message"));
    m_composer->setMaxLength(4096);
    composer->addWidget(m_composer, 1);
    m_send = new QPushButton(tr("Send"));
    m_send->setProperty("role", QStringLiteral("primary"));
    composer->addWidget(m_send);
    content->addLayout(composer);
    m_composerHint = new QLabel;
    m_composerHint->setProperty("role", QStringLiteral("muted"));
    m_composerHint->setWordWrap(true);
    content->addWidget(m_composerHint);

    connect(m_composer, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_controller.setDraft(text);
        refreshComposer();
    });
    connect(m_composer, &QLineEdit::returnPressed, this, &ChatWindow::send);
    connect(m_send, &QPushButton::clicked, this, &ChatWindow::send);
    refreshComposer();
    m_timeline->showLatest();
}

void ChatWindow::refresh(const Conversation &conversation, const QString &accountName, ConnectionState state,
    const QString &statusDetail, Protocol protocol)
{
    setWindowTitle(tr("%1 · PhosChat").arg(conversation.title));
    m_title->setText(conversation.title);
    m_title->setToolTip(conversation.title.toHtmlEscaped());
    const auto type = conversation.kind == ConversationKind::Channel ? tr("Channel")
        : conversation.kind == ConversationKind::Group               ? tr("Group")
                                                                     : tr("Direct message");
    m_subtitle->setText(tr("%1 · %2 · %3").arg(accountName, protocolLabel(protocol), type));
    m_status->setText(connectionLabel(state));
    const bool connected = state == ConnectionState::Connected;
    if (m_status->property("connected").isNull() || m_status->property("connected").toBool() != connected) {
        m_status->setProperty("connected", connected);
        m_status->style()->unpolish(m_status);
        m_status->style()->polish(m_status);
    }
    m_status->setToolTip(statusDetail.toHtmlEscaped());
    m_state = state;
    m_protocol = protocol;
    const bool channel = protocol == Protocol::Irc && conversation.kind == ConversationKind::Channel;
    m_membersButton->setVisible(channel);
    if (!channel) {
        m_membersButton->setChecked(false);
    }
    refreshComposer();
}

void ChatWindow::setMembers(const QStringList &members)
{
    m_members->clear();
    m_members->addItems(members);
    m_membersButton->setText(members.isEmpty() ? tr("People") : tr("People · %1").arg(members.size()));
}

void ChatWindow::focusComposer() { m_composer->setFocus(); }

void ChatWindow::closeEvent(QCloseEvent *event)
{
    emit closing(saveGeometry());
    QWidget::closeEvent(event);
}

void ChatWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        emit activated();
    }
}

void ChatWindow::send()
{
    if (!m_send->isEnabled()) {
        return;
    }
    if (m_controller.send()) {
        m_composer->clear();
        focusComposer();
    } else {
        m_composerHint->setText(tr("Message could not be sent. Your draft is still in the composer."));
    }
}

void ChatWindow::refreshComposer()
{
    const bool online = m_state == ConnectionState::Connected;
    const bool available = m_controller.canSend();
    const auto text = m_composer->text();
    const bool irc = m_protocol == Protocol::Irc;
    const bool command = irc && text.startsWith(QLatin1Char('/')) && !text.startsWith(QStringLiteral("//"));
    m_send->setEnabled((available || (online && command)) && !text.trimmed().isEmpty());
    m_composer->setPlaceholderText(available ? tr("Write a message…") : tr("Write a draft…"));
    if (available) {
        m_composerHint->setText(irc ? tr("Enter to send · / for IRC commands") : tr("Enter to send"));
    } else if (online && irc) {
        m_composerHint->setText(tr("Join the channel to send messages. IRC commands are available."));
    } else if (online) {
        m_composerHint->setText(tr("Sending is unavailable in this conversation."));
    } else {
        m_composerHint->setText(tr("You can send once the account is connected."));
    }
}

} // namespace phoschat::ui
