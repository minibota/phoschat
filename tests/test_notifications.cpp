#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "notifications/linux_notifier.h"
#include "notifications/notification_manager.h"
#include "security/credential_store.h"
#include "storage/database.h"
#include "ui/contacts/MainWindow.hpp"
#include "ui/windows/WindowManager.hpp"

#include <QApplication>
#include <QDBusConnection>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace phoschat;
using namespace phoschat::ui;

class DesktopServer final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    struct Notification {
        QString app;
        uint replaces {};
        QString icon;
        QString title;
        QString body;
        QStringList actions;
        QVariantMap hints;
        int timeout {};
        uint id {};
    };
    QList<Notification> received;
    QList<uint> removed;
public slots:
    uint Notify(const QString &app, uint replaces, const QString &icon, const QString &title,
        const QString &body, const QStringList &actions, const QVariantMap &hints, int timeout)
    {
        const uint id = replaces ? replaces : ++nextId_;
        received.append({ app, replaces, icon, title, body, actions, hints, timeout, id });
        return id;
    }
    void CloseNotification(uint id) { removed.append(id); }
signals:
    void ActionInvoked(uint id, const QString &action);
    void NotificationClosed(uint id, uint reason);

private:
    uint nextId_ {};
};

class PortalServer final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Notification")
public:
    QList<QPair<QString, QVariantMap>> received;
    QStringList removed;
public slots:
    void AddNotification(const QString &id, const QVariantMap &notification)
    {
        received.append({ id, notification });
    }
    void RemoveNotification(const QString &id) { removed.append(id); }
signals:
    void ActionInvoked(const QString &id, const QString &action, const QVariantList &parameters);
};

class NotificationsTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void desktopPostsReplacesAndActivates();
    void portalPostsReplacesAndActivates();
    void withdrawingAnInFlightNotificationClosesIt();
    void unavailableServiceReportsFailure();
    void withoutATrayPrivateAlertsRespectPreferencesAndMessageState();

private:
    QTemporaryDir settingsDirectory_;
    QDBusConnection bus_ { QDBusConnection::sessionBus() };
    DesktopServer desktop_;
    PortalServer portal_;
};

