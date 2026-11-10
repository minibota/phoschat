#pragma once

#include "models/types.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace phoschat {
class AccountManager;
}

namespace phoschat::ui {

class AccountDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AccountDialog(AccountManager &accounts, QWidget *parent = nullptr);
    void prefillIrcNetwork(const QString &label, const QString &host, quint16 port);

private:
    void refreshAccountList(const QString &selection);
    void loadSelected();
    void refreshProtocolFields();
    void refreshStatus();
    void save(bool connectAfter);
    void removeSelected();

    AccountManager &m_accounts;
    QString m_savingAccountId;
    bool m_telegramDefaultsApplied {};
    QComboBox *m_account {};
    QComboBox *m_protocol {};
    QGroupBox *m_ircNetwork {};
    QGroupBox *m_ircAuthentication {};
    QGroupBox *m_cloudSettings {};
    QLineEdit *m_label {};
    QLineEdit *m_host {};
    QSpinBox *m_port {};
    QLineEdit *m_nick {};
    QCheckBox *m_tls {};
    QLineEdit *m_saslUser {};
    QLineEdit *m_password {};
    QLineEdit *m_providerId {};
    QLineEdit *m_cloudSecret {};
    QCheckBox *m_customTelegramApplication {};
    QCheckBox *m_remember {};
    QCheckBox *m_autoConnect {};
    QLabel *m_networkHint {};
    QLabel *m_providerIdLabel {};
    QLabel *m_cloudSecretLabel {};
    QLabel *m_cloudHint {};
    QLabel *m_status {};
    QLabel *m_feedback {};
    QPushButton *m_disconnect {};
    QPushButton *m_remove {};
    QPushButton *m_connectButton {};
};

} // namespace phoschat::ui
