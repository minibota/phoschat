#include "ui/contacts/MainWindow.hpp"

#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "ui/Presentation.hpp"
#include "ui/discovery/IrcDirectoryDialog.hpp"
#include "ui/settings/AccountDialog.hpp"
#include "ui/styles/Theme.hpp"
#include "ui/windows/WindowManager.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace phoschat::ui {
namespace {

    constexpr int kAccountRole = Qt::UserRole + 1;
    constexpr int kTargetRole = Qt::UserRole + 2;
    constexpr int kSectionRole = Qt::UserRole + 3;

    constexpr auto kMainGeometry = "windows/mainGeometry";
    constexpr auto kGrouping = "ui/grouping";
    constexpr auto kCollapsed = "ui/collapsedSections";
    constexpr auto kCloseToTray = "windows/closeToTray";
    constexpr auto kMinimizeToTray = "windows/minimizeToTray";
    constexpr auto kRestoreChats = "windows/restoreChats";

    QLabel *label(const QString &text, const char *role)
    {
        auto *result = new QLabel(text);
        result->setTextFormat(Qt::PlainText);
        result->setProperty("role", QString::fromLatin1(role));
        return result;
    }

    QFrame *rule()
    {
        auto *result = new QFrame;
        result->setProperty("role", QStringLiteral("rule"));
        result->setFixedHeight(1);
        return result;
    }

    QToolButton *iconButton(const QIcon &icon, const QString &description)
    {
        auto *button = new QToolButton;
        button->setProperty("role", QStringLiteral("icon"));
        button->setIcon(icon);
        button->setIconSize(QSize(17, 17));
        button->setPopupMode(QToolButton::InstantPopup);
        button->setAccessibleName(description);
        button->setToolTip(description);
        return button;
    }

} // namespace

