#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "security/credential_store.h"
#include "storage/database.h"
#include "ui/contacts/MainWindow.hpp"
#include "ui/styles/Theme.hpp"
#include "ui/windows/WindowManager.hpp"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest>

using namespace phoschat;
using namespace phoschat::ui;
using namespace Qt::StringLiterals;

namespace {
class PreviewProvider final : public IChatProvider {
public:
    using IChatProvider::IChatProvider;
    Capabilities capabilities() const override { return Capability::Text; }
    void connectAccount(const QByteArray &) override { emit stateChanged(ConnectionState::Connected); }
    void disconnectAccount() override { emit stateChanged(ConnectionState::Disconnected); }
    bool join(const QString &) override { return false; }
    bool part(const QString &) override { return false; }
    bool sendText(const ConversationId &, const QString &) override { return false; }
    bool sendCommand(const ConversationId &, const QString &) override { return false; }
    QString canonicalTarget(const QString &target) const override { return target; }
};

void seed(Database &database)
{
    const QList<Protocol> protocols { Protocol::Telegram, Protocol::Irc, Protocol::Slack };
    for (int i = 0; i < protocols.size(); ++i) {
        Account account;
        account.id = QString::number(i);
        account.label = QStringList { u"Telegram"_s, u"Libera Chat"_s, u"Work"_s }.at(i);
        account.protocol = protocols.at(i);
        account.host = u"irc.example.test"_s;
        account.nick = u"Preview"_s;
        QVERIFY(database.saveAccount(account));
        const int count = i == 0 ? 150 : 2;
        for (int row = 0; row < count; ++row) {
            Conversation conversation;
            conversation.id = { account.id, QString::number(row) };
            conversation.title
                = i == 0 ? u"Group %1"_s.arg(row, 3, 10, QLatin1Char('0')) : u"Conversation %1"_s.arg(row);
            conversation.kind = i == 0 ? ConversationKind::Group : ConversationKind::Channel;
            QVERIFY(database.saveConversation(conversation));
        }
    }
}

QToolButton *header(MainWindow &main, const QString &key)
{
    return main.findChild<QToolButton *>(u"accountHeader/"_s + key);
}
}

class AccountOverviewTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void accountHeadersRemainVisible_data();
    void accountHeadersRemainVisible();
    void searchAndRefreshKeepTheOpenAccount();
    void accountSummaryTracksAllStates();
    void emptyAccountsAndSearchKeepGuidance();
    void incomingUnreadMessagesMoveToTopAndReturnWhenRead();
    void recentSectionsKeepTheirOrderWithOlderUnreadMessages();

private:
    QTemporaryDir settingsDirectory_;
};

void AccountOverviewTest::initTestCase()
{
    QVERIFY(settingsDirectory_.isValid());
    QApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(u"PhosChatTest"_s);
    QCoreApplication::setApplicationName(u"AccountOverviewTest"_s);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
}

void AccountOverviewTest::init()
{
    QSettings().clear();
    applyTheme(*qApp, Theme::Dark);
}

void AccountOverviewTest::accountHeadersRemainVisible_data()
{
    QTest::addColumn<bool>("light");
    QTest::newRow("dark") << false;
    QTest::newRow("light") << true;
}

void AccountOverviewTest::accountHeadersRemainVisible()
{
    QFETCH(bool, light);
    applyTheme(*qApp, light ? Theme::Light : Theme::Dark);
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    seed(database);
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    MainWindow main(accounts, conversations, windows);
    main.resize(328, 620);
    main.show();
    auto *telegram = header(main, u"account/0"_s);
    auto *irc = header(main, u"account/1"_s);
    auto *slack = header(main, u"account/2"_s);
    QVERIFY(telegram && irc && slack);
    QTest::mouseClick(telegram, Qt::LeftButton);
    auto *tree = main.findChild<QTreeWidget *>();
    auto *panel = main.findChild<QScrollArea *>(u"conversationAccordion"_s);
    QVERIFY(tree && panel);
    QTRY_VERIFY(telegram->isChecked());
    QCoreApplication::processEvents();
    QVERIFY(!irc->isChecked() && !slack->isChecked());
    QCOMPARE(tree->rootIndex().data().toString(), u"Telegram"_s);
    QVERIFY(tree->verticalScrollBar()->maximum() > 0);
    for (auto *button : { telegram, irc, slack }) {
        const auto rectangle = QRect(button->mapTo(panel->viewport(), QPoint {}), button->size());
        QVERIFY2(panel->viewport()->rect().contains(rectangle), qPrintable(button->text()));
    }
    QCOMPARE(panel->verticalScrollBar()->maximum(), 0);
    QVERIFY(telegram->geometry().bottom() < tree->geometry().top());
    QVERIFY(tree->geometry().bottom() < slack->geometry().top());
    const auto captureDirectory = qEnvironmentVariable("PHOSCHAT_UI_SCREENSHOT_DIR");
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(captureDirectory));
        QVERIFY(main.grab().save(
            captureDirectory + (light ? u"/accordion-light.png"_s : u"/accordion-dark.png"_s)));
    }
    tree->verticalScrollBar()->setValue(20);
    const auto scroll = tree->verticalScrollBar()->value();
    QTest::mouseClick(irc, Qt::LeftButton);
    QVERIFY(irc->isChecked());
    QVERIFY(!telegram->isChecked());
    QTest::mouseClick(telegram, Qt::LeftButton);
    QCOMPARE(tree->verticalScrollBar()->value(), scroll);
    QTest::mouseClick(telegram, Qt::LeftButton);
    QVERIFY(!tree->isVisible());
    QVERIFY(!telegram->isChecked() && !irc->isChecked() && !slack->isChecked());
    main.activateWindow();
    slack->setFocus();
    QTRY_VERIFY(slack->hasFocus());
    QTest::keyClick(slack, Qt::Key_Right);
    QVERIFY(slack->isChecked());
    QTest::keyClick(slack, Qt::Key_Left);
    QVERIFY(!tree->isVisible());
}

