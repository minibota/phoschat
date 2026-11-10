#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "security/credential_store.h"
#include "storage/database.h"
#include "ui/contacts/MainWindow.hpp"
#include "ui/styles/Theme.hpp"
#include "ui/windows/ChatWindow.hpp"
#include "ui/windows/WindowManager.hpp"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QListView>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
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

QTEST_MAIN(UiTest)
#include "test_ui.moc"
