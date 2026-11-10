#include "app/application_controller.h"

#include "core/account_manager.h"
#include "core/conversation_manager.h"
#include "core/message_manager.h"
#include "notifications/notification_manager.h"
#include "security/credential_store.h"
#include "storage/database.h"
#include "ui/contacts/MainWindow.hpp"
#include "ui/styles/Theme.hpp"
#include "ui/windows/WindowManager.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QLockFile>
#include <QMessageBox>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>

namespace phoschat {

struct ApplicationController::Components {
    explicit Components(const QString &path)
        : lock(path + QStringLiteral(".lock"))
        , database(path)
    {
        lock.setStaleLockTime(0);
    }

    QLockFile lock;
    Database database;
    CredentialStore credentials;
    std::unique_ptr<AccountManager> accounts;
    std::unique_ptr<ConversationManager> conversations;
    std::unique_ptr<MessageManager> messages;
    std::unique_ptr<ui::WindowManager> windows;
    std::unique_ptr<ui::MainWindow> main;
    std::unique_ptr<NotificationManager> notifications;
    QHash<QString, QPointer<QInputDialog>> challenges;
    bool shuttingDown {};
};

ApplicationController::ApplicationController(QApplication &application)
    : application_(application)
{
}
ApplicationController::~ApplicationController() = default;

bool ApplicationController::start()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        QMessageBox::critical(nullptr, tr("PhosChat"), tr("Could not create the local data directory."));
        return false;
    }
#ifndef Q_OS_WIN
    QFile::setPermissions(
        directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    const QString path = QDir(directory).filePath(QStringLiteral("phoschat.sqlite3"));
    auto components = std::make_unique<Components>(path);
    auto &app = *components;
    if (!app.lock.tryLock()) {
        QMessageBox::critical(nullptr, tr("PhosChat"),
            tr("Could not lock the local profile. Check whether PhosChat is already open."));
        return false;
    }
    if (!app.database.open()) {
        QMessageBox::critical(nullptr, tr("PhosChat"),
            tr("Could not open the local database. Check the data directory and database version."));
        return false;
    }
    app.accounts = std::make_unique<AccountManager>(app.database, app.credentials);
    app.conversations = std::make_unique<ConversationManager>(*app.accounts, app.database);
    app.messages = std::make_unique<MessageManager>(*app.accounts, *app.conversations, app.database);
    app.windows = std::make_unique<ui::WindowManager>(*app.accounts, *app.conversations, *app.messages);
    app.main = std::make_unique<ui::MainWindow>(*app.accounts, *app.conversations, *app.windows);
    app.notifications = std::make_unique<NotificationManager>(
        *app.main, *app.windows, *app.conversations, *app.messages, ui::brandIcon());

    application_.setQuitOnLastWindowClosed(false);
    connect(app.messages.get(), &MessageManager::errorOccurred, app.main.get(),
        &ui::MainWindow::showStatusMessage);
    connect(app.conversations.get(), &ConversationManager::errorOccurred, app.main.get(),
        &ui::MainWindow::showStatusMessage);
    connect(app.accounts.get(), &AccountManager::authorizationUrlRequested, app.main.get(),
        [&app](const QUrl &url) {
            if (url.scheme() != QStringLiteral("https") || url.host() != QStringLiteral("slack.com")
                || url.path() != QStringLiteral("/oauth/v2/authorize") || !url.userInfo().isEmpty()
                || (url.port() != -1 && url.port() != 443))
                return;
            if (!QDesktopServices::openUrl(url))
                app.main->showStatusMessage(tr("Could not open the browser for Slack authorization."));
        });
    connect(app.accounts.get(), &AccountManager::accountStateChanged, this,
        [&app](const QString &id, ConnectionState state) {
            if (state != ConnectionState::Authenticating) {
                if (const auto dialog = app.challenges.take(id)) {
                    dialog->hide();
                    dialog->deleteLater();
                }
            }
        });
    connect(app.accounts.get(), &AccountManager::authenticationRequired, this,
        [this, &app](const QString &id, AuthenticationKind kind, const QString &detail) {
            if (app.shuttingDown)
                return;
            if (const auto old = app.challenges.take(id)) {
                old->hide();
                old->deleteLater();
            }

            QString prompt;
            switch (kind) {
            case AuthenticationKind::Phone:
                prompt = tr("Phone number, including country code");
                break;
            case AuthenticationKind::Code:
                prompt = tr("Telegram verification code");
                break;
            case AuthenticationKind::Password:
                prompt = tr("Telegram two-step verification password");
                break;
            case AuthenticationKind::EmailAddress:
                prompt = tr("Verification email address");
                break;
            case AuthenticationKind::EmailCode:
                prompt = tr("Email verification code");
                break;
            case AuthenticationKind::Registration:
            case AuthenticationKind::Unsupported:
                app.main->showStatusMessage(
                    detail.isEmpty() ? tr("This Telegram verification step is unavailable.") : detail);
                app.accounts->disconnectAccount(id);
                return;
            }
            if (!detail.isEmpty())
                prompt += u'\n' + detail;
            auto *parent = QApplication::activeModalWidget();
            auto *dialog = new QInputDialog(parent ? parent : app.main.get());
            dialog->setWindowTitle(tr("Telegram sign-in · PhosChat"));
            dialog->setLabelText(prompt.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>")));
            if (kind == AuthenticationKind::Password || kind == AuthenticationKind::Code
                || kind == AuthenticationKind::EmailCode) {
                dialog->setTextEchoMode(QLineEdit::Password);
                dialog->setInputMethodHints(
                    Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
            }
            app.challenges.insert(id, dialog);
            connect(dialog, &QDialog::finished, this, [&app, dialog, id, kind](int result) {
                if (app.challenges.value(id) != dialog)
                    return;
                app.challenges.remove(id);
                const auto value = dialog->textValue();
                dialog->deleteLater();
                if (result == QDialog::Accepted && !value.isEmpty())
                    app.accounts->submitAuthentication(id, kind, value);
                else
                    app.accounts->disconnectAccount(id);
            });
            dialog->open();
        });
    const auto shutdown = [&app] {
        if (app.shuttingDown)
            return;
        app.shuttingDown = true;
        app.main->prepareToQuit();
        app.windows->closeAll();
        app.accounts->shutdown();
    };
    connect(app.main.get(), &ui::MainWindow::quitRequested, this, [this, shutdown] {
        shutdown();
        QTimer::singleShot(0, &application_, &QApplication::quit);
    });
    connect(&application_, &QApplication::aboutToQuit, this, shutdown);
    app.main->show();
    app.windows->restoreSession();
    QTimer::singleShot(0, app.accounts.get(), &AccountManager::startAutomaticConnections);
    components_ = std::move(components);
    return true;
}

} // namespace phoschat
