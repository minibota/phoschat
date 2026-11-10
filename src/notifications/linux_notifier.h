#pragma once

#include <QDBusConnection>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QVariant>

namespace phoschat {

// Both interfaces work independently of a system tray. The portal preserves
// Flatpak's sandbox without granting access to the host notification service.
class LinuxNotifier final : public QObject {
    Q_OBJECT
public:
    enum class Backend { Desktop, Portal };
    explicit LinuxNotifier(QObject *parent = nullptr);
    LinuxNotifier(QDBusConnection bus, Backend backend, QObject *parent = nullptr);
    ~LinuxNotifier() override;

    void post(const QString &id, const QString &title, const QString &body);
    void withdraw(const QString &id);

signals:
    void activated(const QString &id);
    void failed(const QString &id);

private slots:
    void desktopAction(uint id, const QString &action);
    void desktopClosed(uint id, uint reason);
    void portalAction(const QString &id, const QString &action, const QVariantList &parameters);

private:
    void closeDesktop(uint id);

    QDBusConnection bus_;
    Backend backend_;
    QSet<QString> active_;
    QSet<QString> pending_;
    QHash<QString, QPair<QString, QString>> queued_;
    QHash<QString, uint> desktopIds_;
};

} // namespace phoschat
