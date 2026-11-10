#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "security/credential_store.h"
#include "storage/database.h"
#include "ui/conversations/MessageModel.hpp"
#include "ui/conversations/MessageView.hpp"
#include "ui/styles/Theme.hpp"
#include "ui/windows/ChatWindow.hpp"
#include "ui/windows/WindowManager.hpp"

#include <QApplication>
#include <QDir>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace phoschat;
using namespace phoschat::ui;

namespace {

Message message(qint64 sequence, bool historical = true)
{
    Message value;
    value.conversation = { u"telegram"_s, u"1/42"_s };
    value.id = QString::number(sequence);
    value.sender = u"Alex"_s;
    value.text = u"History message %1. A longer line gives this conversation naturally wrapped rows."_s.arg(
        sequence);
    value.timestamp = QDateTime::fromSecsSinceEpoch(1700000000 + sequence, QTimeZone::UTC);
    value.historical = historical;
    value.sequence = sequence;
    return value;
}

class HistoryProvider final : public IChatProvider {
public:
    explicit HistoryProvider(QObject *parent)
        : IChatProvider(parent)
    {
    }
    Capabilities capabilities() const override
    {
        return Capability::Text | Capability::RemoteHistory | Capability::PaginatedHistory;
    }
    void connectAccount(const QByteArray &) override
    {
        connected = true;
        state = HistoryState::Ready;
        emit stateChanged(ConnectionState::Connected);
    }
    void disconnectAccount() override
    {
        connected = false;
        state = HistoryState::Unavailable;
        emit stateChanged(ConnectionState::Disconnected);
    }
    bool join(const QString &) override { return false; }
    bool part(const QString &) override { return false; }
    bool sendText(const ConversationId &, const QString &) override { return false; }
    bool sendCommand(const ConversationId &, const QString &) override { return false; }
    QString canonicalTarget(const QString &target) const override { return target; }
    void requestHistory(const ConversationId &id) override
    {
        if (connected) {
            ++initialRequests;
            setHistoryState(id, HistoryState::Loading);
        }
    }
    void requestOlderHistory(const ConversationId &id) override
    {
        if (state == HistoryState::Ready) {
            ++olderRequests;
            setHistoryState(id, HistoryState::Loading);
        }
    }
    HistoryState historyState(const ConversationId &) const override { return state; }
    void setConversationOpen(const ConversationId &, bool value) override { open = value; }
    void setHistoryState(const ConversationId &id, HistoryState value)
    {
        state = value;
        emit historyStateChanged(id);
    }
    bool connected {};
    bool open {};
    int initialRequests {};
    int olderRequests {};
    HistoryState state { HistoryState::Unavailable };
};

ChatWindow *visibleChat()
{
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (auto *chat = qobject_cast<ChatWindow *>(widget); chat && chat->isVisible())
            return chat;
    }
    return nullptr;
}

} // namespace

class HistoryUiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void olderPageKeepsViewport_data();
    void olderPageKeepsViewport();
    void fullTimelineRetainsPagesAndSameSecondOrder();
    void restoredConversationLoadsAfterConnecting();

private:
    QTemporaryDir settingsDirectory_;
};

void HistoryUiTest::initTestCase()
{
    QVERIFY(settingsDirectory_.isValid());
    QApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(u"PhosChatTest"_s);
    QCoreApplication::setApplicationName(u"PhosChatHistoryUiTest"_s);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
}

void HistoryUiTest::olderPageKeepsViewport_data()
{
    QTest::addColumn<bool>("lightTheme");
    QTest::newRow("dark") << false;
    QTest::newRow("light") << true;
}

