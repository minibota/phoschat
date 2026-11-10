#include "ui/windows/ChatWindow.hpp"

#include "ui/Presentation.hpp"
#include "ui/conversations/ConversationController.hpp"
#include "ui/conversations/MessageView.hpp"
#include "ui/styles/Theme.hpp"

#include <QCloseEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace phoschat::ui {
namespace {

    constexpr int kAvatarSize = 32;

} // namespace

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
    header->setSpacing(9);
    m_avatar = new QToolButton;
    m_avatar->setObjectName(QStringLiteral("contactProfilePhoto"));
    m_avatar->setProperty("role", QStringLiteral("chatProfile"));
    m_avatar->setFixedSize(kAvatarSize + 2, kAvatarSize + 2);
    m_avatar->setIconSize(QSize(kAvatarSize, kAvatarSize));
    m_avatar->setCursor(Qt::PointingHandCursor);
    m_avatar->hide();
    header->addWidget(m_avatar, 0, Qt::AlignVCenter);
    auto *titleBlock = new QVBoxLayout;
    titleBlock->setSpacing(1);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(6);
    m_title = new QToolButton;
    m_title->setObjectName(QStringLiteral("contactProfileName"));
    m_title->setProperty("role", QStringLiteral("chatProfileTitle"));
    m_title->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_title->setMinimumWidth(1);
    m_title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_title->setCursor(Qt::PointingHandCursor);
    titleRow->addWidget(m_title);
    m_presenceDot = new QLabel;
    m_presenceDot->setObjectName(QStringLiteral("contactPresenceIndicator"));
    m_presenceDot->setFixedSize(8, 8);
    m_presenceDot->hide();
    titleRow->addWidget(m_presenceDot, 0, Qt::AlignVCenter);
    titleRow->addStretch(1);
    connect(m_avatar, &QToolButton::clicked, this, &ChatWindow::profileRequested);
    connect(m_title, &QToolButton::clicked, this, &ChatWindow::profileRequested);
    m_subtitle = new QLabel;
    m_subtitle->setProperty("role", QStringLiteral("muted"));
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setMinimumWidth(0);
    m_subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleBlock->addLayout(titleRow);
    titleBlock->addWidget(m_subtitle);
    header->addLayout(titleBlock, 1);
    m_membersButton = new QPushButton(tr("People"));
    m_membersButton->setCheckable(true);
    m_membersButton->setToolTip(tr("Show the current channel member list"));
    header->addWidget(m_membersButton, 0, Qt::AlignTop);
    content->addLayout(header);

    auto *divider = new QFrame;
    divider->setProperty("role", QStringLiteral("rule"));
    divider->setFixedHeight(1);
    content->addWidget(divider);

    m_historyButton = new QPushButton;
    m_historyButton->setObjectName(QStringLiteral("loadOlderMessages"));
    m_historyButton->setToolTip(tr("Load the next page of earlier messages in this conversation"));
    content->addWidget(m_historyButton);
    connect(m_historyButton, &QPushButton::clicked, this, [this] { m_controller.requestOlderHistory(); });
    connect(&m_controller, &ConversationController::historyStateChanged, this, &ChatWindow::refreshHistory);
    refreshHistory();

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
    m_attach = new QPushButton(tr("Attach…"));
    m_attach->setAccessibleName(tr("Send an attachment"));
    composer->addWidget(m_attach);
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
    connect(m_attach, &QPushButton::clicked, this, &ChatWindow::attach);
    refreshComposer();
    m_timeline->showLatest();
}

void ChatWindow::refresh(const Conversation &conversation, const QString &accountName, ConnectionState state,
    const QString &statusDetail, Protocol protocol)
{
    setWindowTitle(tr("%1 · PhosChat").arg(conversation.title));
    // Tool buttons use ampersands as mnemonics, including in remote names.
    QString title = conversation.title;
    m_title->setText(title.replace(QLatin1Char('&'), QStringLiteral("&&")));
    const auto profileLabel = tr("View profile for %1").arg(conversation.title);
    m_title->setToolTip(profileLabel.toHtmlEscaped());
    m_title->setAccessibleName(profileLabel);
    m_avatar->setVisible(protocol == Protocol::Telegram);
    if (protocol == Protocol::Telegram) {
        const auto photo = photoAvatar(conversation.avatar, kAvatarSize, devicePixelRatioF());
        m_avatar->setIcon(
            photo.isNull() ? conversationIcon(conversation.kind != ConversationKind::Direct) : QIcon(photo));
        m_avatar->setToolTip(profileLabel.toHtmlEscaped());
        m_avatar->setAccessibleName(profileLabel);
    }
    const auto type = conversation.kind == ConversationKind::Channel ? tr("Channel")
        : conversation.kind == ConversationKind::Group               ? tr("Group")
                                                                     : tr("Direct message");
    m_subtitle->setText(tr("%1 · %2 · %3").arg(accountName, protocolLabel(protocol), type));
    m_subtitle->setToolTip(statusDetail.toHtmlEscaped());
    refreshPresence(conversation.kind == ConversationKind::Direct && state == ConnectionState::Connected
            ? conversation.presence
            : Presence::Unknown);
    m_state = state;
    m_protocol = protocol;
    const bool channel = protocol == Protocol::Irc && conversation.kind == ConversationKind::Channel;
    m_membersButton->setVisible(channel);
    if (!channel) {
        m_membersButton->setChecked(false);
    }
    refreshComposer();
    refreshHistory();
}

void ChatWindow::refreshPresence(Presence presence)
{
    if (presence == m_presence)
        return;
    m_presence = presence;
    QString color;
    QString description;
    switch (presence) {
    case Presence::Online:
        color = QStringLiteral("#43a76b");
        description = tr("Contact is online");
        break;
    case Presence::Away:
        color = QStringLiteral("#d69b31");
        description = tr("Contact is away");
        break;
    case Presence::Offline:
        color = QStringLiteral("#e26368");
        description = tr("Contact is offline");
        break;
    case Presence::Unknown:
        break;
    }
    if (!color.isEmpty())
        m_presenceDot->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(color));
    m_presenceDot->setAccessibleName(description);
    m_presenceDot->setToolTip(description);
    m_presenceDot->setVisible(!color.isEmpty());
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
    m_attach->setVisible(m_controller.canSendFiles());
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

void ChatWindow::attach()
{
    if (!m_controller.canSendFiles())
        return;
    auto *dialog = new QFileDialog(this, tr("Send attachment"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setLabelText(QFileDialog::Accept, tr("Send"));
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString &path) {
        if (!m_controller.sendFile(path))
            m_composerHint->setText(tr("The attachment could not be sent."));
    });
    dialog->open();
}

void ChatWindow::refreshHistory()
{
    m_historyButton->setVisible(m_controller.hasPagedHistory());
    const auto history = m_controller.historyState();
    const bool online = m_state == ConnectionState::Connected;
    m_historyButton->setEnabled(online && history == HistoryState::Ready);
    if (!online) {
        m_historyButton->setText(tr("Connect to load earlier messages"));
    } else if (history == HistoryState::Loading) {
        m_historyButton->setText(tr("Loading messages…"));
    } else if (history == HistoryState::Complete) {
        m_historyButton->setText(tr("Beginning of conversation"));
    } else if (history == HistoryState::Ready) {
        m_historyButton->setText(tr("Load older messages"));
    } else {
        m_historyButton->setText(tr("Message history is unavailable"));
    }
}

} // namespace phoschat::ui
