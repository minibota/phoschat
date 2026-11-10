#include "ui/windows/WindowManager.hpp"

#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "ui/Presentation.hpp"
#include "ui/conversations/ConversationController.hpp"
#include "ui/conversations/ConversationProfile.hpp"
#include "ui/windows/ChatWindow.hpp"

#include <QCryptographicHash>
#include <QSettings>
#include <QTimer>
#include <QVariant>
#include <algorithm>

namespace phoschat::ui {
namespace {

    constexpr auto kOpenChats = "windows/openChats";

    QString settingsId(const QString &id)
    {
        return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex());
    }

    QString accountGeometryKey(const QString &accountId)
    {
        return QStringLiteral("windows/chatGeometry/") + settingsId(accountId);
    }

    QString geometryKey(const ConversationId &id)
    {
        return accountGeometryKey(id.accountId) + QLatin1Char('/') + settingsId(id.remoteId);
    }

} // namespace

WindowManager::WindowManager(
    AccountManager &accounts, ConversationManager &conversations, MessageManager &messages, QObject *parent)
    : QObject(parent)
    , m_accounts(accounts)
    , m_conversations(conversations)
    , m_messages(messages)
{
    connect(&m_conversations, &ConversationManager::channelJoined, this, &WindowManager::open,
        Qt::QueuedConnection);
    connect(&m_conversations, &ConversationManager::conversationsChanged, this, [this] {
        pruneRemovedConversations();
        refreshOpenWindows();
    });
    connect(&m_accounts, &AccountManager::accountStateChanged, this,
        [this](const QString &accountId, ConnectionState state) {
            refreshOpenWindows();
            if (m_closing || state != ConnectionState::Connected)
                return;
            const auto ids = m_windows.keys();
            for (const auto &id : ids) {
                if (id.accountId == accountId && m_windows.value(id)) {
                    m_messages.setConversationOpen(id, true);
                    if (m_messages.hasPagedHistory(id))
                        m_messages.requestHistory(id);
                }
            }
        });
    connect(&m_accounts, &AccountManager::accountsChanged, this, [this] { refreshOpenWindows(); });
    connect(&m_accounts, &AccountManager::accountRemoved, this, [](const QString &id) {
        QSettings settings;
        settings.remove(accountGeometryKey(id));
        auto entries = settings.value(QLatin1String(kOpenChats)).toList();
        entries.removeIf([&id](const QVariant &entry) { return entry.toStringList().value(0) == id; });
        settings.setValue(QLatin1String(kOpenChats), entries);
    });
    connect(&m_conversations, &ConversationManager::membersChanged, this, [this](const ConversationId &id) {
        if (const auto window = m_windows.value(id)) {
            window->setMembers(m_conversations.members(id));
        }
    });
    connect(&m_messages, &MessageManager::messageAdded, this, [this](const Message &message) {
        if (!message.historical && !message.outgoing && isActive(message.conversation)) {
            const auto id = message.conversation;
            QTimer::singleShot(0, this, [this, id] {
                if (isActive(id)) {
                    m_conversations.markRead(id);
                }
            });
        }
    });
}

WindowManager::~WindowManager() { closeAll(); }

void WindowManager::open(const ConversationId &id) { showConversation(id, true); }

void WindowManager::showConversation(const ConversationId &id, bool focus)
{
    const auto conversation = m_conversations.conversation(id);
    if (m_closing || !conversation) {
        return;
    }

    auto window = m_windows.value(id);
    if (!window) {
        auto *controller = m_controllers.value(id, nullptr);
        if (!controller) {
            controller = new ConversationController(id, m_messages, this);
            m_controllers.insert(id, controller);
        }
        window = new ChatWindow(*controller);
        m_windows.insert(id, window);
        connect(window, &ChatWindow::profileRequested, this, [this, id, window] {
            if (const auto value = m_conversations.conversation(id))
                showConversationProfile(*window, m_accounts, *value);
        });
        m_messages.setConversationOpen(id, true);
        connect(window, &ChatWindow::activated, this, [this, id] {
            m_messages.requestHistory(id);
            m_conversations.markRead(id);
        });
        connect(window, &ChatWindow::closing, this, [this, id](const QByteArray &geometry) {
            QSettings().setValue(geometryKey(id), geometry);
            m_windows.remove(id);
            m_messages.setConversationOpen(id, false);
        });
        refreshWindow(*window, *conversation);
        window->setMembers(m_conversations.members(id));
        const auto geometry = QSettings().value(geometryKey(id)).toByteArray();
        if (!geometry.isEmpty()) {
            window->restoreGeometry(geometry);
        }
        window->setAttribute(Qt::WA_ShowWithoutActivating, !focus);
        window->show();
        window->setAttribute(Qt::WA_ShowWithoutActivating, false);
        if (!focus && m_messages.hasPagedHistory(id))
            m_messages.requestHistory(id);
    }
    if (focus) {
        if (window->isMinimized()) {
            window->showNormal();
        }
        window->raise();
        window->activateWindow();
        window->focusComposer();
        m_messages.requestHistory(id);
        m_conversations.markRead(id);
    }
}

bool WindowManager::isActive(const ConversationId &id) const
{
    const auto window = m_windows.value(id);
    return window && window->isVisible() && !window->isMinimized() && window->isActiveWindow();
}

void WindowManager::restoreSession()
{
    const QSettings settings;
    if (!settings.value(QStringLiteral("windows/restoreChats"), true).toBool()) {
        return;
    }
    for (const auto &entry : settings.value(QLatin1String(kOpenChats)).toList()) {
        const auto fields = entry.toStringList();
        if (fields.size() == 2) {
            showConversation({ fields.at(0), fields.at(1) }, false);
        }
    }
}

void WindowManager::saveSession() const
{
    if (m_closing) {
        return;
    }
    QVariantList entries;
    QSettings settings;
    for (auto it = m_windows.cbegin(); it != m_windows.cend(); ++it) {
        if (const auto window = it.value()) {
            const auto &id = it.key();
            entries.append(QVariant(QStringList { id.accountId, id.remoteId }));
            settings.setValue(geometryKey(id), window->saveGeometry());
        }
    }
    settings.setValue(QLatin1String(kOpenChats), entries);
}

void WindowManager::closeAll()
{
    if (m_closing) {
        return;
    }
    saveSession();
    m_closing = true;
    const auto windows = m_windows.values();
    for (const auto &window : windows) {
        if (window) {
            window->close();
        }
    }
}

void WindowManager::refreshOpenWindows()
{
    for (auto it = m_windows.cbegin(); it != m_windows.cend(); ++it) {
        const auto conversation = m_conversations.conversation(it.key());
        if (conversation && it.value()) {
            refreshWindow(*it.value(), *conversation);
        }
    }
}

void WindowManager::refreshWindow(ChatWindow &window, const Conversation &conversation) const
{
    const auto accounts = m_accounts.accounts();
    const auto it = std::ranges::find(accounts, conversation.id.accountId, &Account::id);
    const auto label = it == accounts.end() ? QString {} : accountLabel(*it);
    const auto &id = conversation.id.accountId;
    window.refresh(
        conversation, label, m_accounts.state(id), m_accounts.statusDetail(id), m_accounts.protocol(id));
}

void WindowManager::pruneRemovedConversations()
{
    const auto ids = m_controllers.keys();
    QSettings settings;
    for (const auto &id : ids) {
        if (m_conversations.conversation(id)) {
            continue;
        }
        if (const auto window = m_windows.value(id)) {
            window->close();
        }
        settings.remove(geometryKey(id));
        m_controllers.take(id)->deleteLater();
    }
}

} // namespace phoschat::ui