void HistoryUiTest::olderPageKeepsViewport()
{
    QFETCH(bool, lightTheme);
    applyTheme(*qApp, lightTheme ? Theme::Light : Theme::Dark);
    MessageModel model({});
    for (int i = 101; i <= 150; ++i)
        model.append(message(i));
    MessageView view(model);
    view.resize(480, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.showLatest();
    QTRY_VERIFY(view.verticalScrollBar()->maximum() > 0);
    QTRY_COMPARE(view.verticalScrollBar()->value(), view.verticalScrollBar()->maximum());

    view.scrollTo(model.index(15), QAbstractItemView::PositionAtTop);
    view.verticalScrollBar()->setValue(view.verticalScrollBar()->value() + 19);
    const QPersistentModelIndex anchor = view.indexAt(QPoint(view.viewport()->width() / 2, 0));
    QVERIFY(anchor.isValid());
    const auto anchorId = anchor.data(MessageModel::MessageIdRole).toString();
    const int offset = view.visualRect(anchor).top();
    QVERIFY(offset < 0);
    for (int i = 51; i <= 100; ++i) {
        auto earlier = message(i);
        earlier.outgoing = true;
        model.append(earlier);
    }
    QCoreApplication::processEvents();
    QTRY_COMPARE(view.visualRect(anchor).top(), offset);
    QTRY_COMPARE(
        view.indexAt(QPoint(view.viewport()->width() / 2, 0)).data(MessageModel::MessageIdRole).toString(),
        anchorId);
    QVERIFY(view.verticalScrollBar()->value() < view.verticalScrollBar()->maximum());

    model.append(message(151, false));
    QCoreApplication::processEvents();
    QTRY_COMPARE(view.visualRect(anchor).top(), offset);
    auto sent = message(152, false);
    sent.outgoing = true;
    model.append(sent);
    QTRY_COMPARE(view.verticalScrollBar()->value(), view.verticalScrollBar()->maximum());
}

void HistoryUiTest::fullTimelineRetainsPagesAndSameSecondOrder()
{
    MessageModel full({});
    full.setRetainFullHistory(true);
    MessageModel recent({});
    const auto timestamp = message(1).timestamp;
    for (int i = 1200; i > 0; --i) {
        auto older = message(i);
        older.timestamp = timestamp;
        full.append(older);
        recent.append(older);
    }
    QCOMPARE(full.rowCount(), 1200);
    QCOMPARE(full.index(0).data(MessageModel::MessageIdRole).toString(), u"1"_s);
    QCOMPARE(full.index(1199).data(MessageModel::MessageIdRole).toString(), u"1200"_s);
    QVERIFY(recent.rowCount() <= 1000);
}

void HistoryUiTest::restoredConversationLoadsAfterConnecting()
{
    Database database(u":memory:"_s);
    QVERIFY(database.open());
    CredentialStore credentials;
    AccountManager accounts(database, credentials, nullptr,
        [](const Account &, QObject *owner) { return new HistoryProvider(owner); });
    Account account;
    account.id = u"telegram"_s;
    account.label = u"Personal"_s;
    account.protocol = Protocol::Telegram;
    account.providerId = u"12345"_s;
    QVERIFY(accounts.saveAccount(account, QByteArrayLiteral("0123456789abcdef0123456789abcdef")));
    Conversation conversation;
    conversation.id = { account.id, u"1/42"_s };
    conversation.title = u"History test"_s;
    conversation.kind = ConversationKind::Group;
    QVERIFY(database.saveConversation(conversation));
    ConversationManager conversations(accounts, database);
    MessageManager messages(accounts, conversations, database);
    QSettings().setValue(u"windows/openChats"_s,
        QVariantList { QVariant(QStringList { conversation.id.accountId, conversation.id.remoteId }) });
    WindowManager windows(accounts, conversations, messages);
    windows.restoreSession();
    auto *window = visibleChat();
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *button = window->findChild<QPushButton *>(u"loadOlderMessages"_s);
    QVERIFY(button);
    QTRY_VERIFY(button->isVisible());
    QVERIFY(!button->isEnabled());

    accounts.connectAccount(account.id);
    auto *provider = static_cast<HistoryProvider *>(accounts.provider(account.id));
    QVERIFY(provider);
    QVERIFY(provider->open);
    QTRY_VERIFY(provider->initialRequests > 0);
    QVERIFY(!button->isEnabled());
    provider->setHistoryState(conversation.id, HistoryState::Ready);
    QTRY_VERIFY(button->isEnabled());
    auto *timeline = window->findChild<QListView *>();
    QVERIFY(timeline);
    QCOMPARE(timeline->model()->rowCount(), 0);
    provider->messageReceived(message(50));
    QCOMPARE(timeline->model()->rowCount(), 1);
    QVERIFY(button->isEnabled());
    const auto captureDirectory = qEnvironmentVariable("PHOSCHAT_UI_SCREENSHOT_DIR");
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(captureDirectory));
        QCoreApplication::processEvents();
        QVERIFY(window->grab().save(captureDirectory + u"/history-single-message.png"_s));
    }
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(provider->olderRequests, 1);
    QVERIFY(!button->isEnabled());
    provider->setHistoryState(conversation.id, HistoryState::Complete);
    QCOMPARE(button->text(), u"Beginning of conversation"_s);
    QVERIFY(!button->isEnabled());

    accounts.disconnectAccount(account.id);
    accounts.connectAccount(account.id);
    provider = static_cast<HistoryProvider *>(accounts.provider(account.id));
    QVERIFY(provider);
    QTRY_VERIFY(provider->initialRequests > 0);
    QVERIFY(provider->open);
    window->close();
    QVERIFY(!provider->open);
    windows.closeAll();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QTEST_MAIN(HistoryUiTest)
#include "test_history_ui.moc"
