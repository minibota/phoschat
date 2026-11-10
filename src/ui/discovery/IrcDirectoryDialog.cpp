#include "ui/discovery/IrcDirectoryDialog.hpp"

#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "providers/irc/irc_provider.h"
#include "ui/Presentation.hpp"
#include "ui/settings/AccountDialog.hpp"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace phoschat::ui {
namespace {

    struct DirectoryRow {
        QString channel;
        QString network;
        QString category;
        QString description;
        QString source;
        int users {};
    };

    QLabel *descriptionLabel(const QString &text)
    {
        auto *label = new QLabel(text);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setProperty("role", QStringLiteral("muted"));
        return label;
    }

    const irc::NetworkSuggestion *networkPreset(QStringView host)
    {
        const auto choices = irc::recommendedNetworks();
        const auto it = std::ranges::find(choices, host, &irc::NetworkSuggestion::host);
        return it == choices.end() ? nullptr : &*it;
    }

    bool sameNetwork(QStringView accountHost, QStringView presetHost)
    {
        if (accountHost.compare(presetHost, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (presetHost == u"irc.libera.chat") {
            return accountHost.endsWith(u".libera.chat", Qt::CaseInsensitive);
        }
        if (presetHost == u"irc.hackint.org") {
            return accountHost.compare(u"irc.eu.hackint.org", Qt::CaseInsensitive) == 0;
        }
        if (presetHost == u"ssl.chathispano.com") {
            return accountHost.compare(u"irc.chathispano.com", Qt::CaseInsensitive) == 0
                || accountHost.compare(u"irc.irc-hispano.org", Qt::CaseInsensitive) == 0;
        }
        return presetHost == u"irc.oftc.net"
            && accountHost.compare(u"irc.debian.org", Qt::CaseInsensitive) == 0;
    }

    QTableView *makeTable(QAbstractItemModel *model)
    {
        auto *table = new QTableView;
        table->setModel(model);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setWordWrap(false);
        table->setShowGrid(false);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(30);
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setMinimumSectionSize(58);
        table->setColumnWidth(0, 140);
        table->setColumnWidth(1, 94);
        table->setSortingEnabled(true);
        return table;
    }

} // namespace

class DirectoryTableModel final : public QAbstractTableModel {
public:
    DirectoryTableModel(bool curated, QObject *parent)
        : QAbstractTableModel(parent)
        , curated_(curated)
    {
        if (!curated_) {
            return;
        }
        for (const auto &entry : irc::recommendedChannels()) {
            rows_.append({ entry.name.toString(), entry.networkHost.toString(), entry.category.toString(),
                entry.description.toString(), entry.source.toString(), 0 });
        }
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : (curated_ ? 4 : 3);
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case 0:
            return IrcDirectoryDialog::tr("Channel");
        case 1:
            return curated_ ? IrcDirectoryDialog::tr("Network") : IrcDirectoryDialog::tr("Users");
        case 2:
            return curated_ ? IrcDirectoryDialog::tr("Category") : IrcDirectoryDialog::tr("Topic");
        case 3:
            return IrcDirectoryDialog::tr("About");
        default:
            return {};
        }
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        const auto *row = rowAt(index.row());
        if (!row || !index.isValid()) {
            return {};
        }
        if (role == Qt::ToolTipRole && index.column() >= 2) {
            return row->description.toHtmlEscaped();
        }
        if (role == Qt::TextAlignmentRole && !curated_ && index.column() == 1) {
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (index.column()) {
        case 0:
            return row->channel;
        case 1: {
            if (!curated_) {
                return row->users;
            }
            const auto *preset = networkPreset(row->network);
            return preset ? preset->name.toString() : row->network;
        }
        case 2:
            return curated_ ? row->category : row->description;
        case 3:
            return row->description;
        default:
            return {};
        }
    }

    const DirectoryRow *rowAt(int index) const
    {
        return index >= 0 && index < rows_.size() ? &rows_.at(index) : nullptr;
    }

    void resetLive()
    {
        beginResetModel();
        rows_.clear();
        endResetModel();
    }

    void syncLive(const QList<irc::ChannelListing> &entries)
    {
        if (rows_.size() > entries.size()) {
            resetLive();
        }
        const auto first = rows_.size();
        if (first == entries.size()) {
            return;
        }
        beginInsertRows({}, static_cast<int>(first), static_cast<int>(entries.size() - 1));
        for (auto index = first; index < entries.size(); ++index) {
            const auto &entry = entries.at(index);
            rows_.append({ entry.name, {}, {}, entry.topic, {}, entry.users });
        }
        endInsertRows();
    }

private:
    const bool curated_;
    QList<DirectoryRow> rows_;
};

class DirectoryFilter final : public QSortFilterProxyModel {
public:
    DirectoryFilter(DirectoryTableModel *model, QObject *parent)
        : QSortFilterProxyModel(parent)
        , model_(model)
    {
        setSourceModel(model);
        setSortCaseSensitivity(Qt::CaseInsensitive);
        setSortLocaleAware(true);
    }

    void setSearch(const QString &search)
    {
        search_ = search;
        invalidateRowsFilter();
    }

    void setNetwork(const QString &network)
    {
        network_ = network;
        invalidateRowsFilter();
    }

    void setMinimumUsers(int minimumUsers)
    {
        minimumUsers_ = minimumUsers;
        invalidateRowsFilter();
    }

    const DirectoryRow *selection(const QTableView *view) const
    {
        return model_->rowAt(mapToSource(view->currentIndex()).row());
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &) const override
    {
        const auto *row = model_->rowAt(sourceRow);
        if (!row || row->users < minimumUsers_ || (!network_.isEmpty() && network_ != row->network)) {
            return false;
        }
        const auto *network = networkPreset(row->network);
        return irc::matchesDirectorySearch(search_,
            { row->channel, row->network, row->category, row->description,
                network ? network->name : QStringView {} });
    }

private:
    DirectoryTableModel *model_;
    QString search_;
    QString network_;
    int minimumUsers_ {};
};

IrcDirectoryDialog::IrcDirectoryDialog(AccountManager &accounts, ConversationManager &conversations,
    const QString &preferredAccount, QWidget *parent)
    : QDialog(parent)
    , accounts_(accounts)
    , conversations_(conversations)
{
    setWindowTitle(tr("Discover IRC channels"));
    resize(800, 525);
    setMinimumSize(555, 375);

    auto *content = new QVBoxLayout(this);
    auto *title = new QLabel(tr("Discover IRC channels"));
    title->setProperty("role", QStringLiteral("heading"));
    content->addWidget(title);

    auto *tabs = new QTabWidget;
    tabs->addTab(makeSuggestedTab(), tr("Suggestions"));
    tabs->addTab(makeLiveTab(), tr("Live directory"));
    content->addWidget(tabs, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    content->addWidget(buttons);

    connect(&accounts_, &AccountManager::accountsChanged, this, [this] { refreshAccounts(); });
    connect(&accounts_, &AccountManager::accountStateChanged, this, [this] { refreshAccounts(); });
    refreshAccounts(preferredAccount);
}

QWidget *IrcDirectoryDialog::makeSuggestedTab()
{
    auto *tab = new QWidget;
    auto *layout = new QVBoxLayout(tab);
    layout->addWidget(descriptionLabel(
        tr("Browse a hand-picked set of communities from official project and network guides.")));

    suggestions_ = new DirectoryTableModel(true, this);
    suggestionsFilter_ = new DirectoryFilter(suggestions_, this);
    auto *filters = new QHBoxLayout;
    auto *search = new QLineEdit;
    search->setMaxLength(120);
    search->setPlaceholderText(tr("Search channels, interests or languages…"));
    search->setAccessibleName(tr("Search suggested IRC channels"));
    filters->addWidget(search, 1);
    auto *preset = new QComboBox;
    preset->setAccessibleName(tr("Suggested IRC networks"));
    preset->addItem(tr("All networks"));
    for (const auto &choice : irc::recommendedNetworks()) {
        preset->addItem(choice.name.toString(), choice.host.toString());
        preset->setItemData(preset->count() - 1,
            QStringLiteral("%1:%2 · TLS").arg(choice.host.toString()).arg(choice.port), Qt::ToolTipRole);
    }
    filters->addWidget(preset);
    layout->addLayout(filters);

    suggestionsView_ = makeTable(suggestionsFilter_);
    suggestionsView_->setAccessibleName(tr("Suggested IRC channels"));
    suggestionsView_->setColumnWidth(2, 153);
    suggestionsView_->sortByColumn(0, Qt::AscendingOrder);
    layout->addWidget(suggestionsView_, 1);

    auto *actions = new QHBoxLayout;
    suggestionSource_ = new QPushButton(tr("Official guide"));
    suggestionSource_->setAutoDefault(false);
    actions->addWidget(suggestionSource_);
    actions->addStretch();
    suggestionAction_ = new QPushButton;
    suggestionAction_->setAutoDefault(false);
    suggestionAction_->setProperty("role", QStringLiteral("primary"));
    actions->addWidget(suggestionAction_);
    layout->addLayout(actions);

    connect(search, &QLineEdit::textChanged, this, [this](const QString &value) {
        suggestionsFilter_->setSearch(value);
        if (!suggestionsView_->currentIndex().isValid()) {
            suggestionsView_->selectRow(0);
        }
        updateSuggestionActions();
    });
    connect(preset, &QComboBox::currentIndexChanged, this, [this, preset] {
        suggestionsFilter_->setNetwork(preset->currentData().toString());
        if (!suggestionsView_->currentIndex().isValid()) {
            suggestionsView_->selectRow(0);
        }
        updateSuggestionActions();
    });
    connect(suggestionsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        &IrcDirectoryDialog::updateSuggestionActions);
    connect(suggestionAction_, &QPushButton::clicked, this, &IrcDirectoryDialog::activateSuggestion);
    connect(suggestionsView_, &QTableView::doubleClicked, this, &IrcDirectoryDialog::activateSuggestion);
    connect(suggestionSource_, &QPushButton::clicked, this, [this] {
        if (const auto *row = suggestionsFilter_->selection(suggestionsView_)) {
            QDesktopServices::openUrl(QUrl(row->source));
        }
    });
    suggestionsView_->selectRow(0);
    return tab;
}

QWidget *IrcDirectoryDialog::makeLiveTab()
{
    auto *tab = new QWidget;
    auto *layout = new QVBoxLayout(tab);
    layout->addWidget(
        descriptionLabel(tr("Load the public channels advertised by a connected IRC network.")));
    auto *networkRow = new QHBoxLayout;
    networks_ = new QComboBox;
    networks_->setAccessibleName(tr("IRC account for live directory"));
    networkRow->addWidget(networks_, 1);
    refresh_ = new QPushButton(tr("Load channels"));
    refresh_->setAutoDefault(false);
    networkRow->addWidget(refresh_);
    layout->addLayout(networkRow);

    live_ = new DirectoryTableModel(false, this);
    liveFilter_ = new DirectoryFilter(live_, this);
    auto *filters = new QHBoxLayout;
    auto *search = new QLineEdit;
    search->setMaxLength(120);
    search->setPlaceholderText(tr("Search by channel name or topic…"));
    search->setAccessibleName(tr("Search live IRC channels"));
    filters->addWidget(search, 1);
    auto *minimumUsers = new QSpinBox;
    minimumUsers->setRange(0, 1000000);
    minimumUsers->setPrefix(tr("Min. users: "));
    minimumUsers->setAccessibleName(tr("Minimum users"));
    filters->addWidget(minimumUsers);
    layout->addLayout(filters);

    liveView_ = makeTable(liveFilter_);
    liveView_->setAccessibleName(tr("Live IRC channel results"));
    liveView_->sortByColumn(1, Qt::DescendingOrder);
    layout->addWidget(liveView_, 1);
    auto *actions = new QHBoxLayout;
    liveStatus_ = descriptionLabel({});
    actions->addWidget(liveStatus_, 1);
    joinLive_ = new QPushButton(tr("Join selected"));
    joinLive_->setAutoDefault(false);
    joinLive_->setProperty("role", QStringLiteral("primary"));
    actions->addWidget(joinLive_);
    layout->addLayout(actions);

    connect(networks_, &QComboBox::currentIndexChanged, this, &IrcDirectoryDialog::bindLiveProvider);
    connect(refresh_, &QPushButton::clicked, this, &IrcDirectoryDialog::refreshLiveDirectory);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &value) {
        liveFilter_->setSearch(value);
        updateLiveActions();
    });
    connect(minimumUsers, &QSpinBox::valueChanged, this, [this](int value) {
        liveFilter_->setMinimumUsers(value);
        updateLiveActions();
    });
    connect(liveView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        &IrcDirectoryDialog::updateLiveActions);
    auto join = [this] {
        if (const auto *row = liveFilter_->selection(liveView_)) {
            joinChannel(networks_->currentData().toString(), row->channel);
        }
    };
    connect(joinLive_, &QPushButton::clicked, this, join);
    connect(liveView_, &QTableView::doubleClicked, this, join);
    return tab;
}

void IrcDirectoryDialog::refreshAccounts(const QString &preferredAccount)
{
    const auto selection
        = preferredAccount.isEmpty() ? networks_->currentData().toString() : preferredAccount;
    const QSignalBlocker blocker(networks_);
    networks_->clear();
    int firstOnline = -1;
    for (const auto &account : accounts_.accounts()) {
        if (account.protocol != Protocol::Irc) {
            continue;
        }
        const auto state = accounts_.state(account.id);
        networks_->addItem(tr("%1 · %2").arg(accountLabel(account), connectionLabel(state)), account.id);
        if (firstOnline < 0 && state == ConnectionState::Connected) {
            firstOnline = networks_->count() - 1;
        }
    }
    const int existingIndex = networks_->findData(selection);
    networks_->setCurrentIndex(existingIndex >= 0 ? existingIndex : std::max(0, firstOnline));
    bindLiveProvider();
    updateSuggestionActions();
}

void IrcDirectoryDialog::bindLiveProvider()
{
    auto *next = qobject_cast<IrcProvider *>(accounts_.provider(networks_->currentData().toString()));
    if (provider_ != next) {
        disconnect(listConnection_);
        provider_ = next;
        live_->resetLive();
        if (provider_) {
            listConnection_ = connect(
                provider_, &IrcProvider::channelListChanged, this, &IrcDirectoryDialog::syncLiveResults);
        }
    }
    syncLiveResults();
}

void IrcDirectoryDialog::syncLiveResults()
{
    if (provider_) {
        live_->syncLive(provider_->channelList());
    }
    updateLiveActions();
}

void IrcDirectoryDialog::updateLiveActions()
{
    const auto account = networks_->currentData().toString();
    const auto connection = accounts_.state(account);
    const bool online = provider_ && connection == ConnectionState::Connected;
    const bool canConnect = !account.isEmpty()
        && (connection == ConnectionState::Disconnected || connection == ConnectionState::Error);
    const auto status = provider_ ? provider_->channelListStatus() : irc::ChannelListStatus::NotRequested;
    refresh_->setText(canConnect                             ? tr("Connect network")
            : status == irc::ChannelListStatus::NotRequested ? tr("Load channels")
                                                             : tr("Refresh"));
    refresh_->setEnabled(canConnect || (online && !provider_->channelListBusy()));
    joinLive_->setEnabled(online && liveFilter_->selection(liveView_));

    if (!online) {
        liveStatus_->setText(networks_->count() == 0
                ? tr("Add an IRC network from Suggestions.")
                : tr("Connect this IRC network before loading channels."));
        return;
    }
    const auto count = tr("%1 matching of %2 channels").arg(liveFilter_->rowCount()).arg(live_->rowCount());
    switch (status) {
    case irc::ChannelListStatus::NotRequested:
        liveStatus_->setText(tr("Choose Load channels to request the network directory."));
        break;
    case irc::ChannelListStatus::Loading:
        liveStatus_->setText(tr("Loading channels… %1").arg(count));
        break;
    case irc::ChannelListStatus::Ready:
        liveStatus_->setText(count);
        break;
    case irc::ChannelListStatus::Truncated:
        liveStatus_->setText(tr("%1 · Display limit reached").arg(count));
        break;
    case irc::ChannelListStatus::TimedOut:
        liveStatus_->setText(provider_->channelListBusy()
                ? tr("%1 · Server timed out. Reconnect to retry.").arg(count)
                : tr("%1 · Server timed out. You can refresh.").arg(count));
        break;
    case irc::ChannelListStatus::Rejected:
        liveStatus_->setText(tr("The server rejected the channel directory request. Try again later."));
        break;
    case irc::ChannelListStatus::Interrupted:
        liveStatus_->setText(tr("%1 · Previous request was interrupted. Refresh to try again.").arg(count));
        break;
    }
}

void IrcDirectoryDialog::refreshLiveDirectory()
{
    const auto account = networks_->currentData().toString();
    const auto state = accounts_.state(account);
    if (!account.isEmpty() && (state == ConnectionState::Disconnected || state == ConnectionState::Error)) {
        accounts_.connectAccount(account);
        return;
    }
    if (provider_) {
        const auto error = provider_->requestChannelList();
        if (!error.isEmpty()) {
            liveStatus_->setText(error);
        }
    }
}

void IrcDirectoryDialog::updateSuggestionActions()
{
    const auto *row = suggestionsFilter_->selection(suggestionsView_);
    suggestionSource_->setEnabled(row);
    suggestionAction_->setEnabled(row);
    const auto account = row ? accountForNetwork(row->network) : QString {};
    const auto state = accounts_.state(account);
    suggestionAction_->setText(account.isEmpty()  ? tr("Add network…")
            : state == ConnectionState::Connected ? tr("Join selected")
                                                  : tr("Connect network"));
    if (!account.isEmpty() && state != ConnectionState::Connected && state != ConnectionState::Disconnected
        && state != ConnectionState::Error) {
        suggestionAction_->setEnabled(false);
    }
}

void IrcDirectoryDialog::activateSuggestion()
{
    if (!suggestionAction_->isEnabled()) {
        return;
    }
    const auto *row = suggestionsFilter_->selection(suggestionsView_);
    if (!row) {
        return;
    }
    const auto account = accountForNetwork(row->network);
    if (accounts_.state(account) == ConnectionState::Connected) {
        joinChannel(account, row->channel);
    } else if (!account.isEmpty()) {
        accounts_.connectAccount(account);
    } else if (const auto *preset = networkPreset(row->network)) {
        AccountDialog dialog(accounts_, this);
        dialog.prefillIrcNetwork(preset->name.toString(), preset->host.toString(), preset->port);
        dialog.exec();
    }
}

QString IrcDirectoryDialog::accountForNetwork(QStringView host) const
{
    QString first;
    for (const auto &account : accounts_.accounts()) {
        if (account.protocol != Protocol::Irc || !sameNetwork(account.host, host)) {
            continue;
        }
        if (accounts_.state(account.id) == ConnectionState::Connected) {
            return account.id;
        }
        if (first.isEmpty()) {
            first = account.id;
        }
    }
    return first;
}

void IrcDirectoryDialog::joinChannel(const QString &accountId, const QString &channel)
{
    if (accounts_.state(accountId) == ConnectionState::Connected
        && conversations_.joinChannel(accountId, channel)) {
        accept();
    }
}

} // namespace phoschat::ui
