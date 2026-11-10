#include "core/account_manager.h"
#include "providers/irc/irc_directory.h"
#include "security/credential_store.h"
#include "storage/database.h"
#include "ui/settings/AccountDialog.hpp"
#include "ui/styles/Theme.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QtTest>

using namespace phoschat;
using namespace phoschat::ui;
using namespace Qt::StringLiterals;

class IrcAccountsTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { applyTheme(*qApp, Theme::Dark); }
    void presetsFillTlsAndPreserveIdentity();
    void customEndpointCanBeSaved();
    void discoveryPrefillAndExistingEndpointStayConsistent();
};

void IrcAccountsTest::presetsFillTlsAndPreserveIdentity()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    AccountDialog dialog(accounts);
    auto *preset = dialog.findChild<QComboBox *>(u"ircNetworkPreset"_s);
    auto *host = dialog.findChild<QLineEdit *>(u"ircServer"_s);
    auto *port = dialog.findChild<QSpinBox *>(u"ircPort"_s);
    auto *tls = dialog.findChild<QCheckBox *>(u"ircTls"_s);
    auto *nick = dialog.findChild<QLineEdit *>(u"ircNickname"_s);
    auto *name = dialog.findChild<QLineEdit *>(u"accountName"_s);
    QVERIFY(preset && host && port && tls && nick && name);
    QCOMPARE(preset->count(), static_cast<int>(irc::recommendedNetworks().size()) + 1);
    QCOMPARE(preset->currentText(), u"Libera.Chat"_s);
    QCOMPARE(host->text(), u"irc.libera.chat"_s);
    QVERIFY(!host->isEnabled() && !port->isEnabled() && !tls->isEnabled());
    QVERIFY(nick->isEnabled());
    nick->setText(u"PhosUser"_s);
    name->setText(u"My communities"_s);
    for (int index = 0; index < preset->count() - 1; ++index) {
        preset->setCurrentIndex(index);
        const auto &network = irc::recommendedNetworks()[index];
        QCOMPARE(host->text(), network.host.toString());
        QCOMPARE(port->value(), network.port);
        QVERIFY(tls->isChecked());
        QCOMPARE(nick->text(), u"PhosUser"_s);
        QCOMPARE(name->text(), u"My communities"_s);
    }
    name->clear();
    preset->setCurrentIndex(preset->findData(u"irc.oftc.net"_s));
    auto *save = dialog.findChild<QPushButton *>(u"saveAccount"_s);
    QVERIFY(save);
    save->click();
    QCOMPARE(accounts.accounts().size(), 1);
    const auto saved = accounts.accounts().first();
    QCOMPARE(saved.label, u"OFTC"_s);
    QCOMPARE(saved.host, u"irc.oftc.net"_s);
    QCOMPARE(saved.port, 6697);
    QVERIFY(saved.tls);
    QCOMPARE(saved.nick, u"PhosUser"_s);
    QCOMPARE(accounts.state(saved.id), ConnectionState::Disconnected);
    QVERIFY(!preset->isEnabled());
}

void IrcAccountsTest::customEndpointCanBeSaved()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    AccountDialog dialog(accounts);
    auto *preset = dialog.findChild<QComboBox *>(u"ircNetworkPreset"_s);
    auto *host = dialog.findChild<QLineEdit *>(u"ircServer"_s);
    auto *port = dialog.findChild<QSpinBox *>(u"ircPort"_s);
    auto *tls = dialog.findChild<QCheckBox *>(u"ircTls"_s);
    auto *nick = dialog.findChild<QLineEdit *>(u"ircNickname"_s);
    QVERIFY(preset && host && port && tls && nick);
    preset->setCurrentIndex(preset->count() - 1);
    QVERIFY(host->isEnabled() && port->isEnabled() && tls->isEnabled());
    QCOMPARE(host->text(), u"irc.libera.chat"_s);
    host->setText(u"irc.example.test"_s);
    tls->setChecked(false);
    QCOMPARE(port->value(), 6667);
    port->setValue(7000);
    nick->setText(u"CustomUser"_s);
    dialog.findChild<QPushButton *>(u"saveAccount"_s)->click();
    QCOMPARE(accounts.accounts().size(), 1);
    const auto saved = accounts.accounts().first();
    QCOMPARE(saved.label, u"irc.example.test"_s);
    QCOMPARE(saved.host, u"irc.example.test"_s);
    QCOMPARE(saved.port, 7000);
    QVERIFY(!saved.tls);
    QCOMPARE(saved.nick, u"CustomUser"_s);
    QVERIFY(!host->isEnabled() && !port->isEnabled() && !tls->isEnabled());
}

void IrcAccountsTest::discoveryPrefillAndExistingEndpointStayConsistent()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    Account existing;
    existing.id = u"existing"_s;
    existing.label = u"Personal server"_s;
    existing.host = u"irc.private.example.test"_s;
    existing.port = 7443;
    existing.nick = u"ExistingNick"_s;
    QVERIFY(database.saveAccount(existing));
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    AccountDialog dialog(accounts);
    auto *preset = dialog.findChild<QComboBox *>(u"ircNetworkPreset"_s);
    auto *host = dialog.findChild<QLineEdit *>(u"ircServer"_s);
    auto *port = dialog.findChild<QSpinBox *>(u"ircPort"_s);
    auto *name = dialog.findChild<QLineEdit *>(u"accountName"_s);
    QVERIFY(preset && host && port && name);
    QCOMPARE(preset->currentIndex(), preset->count() - 1);
    QVERIFY(!preset->isEnabled() && !host->isEnabled() && !port->isEnabled());
    QCOMPARE(host->text(), existing.host);
    QCOMPARE(port->value(), existing.port);

    dialog.prefillIrcNetwork(u"Debian community"_s, u"irc.oftc.net"_s, 6697);
    QCOMPARE(preset->currentText(), u"OFTC"_s);
    QVERIFY(preset->isEnabled() && !host->isEnabled());
    QCOMPARE(name->text(), u"Debian community"_s);
    dialog.prefillIrcNetwork(u"Regional relay"_s, u"relay.example.test"_s, 7443);
    QCOMPARE(preset->currentIndex(), preset->count() - 1);
    QVERIFY(host->isEnabled() && port->isEnabled());
    QCOMPARE(host->text(), u"relay.example.test"_s);
    QCOMPARE(port->value(), 7443);
    QCOMPARE(name->text(), u"Regional relay"_s);
}

QTEST_MAIN(IrcAccountsTest)
#include "test_irc_accounts.moc"
