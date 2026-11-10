#pragma once

#include <QDialog>
#include <QPointer>

class QComboBox;
class QLabel;
class QPushButton;
class QTableView;

namespace phoschat {
class AccountManager;
class ConversationManager;
class IrcProvider;
}

namespace phoschat::ui {

class DirectoryFilter;
class DirectoryTableModel;

class IrcDirectoryDialog final : public QDialog {
    Q_OBJECT

public:
    IrcDirectoryDialog(AccountManager &accounts, ConversationManager &conversations,
        const QString &preferredAccount, QWidget *parent = nullptr);

private:
    QWidget *makeSuggestedTab();
    QWidget *makeLiveTab();
    void refreshAccounts(const QString &preferredAccount = {});
    void bindLiveProvider();
    void syncLiveResults();
    void updateLiveActions();
    void refreshLiveDirectory();
    void updateSuggestionActions();
    void activateSuggestion();
    QString accountForNetwork(QStringView host) const;
    void joinChannel(const QString &accountId, const QString &channel);

    AccountManager &accounts_;
    ConversationManager &conversations_;
    QPointer<IrcProvider> provider_;
    QMetaObject::Connection listConnection_;
    DirectoryTableModel *suggestions_ {};
    DirectoryFilter *suggestionsFilter_ {};
    QTableView *suggestionsView_ {};
    QPushButton *suggestionAction_ {};
    QPushButton *suggestionSource_ {};
    DirectoryTableModel *live_ {};
    DirectoryFilter *liveFilter_ {};
    QTableView *liveView_ {};
    QComboBox *networks_ {};
    QLabel *liveStatus_ {};
    QPushButton *refresh_ {};
    QPushButton *joinLive_ {};
};

} // namespace phoschat::ui
