#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "security/credential_store.h"
#include "security/telegram_app_credentials.h"
#include "storage/database.h"
#include "ui/contacts/MainWindow.hpp"
#include "ui/conversations/ConversationController.hpp"
#include "ui/settings/AccountDialog.hpp"
#include "ui/styles/Theme.hpp"
#include "ui/windows/ChatWindow.hpp"
#include "ui/windows/WindowManager.hpp"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest>
#include <algorithm>

using namespace Qt::StringLiterals;
using namespace phoschat;
using namespace phoschat::ui;

namespace {

QList<ChatWindow *> visibleChats()
{
    QList<ChatWindow *> chats;
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (auto *chat = qobject_cast<ChatWindow *>(widget); chat && chat->isVisible()) {
            chats.append(chat);
        }
    }
    return chats;
}

} // namespace

class UiTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void independentWindowsRestoreAndReuse();
    void mainGeometryWhileHiddenToTray();
    void searchAndConversationFocusChrome_data();
    void searchAndConversationFocusChrome();
    void telegramOnboardingUsesBuildCredentials();
    void existingTelegramApplicationIdentityIsPreserved();
    void chatHeaderShowsClickableAvatarAndContactPresence();

private:
    QTemporaryDir settingsDirectory_;
};

void UiTest::initTestCase()
{
    QVERIFY(settingsDirectory_.isValid());
    QApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(u"PhosChatTest"_s);
    QCoreApplication::setApplicationName(u"PhosChatUiTest"_s);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
    QCOMPARE(preferredTheme(), Theme::Dark);
    applyTheme(*qApp, preferredTheme());
}

