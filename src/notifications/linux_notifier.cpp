#include "notifications/linux_notifier.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>

#include <utility>

namespace phoschat {
namespace {
    using namespace Qt::StringLiterals;
    const auto DesktopService = u"org.freedesktop.Notifications"_s;
    const auto DesktopPath = u"/org/freedesktop/Notifications"_s;
    const auto PortalService = u"org.freedesktop.portal.Desktop"_s;
    const auto PortalPath = u"/org/freedesktop/portal/desktop"_s;
    const auto PortalInterface = u"org.freedesktop.portal.Notification"_s;
    const auto ApplicationId = u"io.phoschat.desktop"_s;
}

LinuxNotifier::LinuxNotifier(QObject *parent)
    : LinuxNotifier(QDBusConnection::sessionBus(),
          QFile::exists(QStringLiteral("/.flatpak-info")) ? Backend::Portal : Backend::Desktop, parent)
{
}

LinuxNotifier::LinuxNotifier(QDBusConnection bus, Backend backend, QObject *parent)
    : QObject(parent)
    , bus_(std::move(bus))
    , backend_(backend)
{
    if (backend_ == Backend::Portal) {
        bus_.connect(PortalService, PortalPath, PortalInterface, u"ActionInvoked"_s, this,
            SLOT(portalAction(QString, QString, QVariantList)));
    } else {
        bus_.connect(DesktopService, DesktopPath, DesktopService, u"ActionInvoked"_s, this,
            SLOT(desktopAction(uint, QString)));
        bus_.connect(DesktopService, DesktopPath, DesktopService, u"NotificationClosed"_s, this,
            SLOT(desktopClosed(uint, uint)));
    }
}

LinuxNotifier::~LinuxNotifier()
{
    const auto ids = active_.values();
    for (const auto &id : ids)
        withdraw(id);
}

void LinuxNotifier::post(const QString &id, const QString &title, const QString &body)
{
    active_.insert(id);
    if (pending_.contains(id)) {
        queued_.insert(id, { title, body });
        return;
    }
    pending_.insert(id);
    auto call = backend_ == Backend::Portal
        ? QDBusMessage::createMethodCall(PortalService, PortalPath, PortalInterface, u"AddNotification"_s)
        : QDBusMessage::createMethodCall(DesktopService, DesktopPath, DesktopService, u"Notify"_s);
    if (backend_ == Backend::Portal) {
        // These fields are supported by version 1; no optional v2 API is needed.
        const QVariantMap notification { { u"title"_s, title }, { u"body"_s, body },
            { u"icon"_s, ApplicationId }, { u"priority"_s, u"normal"_s },
            { u"default-action"_s, u"open"_s } };
        call.setArguments({ id, notification });
    } else {
        const QVariantMap hints { { u"desktop-entry"_s, ApplicationId },
            { u"category"_s, u"im.received"_s } };
        call.setArguments({ u"PhosChat"_s, desktopIds_.value(id), ApplicationId, title, body,
            QStringList { u"default"_s, tr("Open conversation") }, hints, -1 });
    }
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(call, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, id](QDBusPendingCallWatcher *finished) {
        const auto reply = finished->reply();
        finished->deleteLater();
        pending_.remove(id);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            queued_.remove(id);
            if (active_.remove(id))
                emit failed(id);
            return;
        }
        if (backend_ == Backend::Desktop) {
            const QDBusPendingReply<uint> result = *finished;
            if (result.isError() || result.value() == 0) {
                queued_.remove(id);
                if (active_.remove(id))
                    emit failed(id);
                return;
            }
            if (active_.contains(id))
                desktopIds_.insert(id, result.value());
            else
                closeDesktop(result.value());
        }
        if (!active_.contains(id)) {
            queued_.remove(id);
        } else if (queued_.contains(id)) {
            const auto latest = queued_.take(id);
            post(id, latest.first, latest.second);
        }
    });
}

void LinuxNotifier::withdraw(const QString &id)
{
    active_.remove(id);
    queued_.remove(id);
    if (backend_ == Backend::Portal) {
        auto call = QDBusMessage::createMethodCall(
            PortalService, PortalPath, PortalInterface, u"RemoveNotification"_s);
        call.setArguments({ id });
        bus_.asyncCall(call, 5000);
    } else if (const auto nativeId = desktopIds_.take(id)) {
        closeDesktop(nativeId);
    }
}

void LinuxNotifier::closeDesktop(uint id)
{
    auto call
        = QDBusMessage::createMethodCall(DesktopService, DesktopPath, DesktopService, u"CloseNotification"_s);
    call.setArguments({ id });
    bus_.asyncCall(call, 5000);
}

void LinuxNotifier::desktopAction(uint id, const QString &action)
{
    if (action != u"default")
        return;
    const auto local = desktopIds_.key(id);
    if (!local.isEmpty() && active_.contains(local)) {
        emit activated(local);
        withdraw(local);
    }
}

void LinuxNotifier::desktopClosed(uint id, uint)
{
    const auto local = desktopIds_.key(id);
    if (local.isEmpty())
        return;
    desktopIds_.remove(local);
    active_.remove(local);
}

void LinuxNotifier::portalAction(const QString &id, const QString &action, const QVariantList &)
{
    if (action == u"open" && active_.contains(id)) {
        emit activated(id);
        withdraw(id);
    }
}

} // namespace phoschat
