#include "security/credential_store.h"

#include <QTimer>

#ifdef PHOSCHAT_WITH_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

#include <utility>

namespace phoschat {
namespace {

    using namespace Qt::StringLiterals;

#ifdef PHOSCHAT_WITH_KEYCHAIN
    const auto ServiceName = u"PhosChat"_s;

    void configureJob(QKeychain::Job &job, const QString &accountId)
    {
        job.setKey(u"account/"_s + accountId);
        job.setInsecureFallback(false);
    }

    QString keychainError(QKeychain::Error error)
    {
        switch (error) {
        case QKeychain::NoError:
            return {};
        case QKeychain::EntryNotFound:
            return CredentialStore::tr("No saved credential was found for this account.");
        case QKeychain::AccessDenied:
        case QKeychain::AccessDeniedByUser:
            return CredentialStore::tr("Access to the system keychain was denied.");
        case QKeychain::NoBackendAvailable:
        case QKeychain::NotImplemented:
            return CredentialStore::tr("The system keychain is unavailable in this session.");
        case QKeychain::CouldNotDeleteEntry:
            return CredentialStore::tr("The system keychain could not remove the saved credential.");
        default:
            return CredentialStore::tr("The system keychain operation failed.");
        }
    }

    void finishWrite(CredentialStore *store, QKeychain::Job *job, CredentialStore::WriteCallback callback,
        bool missingIsSuccess = false)
    {
        QObject::connect(job, &QKeychain::Job::finished, store,
            [store, callback = std::move(callback), missingIsSuccess](auto *finished) {
                const auto error = finished->error();
                const auto message = missingIsSuccess && error == QKeychain::EntryNotFound
                    ? QString {}
                    : keychainError(error);
                QTimer::singleShot(0, store, [callback, message] { callback(message); });
            });
        job->start();
    }
#else
    QString backendUnavailable() { return CredentialStore::tr("This build has no system keychain support."); }
#endif

} // namespace

bool CredentialStore::persistentAvailable() const
{
#ifdef PHOSCHAT_WITH_KEYCHAIN
    return true;
#else
    return false;
#endif
}

void CredentialStore::read(const QString &accountId, ReadCallback callback)
{
#ifdef PHOSCHAT_WITH_KEYCHAIN
    auto *job = new QKeychain::ReadPasswordJob(ServiceName, this);
    configureJob(*job, accountId);
    connect(job, &QKeychain::Job::finished, this, [this, job, callback = std::move(callback)] {
        const auto error = keychainError(job->error());
        const auto secret = error.isEmpty() ? job->binaryData() : QByteArray {};
        QTimer::singleShot(0, this, [callback, secret, error] { callback(secret, error); });
    });
    job->start();
#else
    Q_UNUSED(accountId);
    QTimer::singleShot(0, this, [callback = std::move(callback)] { callback({}, backendUnavailable()); });
#endif
}

void CredentialStore::write(const QString &accountId, const QByteArray &secret, WriteCallback callback)
{
#ifdef PHOSCHAT_WITH_KEYCHAIN
    auto *job = new QKeychain::WritePasswordJob(ServiceName, this);
    configureJob(*job, accountId);
    job->setBinaryData(secret);
    finishWrite(this, job, std::move(callback));
#else
    Q_UNUSED(accountId);
    Q_UNUSED(secret);
    QTimer::singleShot(0, this, [callback = std::move(callback)] { callback(backendUnavailable()); });
#endif
}

void CredentialStore::remove(const QString &accountId, WriteCallback callback)
{
#ifdef PHOSCHAT_WITH_KEYCHAIN
    auto *job = new QKeychain::DeletePasswordJob(ServiceName, this);
    configureJob(*job, accountId);
    finishWrite(this, job, std::move(callback), true);
#else
    Q_UNUSED(accountId);
    QTimer::singleShot(0, this, [callback = std::move(callback)] { callback(backendUnavailable()); });
#endif
}

} // namespace phoschat
