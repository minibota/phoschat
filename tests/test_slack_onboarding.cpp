#include "core/account_manager.h"
#include "providers/slack/slack_app_config.h"
#include "security/credential_store.h"
#include "storage/database.h"
#include "ui/settings/AccountDialog.hpp"
#include "ui/styles/Theme.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace Qt::StringLiterals;
using namespace phoschat;
using namespace phoschat::ui;

namespace {

class OfflineTestProvider final : public IChatProvider {
public:
    explicit OfflineTestProvider(QObject *parent)
        : IChatProvider(parent)
    {
    }
    Capabilities capabilities() const override { return Capability::Text; }
    void connectAccount(const QByteArray &) override { emit stateChanged(ConnectionState::Connected); }
    void disconnectAccount() override { emit stateChanged(ConnectionState::Disconnected); }
    bool join(const QString &) override { return false; }
    bool part(const QString &) override { return false; }
    bool sendText(const ConversationId &, const QString &) override { return false; }
    bool sendCommand(const ConversationId &, const QString &) override { return false; }
    QString canonicalTarget(const QString &value) const override { return value; }
};

} // namespace

class SlackOnboardingTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void newAccountUsesConfiguredSignIn();
    void customAndTokenAccountsKeepTheirIdentity();

private:
    QTemporaryDir settingsDirectory_;
};

void SlackOnboardingTest::initTestCase()
{
    QVERIFY(settingsDirectory_.isValid());
    QApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(u"PhosChatTest"_s);
    QCoreApplication::setApplicationName(u"PhosChatSlackOnboardingTest"_s);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
    applyTheme(*qApp, Theme::Dark);
}

void SlackOnboardingTest::newAccountUsesConfiguredSignIn()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials, nullptr,
        [](const Account &, QObject *parent) { return new OfflineTestProvider(parent); });
    AccountDialog dialog(accounts);
    auto *protocol = dialog.findChild<QComboBox *>(u"protocolSelector"_s);
    auto *advanced = dialog.findChild<QCheckBox *>(u"customSlackApplication"_s);
    auto *application = dialog.findChild<QLineEdit *>(u"providerApplicationId"_s);
    auto *token = dialog.findChild<QLineEdit *>(u"providerCredential"_s);
    auto *connectButton = dialog.findChild<QPushButton *>(u"connectAccount"_s);
    auto *save = dialog.findChild<QPushButton *>(u"saveAccount"_s);
    QVERIFY(protocol && advanced && application && token && connectButton && save);
    protocol->setCurrentIndex(protocol->findData(static_cast<int>(Protocol::Slack)));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(advanced->isVisible());
    QVERIFY(!advanced->isChecked());
    QVERIFY(application->isHidden());
    QVERIFY(token->isHidden());
    QCOMPARE(connectButton->text(), u"Connect to Slack"_s);
    QCOMPARE(connectButton->isEnabled(), bundledSlackApplicationAvailable());
    const auto captureDirectory = qEnvironmentVariable("PHOSCHAT_UI_SCREENSHOT_DIR");
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(captureDirectory));
        QVERIFY(dialog.grab().save(captureDirectory
            + (bundledSlackApplicationAvailable() ? u"/slack-configured.png"_s
                                                  : u"/slack-unconfigured.png"_s)));
    }
    if (bundledSlackApplicationAvailable()) {
        // Hidden values from another provider must not override the public app identity.
        application->setText(u"999.999"_s);
        token->setText(u"must-not-be-saved"_s);
        QTest::mouseClick(connectButton, Qt::LeftButton);
        QCOMPARE(accounts.accounts().size(), 1);
        const auto saved = accounts.accounts().first();
        QCOMPARE(saved.providerId, bundledSlackClientId());
        QCOMPARE(accounts.state(saved.id), ConnectionState::Connected);
    } else {
        QTest::mouseClick(save, Qt::LeftButton);
        QVERIFY(accounts.accounts().isEmpty());
        advanced->setChecked(true);
        QVERIFY(application->isVisible());
        QVERIFY(token->isVisible());
        QVERIFY(connectButton->isEnabled());
        application->setText(u"678.901"_s);
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(accounts.accounts().size(), 1);
        QCOMPARE(accounts.accounts().first().providerId, u"678.901"_s);
    }
}

void SlackOnboardingTest::customAndTokenAccountsKeepTheirIdentity()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    Account custom;
    custom.id = u"custom"_s;
    custom.label = u"Custom app"_s;
    custom.protocol = Protocol::Slack;
    custom.providerId = bundledSlackClientId() == u"678.901" ? u"987.654"_s : u"678.901"_s;
    QVERIFY(accounts.saveAccount(custom));
    Account tokenAccount;
    tokenAccount.id = u"manual"_s;
    tokenAccount.label = u"Manual token"_s;
    tokenAccount.protocol = Protocol::Slack;
    QVERIFY(accounts.saveAccount(tokenAccount, QByteArrayLiteral("xoxp-offline-test-only")));
    AccountDialog dialog(accounts);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *selector = dialog.findChild<QComboBox *>(u"accountSelector"_s);
    auto *advanced = dialog.findChild<QCheckBox *>(u"customSlackApplication"_s);
    auto *application = dialog.findChild<QLineEdit *>(u"providerApplicationId"_s);
    auto *save = dialog.findChild<QPushButton *>(u"saveAccount"_s);
    QVERIFY(selector && advanced && application && save);
    for (const auto &account : { custom, tokenAccount }) {
        selector->setCurrentIndex(selector->findData(account.id));
        QVERIFY(advanced->isChecked());
        QVERIFY(!advanced->isEnabled());
        QVERIFY(!application->isEnabled());
        QCOMPARE(application->text(), account.providerId);
        QTest::mouseClick(save, Qt::LeftButton);
        const auto saved = database.accounts();
        const auto found = std::ranges::find(saved, account.id, &Account::id);
        QVERIFY(found != saved.end());
        QCOMPARE(found->providerId, account.providerId);
    }
}

QTEST_MAIN(SlackOnboardingTest)
#include "test_slack_onboarding.moc"