MainWindow::MainWindow(
    AccountManager &accounts, ConversationManager &conversations, WindowManager &windows, QWidget *parent)
    : QMainWindow(parent)
    , m_accounts(accounts)
    , m_conversations(conversations)
    , m_windows(windows)
{
    setWindowTitle(QStringLiteral("PhosChat"));
    setWindowIcon(brandIcon());
    resize(328, 620);
    setMinimumSize(282, 440);
    const QSettings settings;
    const int grouping = settings.value(QLatin1String(kGrouping), 0).toInt();
    if (grouping >= 0 && grouping <= static_cast<int>(Grouping::Recent)) {
        m_grouping = static_cast<Grouping>(grouping);
    }
    for (const auto &key : settings.value(QLatin1String(kCollapsed)).toStringList()) {
        m_expanded.insert(key, false);
    }

    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(14, 13, 14, 12);
    layout->setSpacing(12);

    auto *header = new QHBoxLayout;
    header->setSpacing(9);
    auto *logo = new QLabel;
    logo->setPixmap(brandIcon().pixmap(QSize(34, 34), devicePixelRatioF()));
    header->addWidget(logo);
    auto *branding = new QVBoxLayout;
    branding->setSpacing(1);
    branding->addWidget(label(QStringLiteral("PhosChat"), "heading"));
    auto *tagline = label(tr("All your conversations. One place."), "muted");
    tagline->setWordWrap(true);
    branding->addWidget(tagline);
    header->addLayout(branding, 1);

    auto *more = iconButton(moreIcon(), tr("Settings and networks"));
    auto *settingsMenu = new QMenu(more);
    settingsMenu->addAction(tr("Accounts…"), this, &MainWindow::showAccounts);
    m_networks = settingsMenu->addMenu(tr("Networks"));
    connect(m_networks, &QMenu::aboutToShow, this, &MainWindow::refreshNetworks);
    auto *groupingMenu = settingsMenu->addMenu(tr("Group conversations by"));
    auto *group = new QActionGroup(groupingMenu);
    for (const auto &[mode, text] :
        { std::pair { Grouping::Account, tr("Account") }, { Grouping::Protocol, tr("Protocol") },
            { Grouping::Custom, tr("Custom group") }, { Grouping::Recent, tr("Recent activity") } }) {
        auto *action = groupingMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(m_grouping == mode);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { setGrouping(mode); });
    }
    auto *appearanceMenu = settingsMenu->addMenu(tr("Appearance"));
    settingsMenu->addSeparator();
    auto addPreference = [&settings, settingsMenu](const QString &text, const char *key, bool defaultValue) {
        auto *action = settingsMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(settings.value(QLatin1String(key), defaultValue).toBool());
        QObject::connect(action, &QAction::toggled, action,
            [key](bool enabled) { QSettings().setValue(QLatin1String(key), enabled); });
        return action;
    };
    addPreference(tr("Restore chats on startup"), kRestoreChats, true);
    m_closeToTray = addPreference(tr("Close to tray"), kCloseToTray, true);
    m_minimizeToTray = addPreference(tr("Minimize to tray"), kMinimizeToTray, false);
    m_closeToTray->setEnabled(false);
    m_minimizeToTray->setEnabled(false);
    settingsMenu->addSeparator();
    settingsMenu->addAction(tr("Quit PhosChat"), this, [this] {
        saveWindowGeometry();
        emit quitRequested();
    });
    more->setMenu(settingsMenu);
    header->addWidget(more, 0, Qt::AlignTop);
    layout->addLayout(header);

    auto *profile = new QWidget;
    profile->setProperty("role", QStringLiteral("profileCard"));
    auto *profileLayout = new QHBoxLayout(profile);
    profileLayout->setContentsMargins(9, 8, 9, 8);
    profileLayout->setSpacing(8);
    m_avatar = label(QStringLiteral("P"), "avatar");
    m_avatar->setFixedSize(30, 30);
    m_avatar->setAlignment(Qt::AlignCenter);
    profileLayout->addWidget(m_avatar);
    auto *identity = new QVBoxLayout;
    identity->setSpacing(1);
    m_profileName = label({}, "profile");
    m_profileState = label({}, "muted");
    for (auto *field : { m_profileName, m_profileState }) {
        field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        identity->addWidget(field);
    }
    profileLayout->addLayout(identity, 1);
    auto *accountButton = new QPushButton(tr("Accounts"));
    accountButton->setToolTip(tr("Manage network connections"));
    connect(accountButton, &QPushButton::clicked, this, &MainWindow::showAccounts);
    profileLayout->addWidget(accountButton);
    layout->addWidget(profile);

    m_search = new QLineEdit;
    m_search->setProperty("role", QStringLiteral("search"));
    m_search->setPlaceholderText(tr("Find a conversation…"));
    m_search->setAccessibleName(tr("Find a conversation"));
    m_search->setClearButtonEnabled(true);
    auto *searchAction = m_search->addAction(searchIcon(), QLineEdit::LeadingPosition);
    layout->addWidget(m_search);

    auto *section = new QHBoxLayout;
    section->addWidget(label(tr("CONVERSATIONS"), "section"));
    section->addStretch();
    auto *add = iconButton(addIcon(), tr("Start a conversation"));
    auto *appearance = new QActionGroup(appearanceMenu);
    for (const auto &[theme, name] :
        { std::pair { Theme::Dark, tr("Dark") }, { Theme::Light, tr("Light") } }) {
        auto *action = appearanceMenu->addAction(name);
        action->setCheckable(true);
        action->setChecked(preferredTheme() == theme);
        appearance->addAction(action);
        connect(action, &QAction::triggered, this, [this, more, searchAction, add, theme] {
            setPreferredTheme(*qApp, theme);
            more->setIcon(moreIcon());
            searchAction->setIcon(searchIcon());
            add->setIcon(addIcon());
            reloadConversations();
        });
    }
    auto *createMenu = new QMenu(add);
    createMenu->addAction(tr("Join IRC channel…"), this, [this] { showStartConversation(true); });
    createMenu->addAction(tr("Discover IRC channels…"), this, &MainWindow::showIrcDirectory);
    createMenu->addAction(tr("Open IRC direct message…"), this, [this] { showStartConversation(false); });
    add->setMenu(createMenu);
    section->addWidget(add);
    layout->addLayout(section);

    m_tree = new QTreeWidget;
    m_tree->setProperty("role", QStringLiteral("conversations"));
    m_tree->setAccessibleName(tr("Conversations"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(3);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_tree->setColumnWidth(1, 44);
    m_tree->setColumnWidth(2, 35);
    m_tree->setIndentation(12);
    m_tree->setUniformRowHeights(true);
    m_tree->setIconSize(QSize(18, 18));
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_tree, 1);

    m_empty = new QWidget;
    auto *emptyLayout = new QVBoxLayout(m_empty);
    emptyLayout->setContentsMargins(12, 12, 12, 14);
    emptyLayout->setSpacing(7);
    emptyLayout->addStretch();
    m_emptyHeading = label({}, "profile");
    m_emptyHeading->setWordWrap(true);
    m_emptyHeading->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(m_emptyHeading);
    m_emptyDescription = label({}, "muted");
    m_emptyDescription->setAlignment(Qt::AlignCenter);
    m_emptyDescription->setWordWrap(true);
    emptyLayout->addWidget(m_emptyDescription);
    m_emptyAction = new QPushButton;
    m_emptyAction->setProperty("role", QStringLiteral("primary"));
    emptyLayout->addSpacing(4);
    emptyLayout->addWidget(m_emptyAction, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    layout->addWidget(m_empty, 1);

    layout->addWidget(rule());
    auto *status = new QHBoxLayout;
    m_status = label({}, "muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status->addWidget(m_status, 1);
    m_unread = label({}, "badge");
    status->addWidget(m_unread);
    layout->addLayout(status);
    setCentralWidget(central);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setSingleShot(true);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshStatus);
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::reloadConversations);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &MainWindow::refreshProfile);
    auto openItem = [this](QTreeWidgetItem *item) {
        if (const auto id = itemConversation(item)) {
            m_windows.open(*id);
        }
    };
    connect(m_tree, &QTreeWidget::itemActivated, this, openItem);
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (m_search->text().trimmed().isEmpty()) {
            m_expanded[item->data(0, kSectionRole).toString()] = true;
            QSettings settings;
            auto collapsed = settings.value(QLatin1String(kCollapsed)).toStringList();
            collapsed.removeAll(item->data(0, kSectionRole).toString());
            settings.setValue(QLatin1String(kCollapsed), collapsed);
        }
    });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (m_search->text().trimmed().isEmpty()) {
            const auto key = item->data(0, kSectionRole).toString();
            m_expanded[key] = false;
            QSettings settings;
            auto collapsed = settings.value(QLatin1String(kCollapsed)).toStringList();
            if (!collapsed.contains(key)) {
                collapsed.append(key);
                settings.setValue(QLatin1String(kCollapsed), collapsed);
            }
        }
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::showConversationMenu);
    connect(m_emptyAction, &QPushButton::clicked, this, [this] {
        if (!m_search->text().trimmed().isEmpty()) {
            m_search->clear();
        } else if (std::ranges::any_of(m_accounts.accounts(),
                       [](const Account &account) { return account.protocol == Protocol::Irc; })) {
            showIrcDirectory();
        } else {
            showAccounts();
        }
    });
    connect(&m_accounts, &AccountManager::accountsChanged, this, &MainWindow::reloadConversations);
    connect(&m_accounts, &AccountManager::accountStateChanged, this, [this] {
        refreshProfile();
        refreshStatus();
    });
    connect(&m_accounts, &AccountManager::errorOccurred, this,
        [this](const QString &, const QString &message) { showStatusMessage(message); });
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(30);
    connect(m_reloadTimer, &QTimer::timeout, this, &MainWindow::reloadConversations);
    connect(&m_conversations, &ConversationManager::conversationsChanged, this, [this] {
        if (!m_reloadTimer->isActive()) {
            m_reloadTimer->start();
        }
    });
    connect(&m_accounts, &AccountManager::accountRemoved, this, [this](const QString &id) {
        const auto key = QStringLiteral("account/") + id;
        m_expanded.remove(key);
        QSettings settings;
        auto collapsed = settings.value(QLatin1String(kCollapsed)).toStringList();
        if (collapsed.removeAll(key)) {
            settings.setValue(QLatin1String(kCollapsed), collapsed);
        }
    });

    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    auto *join = new QShortcut(QKeySequence(QStringLiteral("Ctrl+J")), this);
    connect(join, &QShortcut::activated, this, [this] { showStartConversation(true); });

    const auto geometry = settings.value(QLatin1String(kMainGeometry)).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    reloadConversations();
}