void UiTest::independentWindowsRestoreAndReuse()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    Account account;
    account.id = u"example"_s;
    account.host = u"irc.example.test"_s;
    account.nick = u"phoschat"_s;
    QVERIFY(database.saveAccount(account));

    Conversation first;
    first.id = { account.id, u"#first"_s };
    first.title = u"#first"_s;
    QVERIFY(database.saveConversation(first));
    Conversation second;
    second.id = { account.id, u"#second"_s };
    second.title = u"#second"_s;
    QVERIFY(database.saveConversation(second));

    const Message message { first.id, u"one"_s, u"River"_s,
        u"<b>Literal message</b> · Independent windows keep the project room open while a colleague sends an update to the team."_s,
        QDateTime::currentDateTimeUtc() };
    QCOMPARE(database.appendMessage(message, first), Database::AppendResult::Inserted);

    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    const QSize firstSize(455, 365);

    {
        WindowManager windows(accounts, conversations, messages);
        windows.open(first.id);
        QCOMPARE(visibleChats().size(), 1);
        const auto firstWindow = visibleChats().front();
        auto *history = firstWindow->findChild<QListView *>();
        QVERIFY(history);
        QCOMPARE(history->model()->rowCount(), 1);
        QCOMPARE(history->model()->index(0, 0).data().toString(), message.text);
        const auto darkText = history->palette().color(QPalette::Text);
        {
            MainWindow main(accounts, conversations, windows);
            const auto actions = main.findChildren<QAction *>();
            const auto light = std::ranges::find_if(
                actions, [](const auto *action) { return action->text() == u"Light"; });
            const auto dark
                = std::ranges::find_if(actions, [](const auto *action) { return action->text() == u"Dark"; });
            QVERIFY(light != actions.end() && dark != actions.end());
            QVERIFY((*dark)->isChecked());
            (*light)->trigger();
            QVERIFY((*light)->isChecked());
            QCOMPARE(preferredTheme(), Theme::Light);
            QVERIFY(history->palette().color(QPalette::Text) != darkText);
            QCOMPARE(visibleChats().size(), 1);
            (*dark)->trigger();
            QCOMPARE(preferredTheme(), Theme::Dark);
            QCOMPARE(history->palette().color(QPalette::Text), darkText);
        }
        const auto row = history->model()->index(0, 0);
        const int wideRowHeight = history->visualRect(row).height();
        QVERIFY(wideRowHeight > 0);
        firstWindow->resize(340, 490);
        QTRY_VERIFY(history->visualRect(row).height() > wideRowHeight);

        windows.open(second.id);
        QCOMPARE(visibleChats().size(), 2);
        windows.open(first.id);
        QCOMPARE(visibleChats().size(), 2);
        QVERIFY(visibleChats().contains(firstWindow));

        auto *composer = firstWindow->findChild<QLineEdit *>();
        QVERIFY(composer);
        composer->setText(u"An unsent draft"_s);
        firstWindow->resize(firstSize);
        firstWindow->close();
        QCOMPARE(visibleChats().size(), 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        windows.open(first.id);
        QCOMPARE(visibleChats().size(), 2);
        const auto chats = visibleChats();
        const auto reopened = std::ranges::find_if(
            chats, [](const ChatWindow *chat) { return chat->windowTitle().startsWith(u"#first"_s); });
        QVERIFY(reopened != chats.end());
        QCOMPARE((*reopened)->size(), firstSize);
        QCOMPARE((*reopened)->findChild<QLineEdit *>()->text(), u"An unsent draft"_s);
        windows.closeAll();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    WindowManager restored(accounts, conversations, messages);
    restored.restoreSession();
    const auto chats = visibleChats();
    QCOMPARE(chats.size(), 2);
    const auto reopened = std::ranges::find_if(
        chats, [](const ChatWindow *chat) { return chat->windowTitle().startsWith(u"#first"_s); });
    QVERIFY(reopened != chats.end());
    QCOMPARE((*reopened)->size(), firstSize);
    QCOMPARE((*reopened)->findChild<QListView *>()->model()->rowCount(), 1);
    restored.closeAll();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void UiTest::mainGeometryWhileHiddenToTray()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    const QSize expected(342, 530);
    {
        MainWindow main(accounts, conversations, windows);
        main.setTrayAvailable(true);
        main.resize(expected);
        main.show();
        QSignalSpy hidden(&main, &MainWindow::hiddenToTray);
        QSignalSpy quit(&main, &MainWindow::quitRequested);
        main.close();
        QVERIFY(main.isHidden());
        QCOMPARE(hidden.size(), 1);
        QCOMPARE(quit.size(), 0);
        main.prepareToQuit();
        main.show();
        QVERIFY(main.close());
        QCOMPARE(hidden.size(), 1);
        QCOMPARE(quit.size(), 0);
    }
    MainWindow restored(accounts, conversations, windows);
    QCOMPARE(restored.size(), expected);
}

void UiTest::searchAndConversationFocusChrome_data()
{
    QTest::addColumn<bool>("lightTheme");
    QTest::newRow("dark") << false;
    QTest::newRow("light") << true;
}

void UiTest::searchAndConversationFocusChrome()
{
    QFETCH(bool, lightTheme);
    applyTheme(*qApp, lightTheme ? Theme::Light : Theme::Dark);
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    Account account;
    account.id = u"chrome"_s;
    account.protocol = Protocol::Telegram;
    account.label = u"Telegram"_s;
    QVERIFY(database.saveAccount(account));
    Conversation group;
    group.id = { account.id, u"-10042"_s };
    group.title = u"PhosChat group"_s;
    group.kind = ConversationKind::Group;
    QVERIFY(database.saveConversation(group));

    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    MainWindow main(accounts, conversations, windows);
    main.show();
    auto *search = main.findChild<QLineEdit *>();
    auto *searchButton = main.findChild<QToolButton *>(u"conversationSearchButton"_s);
    auto *tree = main.findChild<QTreeWidget *>();
    QVERIFY(search && searchButton && tree);
    QVERIFY(!search->isVisible());
    QTest::mouseClick(searchButton, Qt::LeftButton);
    QTRY_VERIFY(search->isVisible());
    search->setText(u"Phos"_s);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->childCount(), 1);

    const auto buttons = search->findChildren<QToolButton *>();
    QCOMPARE(buttons.size(), 1); // The magnifier is next to the new-conversation button.
    for (const auto *button : buttons) {
        QTRY_VERIFY(button->isVisible());
        QVERIFY(search->rect().contains(button->geometry()));
        QVERIFY(qAbs(button->geometry().center().y() - search->rect().center().y()) <= 1);
    }
    auto *item = tree->topLevelItem(0)->child(0);
    QCOMPARE(item->icon(0).pixmap(18, 18).toImage(), conversationIcon(true).pixmap(18, 18).toImage());
    tree->setCurrentItem(item);
    tree->setFocus();
    QTRY_VERIFY(tree->hasFocus());
    QCOMPARE(tree->frameWidth(), 0);
    QVERIFY(item->isSelected());
    search->setFocus();
    QTRY_VERIFY(search->hasFocus());
    QCOMPARE(tree->frameWidth(), 0);
    QVERIFY(item->isSelected());
    QTest::keyClick(search, Qt::Key_Escape);
    QTRY_VERIFY(!search->isVisible());
    QVERIFY(search->text().isEmpty());
    QVERIFY(!searchButton->isChecked());
    applyTheme(*qApp, Theme::Dark);
}

