#include "ui/settings/AccountDialog.hpp"

#include "core/account_manager.h"
#include "ui/Presentation.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace phoschat::ui {

AccountDialog::AccountDialog(AccountManager &accounts, QWidget *parent)
    : QDialog(parent)
    , m_accounts(accounts)
{
    setWindowTitle(tr("Accounts · PhosChat"));
    setMinimumWidth(450);
    auto *content = new QVBoxLayout(this);
    content->setContentsMargins(16, 15, 16, 15);
    content->setSpacing(10);

    auto *heading = new QLabel(tr("Messaging accounts"));
    heading->setProperty("role", QStringLiteral("heading"));
    content->addWidget(heading);
    auto *description
        = new QLabel(tr("Connect IRC networks, Telegram accounts and approved Slack workspaces."));
    description->setProperty("role", QStringLiteral("muted"));
    description->setWordWrap(true);
    content->addWidget(description);

    auto *selector = new QFormLayout;
    m_account = new QComboBox;
    selector->addRow(tr("&Account"), m_account);
    m_protocol = new QComboBox;
    for (const auto protocol : { Protocol::Irc, Protocol::Telegram, Protocol::Slack })
        m_protocol->addItem(protocolLabel(protocol), static_cast<int>(protocol));
    selector->addRow(tr("&Protocol"), m_protocol);
    m_label = new QLineEdit;
    m_label->setPlaceholderText(tr("Optional display name"));
    m_label->setMaxLength(100);
    selector->addRow(tr("&Name"), m_label);
    content->addLayout(selector);

    m_ircNetwork = new QGroupBox(tr("IRC network"));
    auto *form = new QFormLayout(m_ircNetwork);
    m_host = new QLineEdit;
    m_host->setPlaceholderText(QStringLiteral("irc.libera.chat"));
    m_host->setMaxLength(255);
    form->addRow(tr("&Server"), m_host);
    const auto networkHint = tr("Add a new account to change the server, port or TLS mode.");
    m_host->setToolTip(networkHint);
    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_tls = new QCheckBox(tr("Use TLS"));
    m_port->setToolTip(networkHint);
    m_tls->setToolTip(networkHint);
    auto *portRow = new QHBoxLayout;
    portRow->addWidget(m_port);
    portRow->addWidget(m_tls);
    portRow->addStretch(1);
    form->addRow(tr("&Port"), portRow);
    m_nick = new QLineEdit;
    m_nick->setMaxLength(64);
    m_nick->setPlaceholderText(tr("Your IRC nickname"));
    form->addRow(tr("N&ickname"), m_nick);
    m_networkHint = new QLabel(networkHint);
    m_networkHint->setProperty("role", QStringLiteral("muted"));
    m_networkHint->setWordWrap(true);
    form->addRow(m_networkHint);
    content->addWidget(m_ircNetwork);

    m_ircAuthentication = new QGroupBox(tr("SASL authentication (optional)"));
    auto *credentials = new QFormLayout(m_ircAuthentication);
    m_saslUser = new QLineEdit;
    m_saslUser->setMaxLength(255);
    credentials->addRow(tr("&Username"), m_saslUser);
    m_password = new QLineEdit;
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    m_password->setMaxLength(1024);
    credentials->addRow(tr("Pass&word"), m_password);
    content->addWidget(m_ircAuthentication);

    m_cloudSettings = new QGroupBox(tr("Provider setup"));
    auto *cloud = new QFormLayout(m_cloudSettings);
    m_providerId = new QLineEdit;
    m_providerId->setMaxLength(96);
    m_providerIdLabel = new QLabel;
    m_providerIdLabel->setBuddy(m_providerId);
    cloud->addRow(m_providerIdLabel, m_providerId);
    m_cloudSecret = new QLineEdit;
    m_cloudSecret->setEchoMode(QLineEdit::Password);
    m_cloudSecret->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    m_cloudSecret->setMaxLength(4096);
    m_cloudSecretLabel = new QLabel;
    m_cloudSecretLabel->setBuddy(m_cloudSecret);
    cloud->addRow(m_cloudSecretLabel, m_cloudSecret);
    m_cloudHint = new QLabel;
    m_cloudHint->setProperty("role", QStringLiteral("muted"));
    m_cloudHint->setWordWrap(true);
    m_cloudHint->setOpenExternalLinks(true);
    cloud->addRow(m_cloudHint);
    content->addWidget(m_cloudSettings);

    m_remember = new QCheckBox(tr("Remember in the system credential store"));
    m_remember->setEnabled(accounts.persistentSecretsAvailable());
    if (!m_remember->isEnabled()) {
        m_remember->setToolTip(tr("A supported system credential store is unavailable."));
        auto *unavailable
            = new QLabel(tr("Secure storage is unavailable. Credentials are available for this run only."));
        unavailable->setProperty("role", QStringLiteral("muted"));
        unavailable->setWordWrap(true);
        content->addWidget(unavailable);
    }
    content->addWidget(m_remember);
    m_autoConnect = new QCheckBox(tr("Connect when PhosChat starts"));
    content->addWidget(m_autoConnect);

    m_status = new QLabel;
    m_status->setProperty("role", QStringLiteral("muted"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    content->addWidget(m_status);
    m_feedback = new QLabel;
    m_feedback->setProperty("role", QStringLiteral("error"));
    m_feedback->setTextFormat(Qt::PlainText);
    m_feedback->setWordWrap(true);
    m_feedback->hide();
    content->addWidget(m_feedback);

    auto *actions = new QDialogButtonBox(QDialogButtonBox::Close);
    m_remove = actions->addButton(tr("Remove"), QDialogButtonBox::DestructiveRole);
    m_disconnect = actions->addButton(tr("Disconnect"), QDialogButtonBox::ActionRole);
    auto *saveButton = actions->addButton(tr("Save"), QDialogButtonBox::ActionRole);
    m_connectButton = actions->addButton(tr("Save && connect"), QDialogButtonBox::ActionRole);
    m_connectButton->setProperty("role", QStringLiteral("primary"));
    for (auto *button : actions->buttons()) {
        if (auto *push = qobject_cast<QPushButton *>(button)) {
            push->setAutoDefault(false);
        }
    }
    content->addWidget(actions);

    connect(m_account, &QComboBox::currentIndexChanged, this, &AccountDialog::loadSelected);
    connect(m_protocol, &QComboBox::currentIndexChanged, this, &AccountDialog::refreshProtocolFields);
    connect(m_tls, &QCheckBox::toggled, this, [this](bool tls) {
        if (m_port->value() == (tls ? 6667 : 6697)) {
            m_port->setValue(tls ? 6697 : 6667);
        }
    });
    connect(actions, &QDialogButtonBox::rejected, this, &AccountDialog::reject);
    connect(saveButton, &QPushButton::clicked, this, [this] { save(false); });
    connect(m_connectButton, &QPushButton::clicked, this, [this] { save(true); });
    connect(m_disconnect, &QPushButton::clicked, this,
        [this] { m_accounts.disconnectAccount(m_account->currentData().toString()); });
    connect(m_remove, &QPushButton::clicked, this, &AccountDialog::removeSelected);
    connect(&m_accounts, &AccountManager::accountRemoved, this, [this](const QString &id) {
        const auto current = m_account->currentData().toString();
        refreshAccountList(current == id ? QString {} : current);
    });
    connect(&m_accounts, &AccountManager::accountStateChanged, this, [this] { refreshStatus(); });
    connect(
        &m_accounts, &AccountManager::errorOccurred, this, [this](const QString &id, const QString &message) {
            if (id == m_account->currentData().toString() || id == m_savingAccountId) {
                m_feedback->setText(message);
                m_feedback->show();
            }
        });

    const auto existing = m_accounts.accounts();
    refreshAccountList(existing.isEmpty() ? QString {} : existing.front().id);
}

void AccountDialog::prefillIrcNetwork(const QString &label, const QString &host, quint16 port)
{
    refreshAccountList({});
    m_protocol->setCurrentIndex(m_protocol->findData(static_cast<int>(Protocol::Irc)));
    m_label->setText(label);
    m_host->setText(host);
    m_tls->setChecked(true);
    m_port->setValue(port);
    m_nick->setFocus();
}

void AccountDialog::refreshAccountList(const QString &selection)
{
    const QSignalBlocker blocker(m_account);
    m_account->clear();
    m_account->addItem(tr("Add an account"), QString {});
    for (const auto &account : m_accounts.accounts()) {
        m_account->addItem(accountLabel(account), account.id);
    }
    m_account->setCurrentIndex(std::max(0, m_account->findData(selection)));
    loadSelected();
}

void AccountDialog::loadSelected()
{
    const auto accounts = m_accounts.accounts();
    const auto id = m_account->currentData().toString();
    const auto existing = std::ranges::find(accounts, id, &Account::id);
    const auto account = existing == accounts.end() ? Account {} : *existing;
    const bool isNew = existing == accounts.end();
    const QSignalBlocker blocker(m_protocol);
    m_protocol->setCurrentIndex(m_protocol->findData(static_cast<int>(account.protocol)));
    m_protocol->setEnabled(isNew);
    m_host->setEnabled(isNew);
    m_port->setEnabled(isNew);
    m_tls->setEnabled(isNew);
    m_networkHint->setVisible(!isNew);
    m_label->setText(account.label);
    m_host->setText(account.host);
    m_tls->setChecked(account.tls);
    m_port->setValue(account.port);
    m_nick->setText(account.nick);
    m_saslUser->setText(account.saslUser);
    m_providerId->setText(account.providerId);
    m_providerId->setEnabled(isNew);
    m_remember->setChecked(account.saveSecret);
    m_autoConnect->setChecked(account.autoConnect);
    m_password->clear();
    m_cloudSecret->clear();
    m_password->setPlaceholderText(
        account.saveSecret ? tr("Leave blank to keep the saved password") : QString {});
    m_feedback->hide();
    refreshProtocolFields();
}

void AccountDialog::refreshProtocolFields()
{
    const auto protocol = static_cast<Protocol>(m_protocol->currentData().toInt());
    const bool irc = protocol == Protocol::Irc;
    const bool telegram = protocol == Protocol::Telegram;
    m_ircNetwork->setVisible(irc);
    m_ircAuthentication->setVisible(irc);
    m_cloudSettings->setVisible(!irc);
    m_providerIdLabel->setText(telegram ? tr("API &ID") : tr("Client &ID"));
    m_cloudSecretLabel->setText(telegram ? tr("API &hash") : tr("User &token (optional)"));
    m_providerId->setPlaceholderText(telegram ? QStringLiteral("123456") : QStringLiteral("1234.5678"));
    m_cloudSecret->setPlaceholderText(m_remember->isChecked() ? tr("Leave blank to keep the saved credential")
            : telegram                                        ? QString {}
                                                              : tr("Leave blank for browser sign-in"));
    if (telegram) {
        m_cloudHint->setText(m_accounts.protocolAvailable(protocol)
                ? tr("Use the API ID and hash for your own application. Phone verification follows. "
                     "<a href='https://github.com/minibota/phoschat/blob/main/docs/telegram.md'>Telegram "
                     "setup</a>")
                : tr("Telegram requires the official TDLib runtime. "
                     "<a href='https://github.com/minibota/phoschat/blob/main/docs/telegram.md'>Installation "
                     "guide</a>"));
    } else {
        m_cloudHint->setText(
            tr("Use a PKCE-enabled Slack app approved for your workspace. "
               "<a href='https://github.com/minibota/phoschat/blob/main/docs/slack.md'>Slack setup</a>"));
    }
    m_connectButton->setEnabled(m_accounts.protocolAvailable(protocol));
    refreshStatus();
}

void AccountDialog::refreshStatus()
{
    const auto id = m_account->currentData().toString();
    const bool existing = !id.isEmpty();
    const auto state = m_accounts.state(id);
    const auto detail = m_accounts.statusDetail(id);
    const auto protocol = static_cast<Protocol>(m_protocol->currentData().toInt());
    m_status->setText(!existing ? tr("New %1 account").arg(protocolLabel(protocol))
            : detail.isEmpty()  ? connectionLabel(state)
                                : tr("%1 · %2").arg(connectionLabel(state), detail));
    m_remove->setEnabled(existing);
    m_disconnect->setEnabled(existing && state != ConnectionState::Disconnected);
}

void AccountDialog::save(bool connectAfter)
{
    m_feedback->hide();
    const auto host = m_host->text().trimmed();
    const auto nick = m_nick->text().trimmed();
    const auto saslUser = m_saslUser->text().trimmed();
    const auto protocol = static_cast<Protocol>(m_protocol->currentData().toInt());
    if (protocol == Protocol::Irc && (host.isEmpty() || nick.isEmpty())) {
        m_feedback->setText(tr("Enter a server and nickname."));
        m_feedback->show();
        (host.isEmpty() ? m_host : m_nick)->setFocus();
        return;
    }
    if (protocol == Protocol::Irc && !m_tls->isChecked()
        && (!saslUser.isEmpty() || !m_password->text().isEmpty())) {
        m_feedback->setText(tr("Enable TLS to use SASL authentication."));
        m_feedback->show();
        return;
    }

    Account account;
    account.protocol = protocol;
    account.id = m_account->currentData().toString();
    if (account.id.isEmpty()) {
        account.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    account.label = m_label->text().trimmed();
    if (account.label.isEmpty()) {
        account.label = protocol == Protocol::Irc ? host : protocolLabel(protocol);
    }
    if (protocol == Protocol::Irc) {
        account.host = host;
        account.port = static_cast<quint16>(m_port->value());
        account.tls = m_tls->isChecked();
        account.nick = nick;
        account.saslUser = saslUser;
    } else {
        account.providerId = m_providerId->text().trimmed();
    }
    account.saveSecret = m_remember->isChecked();
    account.autoConnect = m_autoConnect->isChecked();

    auto password
        = protocol == Protocol::Irc ? m_password->text().toUtf8() : m_cloudSecret->text().trimmed().toUtf8();
    const QScopedValueRollback saving(m_savingAccountId, account.id);
    const bool saved = m_accounts.saveAccount(account, password);
    password.fill('\0');
    if (!saved) {
        if (m_feedback->isHidden()) {
            m_feedback->setText(
                tr("The account could not be saved. Check its connection and credential settings."));
            m_feedback->show();
        }
        return;
    }
    refreshAccountList(account.id);
    if (connectAfter) {
        m_accounts.connectAccount(account.id);
    }
}

void AccountDialog::removeSelected()
{
    const auto id = m_account->currentData().toString();
    if (id.isEmpty()) {
        return;
    }
    QMessageBox confirmation(QMessageBox::Warning, tr("Remove account"),
        tr("Remove %1 and delete its local conversations, history and stored credentials?")
            .arg(m_account->currentText()),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    if (confirmation.exec() == QMessageBox::Yes) {
        m_accounts.removeAccount(id);
    }
}

} // namespace phoschat::ui