void MainWindow::setTrayAvailable(bool available)
{
    m_trayAvailable = available;
    m_closeToTray->setEnabled(available);
    m_minimizeToTray->setEnabled(available);
}

void MainWindow::showStatusMessage(const QString &message)
{
    m_status->setText(message);
    m_status->setToolTip(message.toHtmlEscaped());
    m_statusTimer->start(6500);
}

void MainWindow::prepareToQuit()
{
    m_quitting = true;
    saveWindowGeometry();
}

void MainWindow::saveWindowGeometry() const
{
    QSettings().setValue(QLatin1String(kMainGeometry), saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_quitting) {
        event->accept();
        return;
    }
    saveWindowGeometry();
    if (m_trayAvailable && m_closeToTray->isChecked()) {
        hide();
        emit hiddenToTray();
        event->ignore();
        return;
    }
    event->accept();
    emit quitRequested();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && isMinimized() && m_trayAvailable
        && m_minimizeToTray->isChecked()) {
        QTimer::singleShot(0, this, [this] {
            if (!m_quitting && isMinimized()) {
                hide();
                emit hiddenToTray();
            }
        });
    }
}

void MainWindow::showAccounts()
{
    AccountDialog dialog(m_accounts, this);
    dialog.exec();
}

void MainWindow::showIrcDirectory()
{
    const auto active = itemConversation(m_tree->currentItem());
    IrcDirectoryDialog dialog(m_accounts, m_conversations, active ? active->accountId : QString {}, this);
    dialog.exec();
}

