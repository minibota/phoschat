#include "ui/settings/AccountDialog.hpp"

#include "core/account_manager.h"
#include "providers/irc/irc_directory.h"
#include "providers/slack/slack_app_config.h"
#include "security/telegram_app_credentials.h"
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
    m_account->setObjectName(QStringLiteral("accountSelector"));
    selector->addRow(tr("&Account"), m_account);
    m_protocol = new QComboBox;
    m_protocol->setObjectName(QStringLiteral("protocolSelector"));
    for (const auto protocol : { Protocol::Irc, Protocol::Telegram, Protocol::Slack })
        m_protocol->addItem(protocolLabel(protocol), static_cast<int>(protocol));
    selector->addRow(tr("&Protocol"), m_protocol);
    m_label = new QLineEdit;
    m_label->setObjectName(QStringLiteral("accountName"));
    m_label->setPlaceholderText(tr("Optional display name"));
    m_label->setMaxLength(100);
    selector->addRow(tr("&Name"), m_label);
    content->addLayout(selector);

    m_ircNetwork = new QGroupBox(tr("IRC network"));
    auto *form = new QFormLayout(m_ircNetwork);
    m_ircPreset = new QComboBox;
    m_ircPreset->setObjectName(QStringLiteral("ircNetworkPreset"));
    for (const auto &network : irc::recommendedNetworks()) {
        m_ircPreset->addItem(network.name.toString(), network.host.toString());
        const int index = m_ircPreset->count() - 1;
        m_ircPreset->setItemData(index, network.port, Qt::UserRole + 1);
        m_ircPreset->setItemData(
            index, tr("%1 · TLS on port %2").arg(network.host.toString()).arg(network.port), Qt::ToolTipRole);
    }
    m_ircPreset->addItem(tr("Custom server…"), QString {});
    form->addRow(tr("&Network"), m_ircPreset);
    m_host = new QLineEdit;
    m_host->setObjectName(QStringLiteral("ircServer"));
    m_host->setPlaceholderText(QStringLiteral("irc.libera.chat"));
    m_host->setMaxLength(255);
    form->addRow(tr("&Server"), m_host);
    const auto networkHint = tr("Add a new account to change the server, port or TLS mode.");
    m_host->setToolTip(networkHint);
    m_port = new QSpinBox;
    m_port->setObjectName(QStringLiteral("ircPort"));
    m_port->setRange(1, 65535);
    m_tls = new QCheckBox(tr("Use TLS"));
    m_tls->setObjectName(QStringLiteral("ircTls"));
    m_port->setToolTip(networkHint);
    m_tls->setToolTip(networkHint);
    auto *portRow = new QHBoxLayout;
    portRow->addWidget(m_port);
    portRow->addWidget(m_tls);
    portRow->addStretch(1);
    auto *portLabel = new QLabel(tr("&Port"));
    portLabel->setBuddy(m_port);
    form->addRow(portLabel, portRow);
    m_nick = new QLineEdit;
    m_nick->setObjectName(QStringLiteral("ircNickname"));
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
    m_customTelegramApplication = new QCheckBox(tr("Use your own Telegram application (advanced)"));
    m_customTelegramApplication->setObjectName(QStringLiteral("customTelegramApplication"));
    cloud->addRow(m_customTelegramApplication);
    m_customSlackApplication = new QCheckBox(tr("Use a workspace app or token (advanced)"));
    m_customSlackApplication->setObjectName(QStringLiteral("customSlackApplication"));
    cloud->addRow(m_customSlackApplication);
    m_providerId = new QLineEdit;
    m_providerId->setObjectName(QStringLiteral("providerApplicationId"));
    m_providerId->setMaxLength(96);
    m_providerIdLabel = new QLabel;
    m_providerIdLabel->setBuddy(m_providerId);
    cloud->addRow(m_providerIdLabel, m_providerId);
    m_cloudSecret = new QLineEdit;
    m_cloudSecret->setObjectName(QStringLiteral("providerCredential"));
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
    m_remember->setObjectName(QStringLiteral("rememberSession"));
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
    saveButton->setObjectName(QStringLiteral("saveAccount"));
    m_connectButton = actions->addButton(tr("Save && connect"), QDialogButtonBox::ActionRole);
    m_connectButton->setObjectName(QStringLiteral("connectAccount"));
    m_connectButton->setProperty("role", QStringLiteral("primary"));
    for (auto *button : actions->buttons()) {
        if (auto *push = qobject_cast<QPushButton *>(button)) {
            push->setAutoDefault(false);
        }
    }
    content->addWidget(actions);

    connect(m_account, &QComboBox::currentIndexChanged, this, &AccountDialog::loadSelected);
    connect(m_protocol, &QComboBox::currentIndexChanged, this, &AccountDialog::refreshProtocolFields);
    connect(m_ircPreset, &QComboBox::currentIndexChanged, this, &AccountDialog::applyIrcPreset);
    connect(m_customTelegramApplication, &QCheckBox::toggled, this, &AccountDialog::refreshProtocolFields);
    connect(m_customSlackApplication, &QCheckBox::toggled, this, &AccountDialog::refreshProtocolFields);
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
    syncIrcPreset();
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
    m_label->setText(account.label);
    m_host->setText(account.host);
    m_tls->setChecked(account.tls);
    m_port->setValue(account.port);
    m_nick->setText(account.nick);
    m_saslUser->setText(account.saslUser);
    m_providerId->setText(account.providerId);
    m_providerId->setEnabled(isNew);
    {
        const QSignalBlocker customBlocker(m_customTelegramApplication);
        m_customTelegramApplication->setChecked(
            !account.providerId.isEmpty() || (isNew && !bundledTelegramCredentialsAvailable()));
    }
    m_customTelegramApplication->setEnabled(isNew);
    {
        const QSignalBlocker customBlocker(m_customSlackApplication);
        m_customSlackApplication->setChecked(
            !isNew && (!bundledSlackApplicationAvailable() || account.providerId != bundledSlackClientId()));
    }
    m_customSlackApplication->setEnabled(isNew);
    m_telegramDefaultsApplied = !isNew;
    m_slackDefaultsApplied = !isNew;
    m_remember->setChecked(account.saveSecret);
    m_autoConnect->setChecked(account.autoConnect);
    m_password->clear();
    m_cloudSecret->clear();
    if (isNew) {
        const QSignalBlocker presetBlocker(m_ircPreset);
        m_ircPreset->setCurrentIndex(0);
        applyIrcPreset();
    } else {
        syncIrcPreset();
    }
    m_password->setPlaceholderText(
        account.saveSecret ? tr("Leave blank to keep the saved password") : QString {});
    m_feedback->hide();
    refreshProtocolFields();
}

