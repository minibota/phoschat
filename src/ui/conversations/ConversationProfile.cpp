#include "ui/conversations/ConversationProfile.hpp"

#include "core/account_manager.h"
#include "ui/Presentation.hpp"
#include "ui/styles/Theme.hpp"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace phoschat::ui {
namespace {

    QLabel *field(const QString &value)
    {
        auto *label = new QLabel;
        label->setTextFormat(Qt::PlainText);
        label->setText(value);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        return label;
    }

    QString profileLabel(const char *text)
    {
        return QCoreApplication::translate("ConversationProfile", text);
    }

} // namespace

void showConversationProfile(QWidget &parent, AccountManager &accounts, const Conversation &conversation)
{
    if (auto *opened
        = parent.findChild<QDialog *>(QStringLiteral("conversationProfile"), Qt::FindDirectChildrenOnly)) {
        opened->showNormal();
        opened->raise();
        opened->activateWindow();
        return;
    }

    const auto accountList = accounts.accounts();
    const auto account = std::ranges::find(accountList, conversation.id.accountId, &Account::id);
    if (account == accountList.end())
        return;
    auto *dialog = new QDialog(&parent);
    dialog->setObjectName(QStringLiteral("conversationProfile"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(conversation.kind == ConversationKind::Direct ? profileLabel("Contact information")
            : conversation.kind == ConversationKind::Group               ? profileLabel("Group information")
                                                           : profileLabel("Channel information"));
    dialog->resize(392, 300);
    dialog->setMinimumWidth(290);

    auto *outer = new QVBoxLayout(dialog);
    auto *identity = new QHBoxLayout;
    if (const auto image = photoAvatar(conversation.avatar, 42, dialog->devicePixelRatioF());
        !image.isNull()) {
        auto *photo = new QLabel;
        photo->setPixmap(image);
        photo->setAccessibleName(profileLabel("Profile photo"));
        identity->addWidget(photo);
    }
    auto *title = field(conversation.title);
    title->setProperty("role", QStringLiteral("heading"));
    identity->addWidget(title, 1);
    outer->addLayout(identity);

    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget;
    auto *body = new QVBoxLayout(content);
    body->setContentsMargins(0, 4, 0, 4);
    auto *details = new QFormLayout;
    details->setRowWrapPolicy(QFormLayout::WrapLongRows);
    details->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    details->addRow(field(profileLabel("Protocol")), field(protocolLabel(account->protocol)));
    details->addRow(field(profileLabel("Account")), field(accountLabel(*account)));
    if (account->protocol == Protocol::Irc && !account->host.isEmpty())
        details->addRow(field(profileLabel("Network")), field(account->host));
    body->addLayout(details);

    auto *remote = new QFormLayout;
    remote->setRowWrapPolicy(QFormLayout::WrapLongRows);
    remote->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    body->addLayout(remote);
    auto *status = field(profileLabel("Loading profile…"));
    status->setProperty("role", QStringLiteral("muted"));
    body->addWidget(status);
    body->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    outer->addWidget(buttons);
    const auto id = conversation.id;
    const auto finish = [remote, status](const ProfileDetails &values) {
        while (remote->rowCount() > 0)
            remote->removeRow(0);
        for (const auto &[name, value] : values) {
            if (!name.isEmpty() && !value.isEmpty())
                remote->addRow(field(name), field(value));
        }
        status->setText(profileLabel("No additional information is available."));
        status->setVisible(remote->rowCount() == 0);
    };
    QObject::connect(&accounts, &AccountManager::profileReceived, dialog,
        [id, finish](const ConversationId &result, const ProfileDetails &values) {
            if (result == id)
                finish(values);
        });
    dialog->show();
    if (!accounts.requestProfile(id)) {
        finish({});
    } else {
        QTimer::singleShot(12000, dialog, [status, finish] {
            if (status->text() == profileLabel("Loading profile…"))
                finish({});
        });
    }
}

} // namespace phoschat::ui