void AccountOverviewTest::searchAndRefreshKeepTheOpenAccount()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    seed(database);
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    {
        MainWindow main(accounts, conversations, windows);
        main.show();
        QTest::mouseClick(header(main, u"account/0"_s), Qt::LeftButton);
        auto *tree = main.findChild<QTreeWidget *>();
        auto *search = main.findChild<QLineEdit *>();
        QVERIFY(header(main, u"account/0"_s)->isChecked());
        search->setText(u"Group"_s);
        tree->verticalScrollBar()->setValue(20);
        const auto searchScroll = tree->verticalScrollBar()->value();
        conversations.setMuted({ u"0"_s, u"1"_s }, true);
        QTest::qWait(60);
        QCOMPARE(tree->verticalScrollBar()->value(), searchScroll);
        search->setText(u"Libera"_s);
        QVERIFY(header(main, u"account/1"_s)->isChecked());
        QVERIFY(!header(main, u"account/0"_s)->isEnabled());
        search->clear();
        QVERIFY(header(main, u"account/0"_s)->isChecked());
        auto *item = tree->topLevelItem(1)->child(0);
        QVERIFY(item);
        conversations.setFavorite({ u"0"_s, u"0"_s }, true);
        QTRY_VERIFY(header(main, u"favorites"_s));
        QVERIFY(header(main, u"account/0"_s)->isChecked());
        // QLayout queues the show of newly inserted child widgets. A header can
        // exist before that event runs after the asynchronous section rebuild.
        QTRY_VERIFY(header(main, u"account/1"_s)->isVisible());
        QTRY_VERIFY(header(main, u"account/2"_s)->isVisible());
    }
    MainWindow reopened(accounts, conversations, windows);
    reopened.show();
    QVERIFY(header(reopened, u"account/0"_s)->isChecked());
    QTest::mouseClick(header(reopened, u"account/0"_s), Qt::LeftButton);
    QVERIFY(!reopened.findChild<QTreeWidget *>()->isVisible());
}

void AccountOverviewTest::accountSummaryTracksAllStates()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    seed(database);
    CredentialStore credentials;
    AccountManager accounts(database, credentials, nullptr,
        [](const Account &, QObject *parent) { return new PreviewProvider(parent); });
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    MainWindow main(accounts, conversations, windows);
    main.show();
    auto *title = main.findChild<QLabel *>(u"accountSummaryTitle"_s);
    auto *state = main.findChild<QLabel *>(u"accountSummaryState"_s);
    QVERIFY(title && state);
    QCOMPARE(title->text(), u"3 accounts"_s);
    QCOMPARE(state->text(), u"0 online · 3 offline"_s);
    accounts.connectAccount(u"1"_s);
    QCOMPARE(state->text(), u"1 online · 2 offline"_s);
    auto *provider = accounts.provider(u"1"_s);
    QVERIFY(provider);
    provider->stateChanged(ConnectionState::Reconnecting);
    QCOMPARE(state->text(), u"0 online · 2 offline · 1 connecting"_s);
    QVERIFY(state->toolTip().contains(u"Libera Chat"_s));
    QVERIFY(state->toolTip().contains(u"Telegram"_s));
    QVERIFY(state->wordWrap());
    provider->stateChanged(ConnectionState::Error);
    QCOMPARE(state->text(), u"0 online · 3 offline"_s);
    accounts.removeAccount(u"2"_s);
    QCOMPARE(title->text(), u"2 accounts"_s);
    QCOMPARE(state->text(), u"0 online · 2 offline"_s);
}