void UiTest::telegramOnboardingUsesBuildCredentials()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    AccountDialog dialog(accounts);
    dialog.show();
    auto *protocol = dialog.findChild<QComboBox *>(u"protocolSelector"_s);
    auto *applicationId = dialog.findChild<QLineEdit *>(u"providerApplicationId"_s);
    auto *credential = dialog.findChild<QLineEdit *>(u"providerCredential"_s);
    auto *custom = dialog.findChild<QCheckBox *>(u"customTelegramApplication"_s);
    auto *remember = dialog.findChild<QCheckBox *>(u"rememberSession"_s);
    auto *save = dialog.findChild<QPushButton *>(u"saveAccount"_s);
    QVERIFY(protocol && applicationId && credential && custom && remember && save);
    protocol->setCurrentIndex(protocol->findData(static_cast<int>(Protocol::Telegram)));
    const bool bundled = bundledTelegramCredentialsAvailable();
    QCOMPARE(applicationId->isVisible(), !bundled);
    QCOMPARE(credential->isVisible(), !bundled);
    QCOMPARE(custom->isVisible(), bundled);
    QCOMPARE(remember->isChecked(), accounts.persistentSecretsAvailable());
    QCOMPARE(remember->text(), u"Remember this Telegram session"_s);
    remember->setChecked(false); // Exercise a session-only account without touching the desktop keychain.
    if (bundled) {
        custom->setChecked(true);
        QVERIFY(applicationId->isVisible());
        QVERIFY(credential->isVisible());
        QVERIFY(!remember->isChecked());
        applicationId->setText(u"123456"_s);
        credential->setText(u"0123456789abcdef0123456789abcdef"_s);
        custom->setChecked(false);
        QVERIFY(!applicationId->isVisible());
        QVERIFY(!credential->isVisible());
        QVERIFY(!remember->isChecked());
    } else {
        applicationId->setText(u"123456"_s);
        credential->setText(u"0123456789abcdef0123456789abcdef"_s);
    }
    save->click();
    QCOMPARE(accounts.accounts().size(), 1);
    const auto account = accounts.accounts().first();
    QCOMPARE(account.protocol, Protocol::Telegram);
    QCOMPARE(account.providerId, bundled ? QString {} : u"123456"_s);
    QVERIFY(!account.saveSecret);
    QVERIFY(!applicationId->isEnabled());
    QVERIFY(custom->isHidden());
}