void AccountDialog::applyIrcPreset()
{
    if (m_account->currentData().toString().isEmpty()) {
        const auto host = m_ircPreset->currentData().toString();
        if (!host.isEmpty()) {
            if (m_host->text().compare(host, Qt::CaseInsensitive) != 0) {
                m_saslUser->clear();
                m_password->clear();
            }
            m_host->setText(host);
            m_tls->setChecked(true);
            m_port->setValue(m_ircPreset->currentData(Qt::UserRole + 1).toInt());
        }
    }
    refreshIrcEndpointFields();
}

void AccountDialog::syncIrcPreset()
{
    int selected = m_ircPreset->count() - 1;
    for (int index = 0; index < m_ircPreset->count() - 1; ++index) {
        if (m_tls->isChecked()
            && m_host->text().compare(m_ircPreset->itemData(index).toString(), Qt::CaseInsensitive) == 0
            && m_port->value() == m_ircPreset->itemData(index, Qt::UserRole + 1).toInt()) {
            selected = index;
            break;
        }
    }
    const QSignalBlocker blocker(m_ircPreset);
    m_ircPreset->setCurrentIndex(selected);
    refreshIrcEndpointFields();
}

void AccountDialog::refreshIrcEndpointFields()
{
    const bool existing = !m_account->currentData().toString().isEmpty();
    const bool custom = m_ircPreset->currentData().toString().isEmpty();
    m_ircPreset->setEnabled(!existing);
    for (auto *field :
        { static_cast<QWidget *>(m_host), static_cast<QWidget *>(m_port), static_cast<QWidget *>(m_tls) }) {
        field->setEnabled(!existing && custom);
        field->setToolTip(existing ? tr("Add a new account to change the server, port or TLS mode.")
                : custom           ? QString {}
                                   : tr("Choose Custom server to edit this endpoint."));
    }
    m_networkHint->setText(existing ? tr("Add a new account to change the server, port or TLS mode.")
            : custom                ? tr("Enter the server address and port supplied by your IRC network.")
                     : tr("The network's TLS settings are filled in. Choose a nickname to connect."));
    m_label->setPlaceholderText(
        static_cast<Protocol>(m_protocol->currentData().toInt()) == Protocol::Irc && !custom
            ? m_ircPreset->currentText()
            : tr("Optional display name"));
}