void AccountOverviewTest::emptyAccountsAndSearchKeepGuidance()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    seed(database);
    Account empty;
    empty.id = u"empty"_s;
    empty.label = u"Personal"_s;
    empty.protocol = Protocol::Telegram;
    QVERIFY(database.saveAccount(empty));
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    MainWindow main(accounts, conversations, windows);
    main.show();
    QTest::mouseClick(header(main, u"account/empty"_s), Qt::LeftButton);
    auto *tree = main.findChild<QTreeWidget *>();
    QVERIFY(!tree->isVisible());
    auto visibleText = [&main](const QString &text) {
        for (const auto *label : main.findChildren<QLabel *>()) {
            if (label->isVisible() && label->text() == text)
                return true;
        }
        return false;
    };
    QVERIFY(visibleText(u"No conversations yet"_s));
    main.resize(282, 440);
    QTest::qWait(20);
    auto *panel = main.findChild<QScrollArea *>(u"conversationAccordion"_s);
    QVERIFY(panel);
    if (!qEnvironmentVariable("PHOSCHAT_UI_SCREENSHOT_DIR").isEmpty())
        main.grab().save(qEnvironmentVariable("PHOSCHAT_UI_SCREENSHOT_DIR") + u"/accordion-minimum.png"_s);
    for (const auto &key : { u"account/0"_s, u"account/1"_s, u"account/2"_s, u"account/empty"_s }) {
        auto *button = header(main, key);
        // Native font metrics vary. When the minimum window cannot fit every
        // header, the outer scroll area must still make each one reachable.
        panel->ensureWidgetVisible(button, 0, 0);
        const auto rectangle = QRect(button->mapTo(panel->viewport(), QPoint {}), button->size());
        QVERIFY2(panel->viewport()->rect().contains(rectangle), qPrintable(key));
    }
    auto *search = main.findChild<QLineEdit *>();
    search->setText(u"does not exist"_s);
    QVERIFY(visibleText(u"No matching conversations"_s));
    QTRY_VERIFY(header(main, u"account/0"_s)->isVisible());
    QTRY_VERIFY(header(main, u"account/1"_s)->isVisible());
    QPushButton *clear = nullptr;
    for (auto *button : main.findChildren<QPushButton *>()) {
        if (button->isVisible() && button->text() == u"Clear search"_s)
            clear = button;
    }
    QVERIFY(clear);
    QTest::mouseClick(clear, Qt::LeftButton);
    QVERIFY(search->text().isEmpty());
    QVERIFY(header(main, u"account/empty"_s)->isChecked());
    QVERIFY(visibleText(u"No conversations yet"_s));
}