void MainWindow::showStartConversation(bool channel)
{
    auto accounts = m_accounts.accounts();
    accounts.removeIf([](const Account &account) { return account.protocol != Protocol::Irc; });
    if (accounts.isEmpty()) {
        showAccounts();
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(channel ? tr("Join IRC channel") : tr("Open IRC direct message"));
    dialog.setMinimumWidth(315);
    auto *content = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *network = new QComboBox;
    const auto activeConversation = itemConversation(m_tree->currentItem());
    int preferred = -1;
    int firstOnline = -1;
    for (const auto &account : accounts) {
        const bool online = m_accounts.state(account.id) == ConnectionState::Connected;
        const auto title
            = tr("%1 · %2").arg(accountLabel(account), connectionLabel(m_accounts.state(account.id)));
        network->addItem(title, account.id);
        const int row = network->count() - 1;
        if (online && firstOnline < 0) {
            firstOnline = row;
        }
        if (activeConversation && activeConversation->accountId == account.id) {
            preferred = row;
        }
    }
    const bool preferredOnline = preferred >= 0
        && m_accounts.state(network->itemData(preferred).toString()) == ConnectionState::Connected;
    network->setCurrentIndex(preferredOnline ? preferred : std::max(0, firstOnline));
    form->addRow(tr("&Network"), network);
    auto *target = new QLineEdit;
    target->setMaxLength(200);
    target->setPlaceholderText(channel ? QStringLiteral("#channel") : tr("Nickname"));
    form->addRow(channel ? tr("&Channel") : tr("N&ickname"), target);
    content->addLayout(form);
    auto *hint = label(channel ? tr("Use the full channel name, including its prefix.")
                               : tr("Use the person’s IRC nickname."),
        "muted");
    hint->setWordWrap(true);
    content->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *accept = buttons->addButton(channel ? tr("Join") : tr("Open"), QDialogButtonBox::AcceptRole);
    accept->setProperty("role", QStringLiteral("primary"));
    accept->setDefault(true);
    content->addWidget(buttons);
    auto refresh = [&] {
        const bool online = m_accounts.state(network->currentData().toString()) == ConnectionState::Connected;
        accept->setEnabled(!target->text().trimmed().isEmpty() && online);
        if (!online) {
            hint->setText(tr("Connect this network first from Accounts."));
        } else if (channel) {
            hint->setText(tr("Use the full channel name, including its prefix."));
        } else {
            hint->setText(tr("Use the person’s IRC nickname."));
        }
    };
    connect(target, &QLineEdit::textChanged, &dialog, refresh);
    connect(network, &QComboBox::currentIndexChanged, &dialog, refresh);
    connect(&m_accounts, &AccountManager::accountStateChanged, &dialog, refresh);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    refresh();
    target->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const auto id = network->currentData().toString();
    const auto name = target->text().trimmed();
    if (channel) {
        if (m_conversations.joinChannel(id, name)) {
            showStatusMessage(tr("Joining %1…").arg(name));
        } else {
            showStatusMessage(
                tr("The channel could not be joined. Check its name and the account connection."));
        }
    } else {
        const auto conversation = m_conversations.openDirect(id, name);
        if (!conversation.isValid()) {
            showStatusMessage(tr("Enter a valid IRC nickname."));
        } else {
            m_windows.open(conversation);
        }
    }
}

void MainWindow::reloadConversations()
{
    const auto selection = itemConversation(m_tree->currentItem());
    const auto filter = m_search->text().trimmed();
    const int scrollPosition = m_lastFilter == filter ? m_tree->verticalScrollBar()->value() : 0;
    m_lastFilter = filter;
    const auto accounts = m_accounts.accounts();
    QHash<QString, Account> accountsById;
    for (const auto &account : accounts) {
        accountsById.insert(account.id, account);
    }

    auto conversations = m_conversations.conversations();
    std::stable_sort(
        conversations.begin(), conversations.end(), [this](const Conversation &a, const Conversation &b) {
            if (a.favorite != b.favorite) {
                return a.favorite;
            }
            if (m_grouping == Grouping::Recent && a.updated != b.updated) {
                return a.updated > b.updated;
            }
            return a.title.localeAwareCompare(b.title) < 0;
        });

    const QSignalBlocker blocker(m_tree);
    const auto colors = QApplication::palette();
    m_tree->clear();
    QHash<QString, QTreeWidgetItem *> sections;
    for (const auto &conversation : conversations) {
        const auto &id = conversation.id;
        const auto account = accountsById.value(id.accountId);
        const auto accountName = accountLabel(account);
        const auto protocol = protocolLabel(account.protocol);
        if (!filter.isEmpty() && !conversation.title.contains(filter, Qt::CaseInsensitive)
            && !accountName.contains(filter, Qt::CaseInsensitive)
            && !conversation.group.contains(filter, Qt::CaseInsensitive)
            && !protocol.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }

        QString key;
        QString sectionTitle;
        if (conversation.favorite) {
            key = QStringLiteral("favorites");
            sectionTitle = tr("Favorites");
        } else {
            switch (m_grouping) {
            case Grouping::Account:
                key = QStringLiteral("account/") + id.accountId;
                sectionTitle = accountName.isEmpty() ? protocol : tr("%1 · %2").arg(accountName, protocol);
                break;
            case Grouping::Protocol:
                key = QStringLiteral("protocol/") + protocol;
                sectionTitle = protocol;
                break;
            case Grouping::Custom:
                key = QStringLiteral("custom/") + conversation.group;
                sectionTitle = conversation.group.isEmpty() ? tr("Ungrouped") : conversation.group;
                break;
            case Grouping::Recent:
                if (!conversation.updated.isValid()) {
                    key = QStringLiteral("recent/none");
                    sectionTitle = tr("No recent messages");
                } else if (conversation.updated.toLocalTime().date() == QDate::currentDate()) {
                    key = QStringLiteral("recent/today");
                    sectionTitle = tr("Today");
                } else {
                    key = QStringLiteral("recent/earlier");
                    sectionTitle = tr("Earlier");
                }
                break;
            }
        }

        auto *section = sections.value(key, nullptr);
        if (!section) {
            section = new QTreeWidgetItem(m_tree);
            section->setFirstColumnSpanned(true);
            section->setText(0, sectionTitle);
            section->setData(0, kSectionRole, key);
            auto font = section->font(0);
            font.setBold(true);
            section->setFont(0, font);
            section->setForeground(0, colors.brush(QPalette::PlaceholderText));
            section->setFlags(section->flags() & ~Qt::ItemIsSelectable);
            sections.insert(key, section);
        }
        auto *item = new QTreeWidgetItem(section);
        const bool channel = conversation.kind == ConversationKind::Channel;
        item->setText(0,
            (m_grouping != Grouping::Account || conversation.favorite) && !accountName.isEmpty()
                ? tr("%1 · %2").arg(conversation.title, accountName)
                : conversation.title);
        item->setData(0, kAccountRole, id.accountId);
        item->setData(0, kTargetRole, id.remoteId);
        item->setIcon(0, conversationIcon(channel));
        item->setText(1, account.protocol == Protocol::Telegram ? QStringLiteral("TG") : protocol);
        item->setToolTip(1, protocol);
        item->setTextAlignment(1, Qt::AlignCenter);
        item->setForeground(1, colors.brush(QPalette::PlaceholderText));
        auto protocolFont = item->font(1);
        protocolFont.setPointSizeF(std::max(8.0, font().pointSizeF() - 1.0));
        item->setFont(1, protocolFont);
        if (conversation.unread > 0) {
            auto emphasis = item->font(0);
            emphasis.setBold(true);
            item->setFont(0, emphasis);
            item->setFont(2, emphasis);
            item->setText(
                2, conversation.unread > 99 ? QStringLiteral("99+") : QString::number(conversation.unread));
            item->setTextAlignment(2, Qt::AlignCenter);
            item->setForeground(2, colors.brush(QPalette::Link));
        }
        auto tooltip = tr("%1 · %2").arg(conversation.title, accountName);
        if (conversation.muted) {
            tooltip += tr(" · Muted");
        }
        if (channel && account.protocol == Protocol::Irc && !conversation.joined) {
            tooltip += tr(" · Left channel");
        }
        item->setToolTip(0, tooltip.toHtmlEscaped());
        if (selection && *selection == id) {
            m_tree->setCurrentItem(item);
        }
    }

    for (auto it = sections.cbegin(); it != sections.cend(); ++it) {
        it.value()->setExpanded(!filter.isEmpty() || m_expanded.value(it.key(), true));
    }
    const bool empty = sections.isEmpty();
    m_tree->setVisible(!empty);
    m_empty->setVisible(empty);
    if (!empty && scrollPosition > 0) {
        m_tree->doItemsLayout();
        m_tree->verticalScrollBar()->setValue(scrollPosition);
    }
    if (empty) {
        if (!filter.isEmpty()) {
            m_emptyHeading->setText(tr("No matching conversations"));
            m_emptyDescription->setText(tr("Search by conversation, network or group name."));
            m_emptyAction->setText(tr("Clear search"));
        } else if (accounts.isEmpty()) {
            m_emptyHeading->setText(tr("Connect a messaging account"));
            m_emptyDescription->setText(tr("Add an account to start browsing channels and conversations."));
            m_emptyAction->setText(tr("Add account"));
        } else if (std::ranges::any_of(
                       accounts, [](const Account &account) { return account.protocol == Protocol::Irc; })) {
            m_emptyHeading->setText(tr("No conversations yet"));
            m_emptyDescription->setText(tr("Explore IRC channels, or use + to open a direct message."));
            m_emptyAction->setText(tr("Discover channels"));
        } else {
            m_emptyHeading->setText(tr("No conversations yet"));
            m_emptyDescription->setText(tr("Connect an account to load its conversations."));
            m_emptyAction->setText(tr("Manage accounts"));
        }
    }
    refreshProfile();
    refreshStatus();
}

void MainWindow::refreshProfile()
{
    const auto accounts = m_accounts.accounts();
    if (accounts.isEmpty()) {
        m_avatar->setText(QStringLiteral("P"));
        m_profileName->setText(tr("Local profile"));
        m_profileState->setText(tr("No account configured"));
        m_profileName->setToolTip({});
        m_profileState->setToolTip({});
        return;
    }
    const auto selection = itemConversation(m_tree->currentItem());
    auto selected
        = selection ? std::ranges::find(accounts, selection->accountId, &Account::id) : accounts.end();
    if (selected == accounts.end()) {
        selected = std::ranges::find_if(accounts, [this](const Account &account) {
            return m_accounts.state(account.id) == ConnectionState::Connected;
        });
    }
    const auto &account = selected == accounts.end() ? accounts.front() : *selected;
    const auto name = accountLabel(account);
    const auto state = connectionLabel(m_accounts.state(account.id));
    const auto identity = account.nick.isEmpty() ? name : account.nick;
    m_avatar->setText(identity.isEmpty() ? QStringLiteral("P") : identity.left(1).toUpper());
    m_profileName->setText(identity);
    m_profileName->setToolTip(identity.toHtmlEscaped());
    m_profileState->setText(
        tr("%1 · %2").arg(account.nick.isEmpty() ? protocolLabel(account.protocol) : name, state));
    m_profileState->setToolTip(m_profileState->text().toHtmlEscaped());
}

void MainWindow::refreshStatus()
{
    const auto accounts = m_accounts.accounts();
    const auto connected = std::ranges::count_if(accounts, [this](const Account &account) {
        return m_accounts.state(account.id) == ConnectionState::Connected;
    });
    if (!m_statusTimer->isActive()) {
        const auto status = accounts.isEmpty()
            ? tr("Ready to connect")
            : tr("%1/%2 accounts connected").arg(connected).arg(accounts.size());
        m_status->setText(status);
        m_status->setToolTip(status);
    }
    qint64 unread = 0;
    for (const auto &conversation : m_conversations.conversations()) {
        unread += conversation.unread;
    }
    m_unread->setText(unread == 0 ? tr("All read") : tr("%1 unread").arg(unread));
}

void MainWindow::refreshNetworks()
{
    m_networks->clear();
    const auto accounts = m_accounts.accounts();
    if (accounts.isEmpty()) {
        m_networks->addAction(tr("Add an account…"), this, &MainWindow::showAccounts);
        return;
    }
    for (const auto &account : accounts) {
        const auto state = m_accounts.state(account.id);
        auto *network = m_networks->addMenu(tr("%1 · %2").arg(accountLabel(account), connectionLabel(state)));
        const bool idle = state == ConnectionState::Disconnected || state == ConnectionState::Error;
        auto *connectAction = network->addAction(
            tr("Connect"), this, [this, id = account.id] { m_accounts.connectAccount(id); });
        connectAction->setEnabled(idle);
        auto *disconnectAction = network->addAction(
            tr("Disconnect"), this, [this, id = account.id] { m_accounts.disconnectAccount(id); });
        disconnectAction->setEnabled(state != ConnectionState::Disconnected);
    }
}

void MainWindow::showConversationMenu(const QPoint &position)
{
    const auto id = itemConversation(m_tree->itemAt(position));
    if (!id) {
        return;
    }
    const auto conversation = m_conversations.conversation(*id);
    if (!conversation) {
        return;
    }
    QMenu menu(this);
    menu.addAction(tr("Open conversation"), this, [this, id = *id] { m_windows.open(id); });
    menu.addAction(conversation->favorite ? tr("Remove from favorites") : tr("Add to favorites"), this,
        [this, id = *id, favorite = conversation->favorite] { m_conversations.setFavorite(id, !favorite); });
    menu.addAction(conversation->muted ? tr("Unmute notifications") : tr("Mute notifications"), this,
        [this, id = *id, muted = conversation->muted] { m_conversations.setMuted(id, !muted); });
    menu.addAction(tr("Move to group…"), this, [this, id = *id, currentGroup = conversation->group] {
        bool ok = false;
        const auto group = QInputDialog::getText(this, tr("Custom group"),
            tr("Group name (leave blank to clear)"), QLineEdit::Normal, currentGroup, &ok)
                               .trimmed();
        if (ok) {
            m_conversations.setGroup(id, group.left(100));
        }
    });
    if (conversation->kind == ConversationKind::Channel
        && m_accounts.protocol(id->accountId) == Protocol::Irc) {
        menu.addSeparator();
        auto *membership = menu.addAction(conversation->joined ? tr("Leave channel") : tr("Rejoin channel"),
            this, [this, conversation = *conversation] {
                if (conversation.joined) {
                    m_conversations.partChannel(conversation.id);
                } else {
                    m_conversations.joinChannel(conversation.id.accountId, conversation.title);
                }
            });
        membership->setEnabled(m_accounts.state(id->accountId) == ConnectionState::Connected);
    }
    menu.exec(m_tree->viewport()->mapToGlobal(position));
}

void MainWindow::setGrouping(Grouping grouping)
{
    m_grouping = grouping;
    QSettings().setValue(QLatin1String(kGrouping), static_cast<int>(grouping));
    reloadConversations();
}

std::optional<ConversationId> MainWindow::itemConversation(const QTreeWidgetItem *item) const
{
    if (!item || !item->parent()) {
        return std::nullopt;
    }
    return ConversationId { item->data(0, kAccountRole).toString(), item->data(0, kTargetRole).toString() };
}

} // namespace phoschat::ui