void AccountDialog::refreshProtocolFields()
{
    const auto protocol = static_cast<Protocol>(m_protocol->currentData().toInt());
    const bool irc = protocol == Protocol::Irc;
    const bool telegram = protocol == Protocol::Telegram;
    const bool slack = protocol == Protocol::Slack;
    const bool existing = !m_account->currentData().toString().isEmpty();
    const bool bundled = bundledTelegramCredentialsAvailable();
    const bool customTelegram = m_customTelegramApplication->isChecked();
    const bool slackConfigured = bundledSlackApplicationAvailable();
    const bool customSlack = m_customSlackApplication->isChecked();
    const bool showProviderCredentials = telegram ? customTelegram : slack && customSlack;
    if (telegram && !existing && !m_telegramDefaultsApplied) {
        m_remember->setChecked(m_accounts.persistentSecretsAvailable());
        m_telegramDefaultsApplied = true;
    }
    if (slack && !existing && !m_slackDefaultsApplied) {
        m_remember->setChecked(m_accounts.persistentSecretsAvailable());
        m_slackDefaultsApplied = true;
    }
    m_ircNetwork->setVisible(irc);
    m_ircAuthentication->setVisible(irc);
    refreshIrcEndpointFields();
    m_cloudSettings->setVisible(!irc);
    m_cloudSettings->setTitle(telegram ? tr("Telegram sign-in") : tr("Slack sign-in"));
    m_customTelegramApplication->setVisible(telegram && bundled && !existing);
    m_customSlackApplication->setVisible(slack && (!existing || customSlack));
    m_providerIdLabel->setVisible(showProviderCredentials);
    m_providerId->setVisible(showProviderCredentials);
    m_cloudSecretLabel->setVisible(showProviderCredentials);
    m_cloudSecret->setVisible(showProviderCredentials);
    m_remember->setText(
        telegram ? tr("Remember this Telegram session") : tr("Remember in the system credential store"));
    m_providerIdLabel->setText(telegram ? tr("API &ID") : tr("Client &ID"));
    m_cloudSecretLabel->setText(telegram ? tr("API &hash") : tr("User &token (optional)"));
    m_providerId->setPlaceholderText(telegram ? QStringLiteral("123456") : QStringLiteral("1234.5678"));
    m_cloudSecret->setPlaceholderText(existing && m_remember->isChecked()
            ? tr("Leave blank to keep the saved credential")
            : telegram ? QString {}
                       : tr("Leave blank for browser sign-in"));
    if (telegram) {
        if (!m_accounts.protocolAvailable(protocol)) {
            m_cloudHint->setText(
                tr("Telegram requires the official TDLib runtime. "
                   "<a href='https://github.com/minibota/phoschat/blob/main/docs/telegram.md'>Installation "
                   "guide</a>"));
        } else if (!customTelegram && bundled) {
            m_cloudHint->setText(tr("Sign in with your phone number and the code Telegram sends you. "
                                    "Your two-step verification password may also be requested."));
        } else if (!customTelegram) {
            m_cloudHint->setText(tr("This build does not include the Telegram application credentials "
                                    "used by this account. Use a build configured for PhosChat Telegram "
                                    "sign-in."));
        } else {
            m_cloudHint->setText(bundled
                    ? tr("Enter your application's API ID and hash. Phone verification follows. "
                         "<a href='https://github.com/minibota/phoschat/blob/main/docs/telegram.md'>Advanced "
                         "setup</a>")
                    : tr("This build does not include PhosChat's Telegram application credentials. "
                         "Enter your own API ID and hash, then sign in with your phone number. "
                         "<a href='https://github.com/minibota/phoschat/blob/main/docs/telegram.md'>Build "
                         "and setup guide</a>"));
        }
    } else if (slack) {
        if (!customSlack && slackConfigured) {
            m_cloudHint->setText(tr("Connect in your browser and choose your workspace. Slack may ask "
                                    "your workspace administrator to approve PhosChat."));
        } else if (customSlack) {
            m_cloudHint->setText(tr("Enter the public Client ID supplied by your workspace administrator "
                                    "for browser sign-in, or a user token. Never enter a client secret. "
                                    "<a href='https://github.com/minibota/phoschat/blob/main/docs/slack.md'>"
                                    "Administrator setup</a>"));
        } else {
            m_cloudHint->setText(tr("Slack sign-in is not configured in this build. Your administrator "
                                    "can provide a configured build or a workspace app through Advanced. "
                                    "<a href='https://github.com/minibota/phoschat/blob/main/docs/slack.md'>"
                                    "Administrator setup</a>"));
        }
    }
    m_connectButton->setText(slack && !customSlack ? tr("Connect to Slack") : tr("Save && connect"));
    m_connectButton->setEnabled(m_accounts.protocolAvailable(protocol)
        && (!telegram || customTelegram || bundled) && (!slack || customSlack || slackConfigured));
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
    if (protocol == Protocol::Slack && !m_customSlackApplication->isChecked()
        && !bundledSlackApplicationAvailable()) {
        m_feedback->setText(tr("This build needs a Slack application configured by its distributor. "
                               "Use Advanced if your workspace administrator supplied an app or token."));
        m_feedback->show();
        return;
    }
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
        account.label = protocol == Protocol::Irc
            ? (m_ircPreset->currentData().toString().isEmpty() ? host : m_ircPreset->currentText())
            : protocolLabel(protocol);
    }
    if (protocol == Protocol::Irc) {
        account.host = host;
        account.port = static_cast<quint16>(m_port->value());
        account.tls = m_tls->isChecked();
        account.nick = nick;
        account.saslUser = saslUser;
    } else if (protocol == Protocol::Slack && !m_customSlackApplication->isChecked()) {
        account.providerId = m_account->currentData().toString().isEmpty() ? bundledSlackClientId()
                                                                           : m_providerId->text().trimmed();
    } else if (protocol != Protocol::Telegram || m_customTelegramApplication->isChecked()) {
        account.providerId = m_providerId->text().trimmed();
    }
    account.saveSecret = m_remember->isChecked();
    account.autoConnect = m_autoConnect->isChecked();

    auto password
        = protocol == Protocol::Irc ? m_password->text().toUtf8() : m_cloudSecret->text().trimmed().toUtf8();
    if ((protocol == Protocol::Telegram && !m_customTelegramApplication->isChecked())
        || (protocol == Protocol::Slack && !m_customSlackApplication->isChecked())) {
        password.fill('\0');
        password.clear();
    }
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