void AccountOverviewTest::incomingUnreadMessagesMoveToTopAndReturnWhenRead()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    Account account;
    account.id = u"unread-account"_s;
    account.label = u"Unread test"_s;
    account.host = u"irc.example.test"_s;
    account.nick = u"Preview"_s;
    QVERIFY(database.saveAccount(account));
    for (const auto &name :
        { u"Alpha"_s, u"Beta"_s, u"History"_s, u"Outgoing"_s, u"Zulu"_s, u"Favorite A"_s, u"Favorite Z"_s }) {
        Conversation conversation;
        conversation.id = { account.id, name };
        conversation.title = name;
        conversation.favorite = name.startsWith(u"Favorite");
        QVERIFY(database.saveConversation(conversation));
    }
    CredentialStore credentials;
    AccountManager accounts(database, credentials, nullptr,
        [](const Account &, QObject *parent) { return new PreviewProvider(parent); });
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    MainWindow main(accounts, conversations, windows);
    main.show();
    accounts.connectAccount(account.id);
    auto *provider = accounts.provider(account.id);
    QVERIFY(provider);
    auto *tree = main.findChild<QTreeWidget *>();
    QVERIFY(tree);
    const auto section = [tree](const QString &key) -> QTreeWidgetItem * {
        for (int index = 0; index < tree->topLevelItemCount(); ++index) {
            auto *item = tree->topLevelItem(index);
            if (item->data(0, Qt::UserRole + 3).toString() == key)
                return item;
        }
        return nullptr;
    };
    const auto children = [&section](const QString &key) {
        QStringList ids;
        if (const auto *group = section(key)) {
            for (int index = 0; index < group->childCount(); ++index)
                ids.append(group->child(index)->data(0, Qt::UserRole + 2).toString());
        }
        return ids;
    };
    const auto accountKey = u"account/"_s + account.id;
    QVERIFY(section(accountKey));
    QTest::mouseClick(header(main, accountKey), Qt::LeftButton);
    tree->setCurrentItem(section(accountKey)->child(0));
    QCOMPARE(tree->currentItem()->text(0), u"Alpha"_s);
    const auto now = QDateTime::currentDateTimeUtc();
    const Message older { { account.id, u"Beta"_s }, u"older"_s, u"Peer"_s, u"Earlier unread"_s,
        now.addSecs(-10) };
    const Message newest { { account.id, u"Zulu"_s }, u"newest"_s, u"Peer"_s, u"Latest unread"_s, now };
    provider->messageReceived(older);
    provider->messageReceived(newest);
    QTRY_COMPARE(children(accountKey),
        (QStringList { u"Zulu"_s, u"Beta"_s, u"Alpha"_s, u"History"_s, u"Outgoing"_s }));
    QVERIFY(section(accountKey)->child(0)->font(0).bold());
    QCOMPARE(section(accountKey)->child(0)->text(2), u"1"_s);
    QCOMPARE(tree->currentItem()->text(0), u"Alpha"_s);
    QVERIFY(header(main, accountKey)->isChecked());

    // Replays, fetched history and our own sends do not create new unread chats.
    provider->messageReceived(newest);
    provider->messageReceived(Message { { account.id, u"History"_s }, u"history"_s, u"Peer"_s,
        u"Fetched history"_s, now.addSecs(10), false, false, true });
    provider->messageReceived(Message { { account.id, u"Outgoing"_s }, u"outgoing"_s, u"Preview"_s,
        u"Our own message"_s, now.addSecs(20), true });
    QCOMPARE(conversations.conversation(newest.conversation)->unread, 1);
    QCOMPARE(conversations.conversation({ account.id, u"History"_s })->unread, 0);
    QCOMPARE(conversations.conversation({ account.id, u"Outgoing"_s })->unread, 0);
    conversations.markRead(newest.conversation);
    QTRY_COMPARE(children(accountKey),
        (QStringList { u"Beta"_s, u"Alpha"_s, u"History"_s, u"Outgoing"_s, u"Zulu"_s }));
    QCOMPARE(section(accountKey)->child(4)->text(2), QString {});
    QVERIFY(!section(accountKey)->child(4)->font(0).bold());

    provider->messageReceived(Message {
        { account.id, u"Favorite Z"_s }, u"favorite"_s, u"Peer"_s, u"Favorite unread"_s, now.addSecs(-20) });
    QTRY_COMPARE(children(u"favorites"_s), (QStringList { u"Favorite Z"_s, u"Favorite A"_s }));
    QCOMPARE(tree->topLevelItem(0)->data(0, Qt::UserRole + 3).toString(), u"favorites"_s);
    QVERIFY(header(main, accountKey)->isChecked());
    conversations.markRead(older.conversation);
    QTRY_COMPARE(children(accountKey),
        (QStringList { u"Alpha"_s, u"Beta"_s, u"History"_s, u"Outgoing"_s, u"Zulu"_s }));
}

void AccountOverviewTest::recentSectionsKeepTheirOrderWithOlderUnreadMessages()
{
    QSettings().setValue(u"ui/grouping"_s, 3);
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    Account account;
    account.id = u"recent-account"_s;
    account.label = u"Recent test"_s;
    QVERIFY(database.saveAccount(account));
    const auto now = QDateTime::currentDateTimeUtc();
    const auto addConversation
        = [&database, &account](const QString &id, const QDateTime &updated, int unread = 0) {
              Conversation conversation;
              conversation.id = { account.id, id };
              conversation.title = id;
              conversation.kind = ConversationKind::Direct;
              conversation.updated = updated;
              conversation.unread = unread;
              QVERIFY(database.saveConversation(conversation));
          };
    addConversation(u"today"_s, now);
    addConversation(u"recent-read"_s, now.addDays(-2));
    addConversation(u"unread"_s, now.addDays(-3), 1);
    addConversation(u"old-read"_s, now.addDays(-4));
    addConversation(u"none"_s, {});
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    MainWindow main(accounts, conversations, windows);
    auto *tree = main.findChild<QTreeWidget *>();
    QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 3);
    QCOMPARE(tree->topLevelItem(0)->data(0, Qt::UserRole + 3).toString(), u"recent/today"_s);
    QCOMPARE(tree->topLevelItem(1)->data(0, Qt::UserRole + 3).toString(), u"recent/earlier"_s);
    QCOMPARE(tree->topLevelItem(2)->data(0, Qt::UserRole + 3).toString(), u"recent/none"_s);
    auto *earlier = tree->topLevelItem(1);
    QCOMPARE(earlier->child(0)->data(0, Qt::UserRole + 2).toString(), u"unread"_s);
    QCOMPARE(earlier->child(1)->data(0, Qt::UserRole + 2).toString(), u"recent-read"_s);
    QCOMPARE(earlier->child(2)->data(0, Qt::UserRole + 2).toString(), u"old-read"_s);
}

QTEST_MAIN(AccountOverviewTest)
#include "test_account_overview.moc"