void UiTest::existingTelegramApplicationIdentityIsPreserved()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    Account account;
    account.id = u"manual-telegram"_s;
    account.label = u"My Telegram"_s;
    account.protocol = Protocol::Telegram;
    account.providerId = u"123456"_s;
    QVERIFY(database.saveAccount(account));
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    AccountDialog dialog(accounts);
    dialog.show();
    auto *applicationId = dialog.findChild<QLineEdit *>(u"providerApplicationId"_s);
    auto *credential = dialog.findChild<QLineEdit *>(u"providerCredential"_s);
    auto *custom = dialog.findChild<QCheckBox *>(u"customTelegramApplication"_s);
    auto *remember = dialog.findChild<QCheckBox *>(u"rememberSession"_s);
    QVERIFY(applicationId && credential && custom && remember);
    QVERIFY(applicationId->isVisible());
    QVERIFY(credential->isVisible());
    QCOMPARE(applicationId->text(), account.providerId);
    QVERIFY(!applicationId->isEnabled());
    QVERIFY(custom->isChecked());
    QVERIFY(custom->isHidden());
    QVERIFY(!remember->isChecked());
}

void UiTest::chatHeaderShowsClickableAvatarAndContactPresence()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    Conversation value;
    value.id = { u"telegram"_s, u"42/7"_s };
    value.title = u"Contact & Co"_s;
    value.kind = ConversationKind::Direct;
    QImage image(24, 24, QImage::Format_RGB32);
    image.fill(QColor(Qt::red));
    QBuffer jpeg(&value.avatar);
    QVERIFY(jpeg.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&jpeg, "JPEG"));
    const auto rendered = photoAvatar(value.avatar, 32, 2.0);
    QCOMPARE(rendered.size(), QSize(64, 64));
    QCOMPARE(rendered.devicePixelRatio(), 2.0);
    const auto circular = rendered.toImage();
    QCOMPARE(circular.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(circular.pixelColor(32, 32).alpha(), 255);
    QVERIFY(circular.pixelColor(32, 32).red() > 220);

    ConversationController controller(value.id, messages);
    ChatWindow window(controller);
    window.refresh(value, u"My account"_s, ConnectionState::Connected, {}, Protocol::Telegram);
    window.show();
    auto *photo = window.findChild<QToolButton *>(u"contactProfilePhoto"_s);
    auto *name = window.findChild<QToolButton *>(u"contactProfileName"_s);
    auto *status = window.findChild<QLabel *>(u"contactPresenceIndicator"_s);
    QVERIFY(photo && name && status);
    QVERIFY(photo->isVisible() && !photo->icon().isNull());
    QCOMPARE(name->text(), u"Contact && Co"_s);
    QVERIFY(status->text().isEmpty());
    QVERIFY(status->isHidden());
    QSignalSpy opened(&window, &ChatWindow::profileRequested);
    name->click();
    photo->click();
    QCOMPARE(opened.size(), 2);

    value.presence = Presence::Online;
    window.refresh(value, u"My account"_s, ConnectionState::Connected, {}, Protocol::Telegram);
    QVERIFY(status->isVisible());
    QCOMPARE(status->accessibleName(), u"Contact is online"_s);
    const auto green = status->styleSheet();
    value.presence = Presence::Away;
    window.refresh(value, u"My account"_s, ConnectionState::Connected, {}, Protocol::Telegram);
    QCOMPARE(status->accessibleName(), u"Contact is away"_s);
    const auto yellow = status->styleSheet();
    QVERIFY(green != yellow);
    value.presence = Presence::Offline;
    window.refresh(value, u"My account"_s, ConnectionState::Connected, {}, Protocol::Telegram);
    QCOMPARE(status->accessibleName(), u"Contact is offline"_s);
    QVERIFY(status->styleSheet() != green && status->styleSheet() != yellow);

    window.refresh(value, u"My account"_s, ConnectionState::Reconnecting, {}, Protocol::Telegram);
    QVERIFY(status->isHidden());
    QVERIFY(status->accessibleName().isEmpty());
    value.kind = ConversationKind::Group;
    value.presence = Presence::Online;
    window.refresh(value, u"My account"_s, ConnectionState::Connected, {}, Protocol::Telegram);
    QVERIFY(status->isHidden());
}

QTEST_MAIN(UiTest)
#include "test_ui.moc"