void NotificationsTest::initTestCase()
{
    // Never register notification services on the developer's actual session bus.
    // CTest starts this executable inside a fresh dbus-run-session.
    if (qEnvironmentVariable("PHOSCHAT_PRIVATE_NOTIFICATION_TEST_BUS") != u"1")
        QSKIP("Run with CTest: notification tests require an isolated D-Bus session.");
    QVERIFY(bus_.isConnected());
    QVERIFY(bus_.registerService(u"org.freedesktop.Notifications"_s));
    QVERIFY(bus_.registerObject(u"/org/freedesktop/Notifications"_s, &desktop_,
        QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(bus_.registerService(u"org.freedesktop.portal.Desktop"_s));
    QVERIFY(bus_.registerObject(u"/org/freedesktop/portal/desktop"_s, &portal_,
        QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(settingsDirectory_.isValid());
    QCoreApplication::setOrganizationName(u"PhosChatTest"_s);
    QCoreApplication::setApplicationName(u"NotificationsTest"_s);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
    QApplication::setQuitOnLastWindowClosed(false);
}

void NotificationsTest::init()
{
    // Drain asynchronous cleanup from the previous notifier before clearing logs.
    QTest::qWait(30);
    desktop_.received.clear();
    desktop_.removed.clear();
    portal_.received.clear();
    portal_.removed.clear();
    QSettings().clear();
}

void NotificationsTest::desktopPostsReplacesAndActivates()
{
    LinuxNotifier notifier(bus_, LinuxNotifier::Backend::Desktop);
    QSignalSpy activated(&notifier, &LinuxNotifier::activated);
    QSignalSpy failed(&notifier, &LinuxNotifier::failed);
    notifier.post(u"conversation"_s, u"PhosChat"_s, u"First message"_s);
    notifier.post(u"conversation"_s, u"PhosChat"_s, u"Latest message"_s);
    QTRY_COMPARE(desktop_.received.size(), 2);
    const auto first = desktop_.received.first();
    const auto last = desktop_.received.last();
    QCOMPARE(first.app, u"PhosChat"_s);
    QCOMPARE(first.replaces, 0u);
    QCOMPARE(first.icon, u"io.phoschat.desktop"_s);
    QCOMPARE(first.title, u"PhosChat"_s);
    QCOMPARE(first.hints.value(u"desktop-entry"_s).toString(), u"io.phoschat.desktop"_s);
    QCOMPARE(first.hints.value(u"category"_s).toString(), u"im.received"_s);
    QCOMPARE(first.actions, QStringList({ u"default"_s, u"Open conversation"_s }));
    QCOMPARE(first.timeout, -1);
    QCOMPARE(last.replaces, first.id);
    QCOMPARE(last.body, u"Latest message"_s);
    emit desktop_.ActionInvoked(last.id, u"default"_s);
    QTRY_COMPARE(activated.size(), 1);
    QCOMPARE(activated.first().first().toString(), u"conversation"_s);
    QTRY_VERIFY(desktop_.removed.contains(last.id));
    QCOMPARE(failed.size(), 0);
    emit desktop_.ActionInvoked(last.id, u"default"_s);
    QTest::qWait(30);
    QCOMPARE(activated.size(), 1);
}

void NotificationsTest::portalPostsReplacesAndActivates()
{
    LinuxNotifier notifier(bus_, LinuxNotifier::Backend::Portal);
    QSignalSpy activated(&notifier, &LinuxNotifier::activated);
    QSignalSpy failed(&notifier, &LinuxNotifier::failed);
    notifier.post(u"conversation"_s, u"PhosChat"_s, u"New message"_s);
    QTRY_COMPARE(portal_.received.size(), 1);
    const auto first = portal_.received.first();
    QCOMPARE(first.first, u"conversation"_s);
    QCOMPARE(first.second.value(u"title"_s).toString(), u"PhosChat"_s);
    QCOMPARE(first.second.value(u"body"_s).toString(), u"New message"_s);
    QCOMPARE(first.second.value(u"icon"_s).toString(), u"io.phoschat.desktop"_s);
    QCOMPARE(first.second.value(u"priority"_s).toString(), u"normal"_s);
    QCOMPARE(first.second.value(u"default-action"_s).toString(), u"open"_s);
    notifier.post(u"conversation"_s, u"PhosChat"_s, u"New mention"_s);
    QTRY_COMPARE(portal_.received.size(), 2);
    QCOMPARE(portal_.received.last().first, first.first);
    QCOMPARE(portal_.received.last().second.value(u"body"_s).toString(), u"New mention"_s);
    emit portal_.ActionInvoked(u"unknown"_s, u"open"_s, {});
    emit portal_.ActionInvoked(u"conversation"_s, u"unknown"_s, {});
    emit portal_.ActionInvoked(u"conversation"_s, u"open"_s, {});
    QTRY_COMPARE(activated.size(), 1);
    QCOMPARE(activated.first().first().toString(), u"conversation"_s);
    QTRY_VERIFY(portal_.removed.contains(u"conversation"_s));
    QCOMPARE(failed.size(), 0);
}

void NotificationsTest::withdrawingAnInFlightNotificationClosesIt()
{
    LinuxNotifier desktop(bus_, LinuxNotifier::Backend::Desktop);
    desktop.post(u"conversation"_s, u"PhosChat"_s, u"New message"_s);
    desktop.withdraw(u"conversation"_s);
    QTRY_COMPARE(desktop_.received.size(), 1);
    QTRY_VERIFY(desktop_.removed.contains(desktop_.received.first().id));

    LinuxNotifier portal(bus_, LinuxNotifier::Backend::Portal);
    portal.post(u"conversation"_s, u"PhosChat"_s, u"New message"_s);
    portal.withdraw(u"conversation"_s);
    QTRY_COMPARE(portal_.received.size(), 1);
    QTRY_VERIFY(portal_.removed.contains(u"conversation"_s));
}

void NotificationsTest::unavailableServiceReportsFailure()
{
    QVERIFY(bus_.unregisterService(u"org.freedesktop.Notifications"_s));
    LinuxNotifier notifier(bus_, LinuxNotifier::Backend::Desktop);
    QSignalSpy failed(&notifier, &LinuxNotifier::failed);
    notifier.post(u"conversation"_s, u"PhosChat"_s, u"New message"_s);
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed.first().first().toString(), u"conversation"_s);
    QVERIFY(bus_.registerService(u"org.freedesktop.Notifications"_s));
}

void NotificationsTest::withoutATrayPrivateAlertsRespectPreferencesAndMessageState()
{
    QVERIFY(!QSystemTrayIcon::isSystemTrayAvailable());
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    Account account;
    account.id = u"account"_s;
    account.label = u"Private account label"_s;
    account.protocol = Protocol::Irc;
    account.host = u"irc.example.test"_s;
    account.nick = u"Private nickname"_s;
    QVERIFY(database.saveAccount(account));
    Conversation conversation;
    conversation.id = { account.id, u"#private"_s };
    conversation.title = u"Private conversation title"_s;
    QVERIFY(database.saveConversation(conversation));
    CredentialStore credentials;
    AccountManager accounts(database, credentials);
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    WindowManager windows(accounts, conversations, messages);
    MainWindow main(accounts, conversations, windows);
    NotificationManager notifications(main, windows, conversations, messages, {});
    Message message;
    message.conversation = conversation.id;
    message.text = u"Private content"_s;
    message.sender = u"Private sender"_s;
    emit messages.messageAdded(message);
    QTRY_COMPARE(desktop_.received.size(), 1);
    QCOMPARE(desktop_.received.first().title, u"PhosChat"_s);
    QCOMPARE(desktop_.received.first().body, u"You have a new message."_s);
    message.mention = true;
    emit messages.messageAdded(message);
    QTRY_COMPARE(desktop_.received.size(), 2);
    QCOMPARE(desktop_.received.last().body, u"You have a new mention."_s);

    message.historical = true;
    emit messages.messageAdded(message);
    message.historical = false;
    message.outgoing = true;
    emit messages.messageAdded(message);
    message.outgoing = false;
    conversations.setMuted(conversation.id, true);
    emit messages.messageAdded(message);
    conversations.setMuted(conversation.id, false);
    QSettings().setValue(u"notifications/enabled"_s, false);
    emit messages.messageAdded(message);
    QSettings().setValue(u"notifications/enabled"_s, true);
    message.conversation.remoteId = u"unknown"_s;
    emit messages.messageAdded(message);
    message.conversation = conversation.id;
    emit desktop_.ActionInvoked(desktop_.received.last().id, u"default"_s);
    QTRY_VERIFY(windows.isActive(conversation.id));
    emit messages.messageAdded(message);
    QTest::qWait(80);
    QCOMPARE(desktop_.received.size(), 2);
    windows.closeAll();
}

QTEST_MAIN(NotificationsTest)
#include "test_notifications.moc"
