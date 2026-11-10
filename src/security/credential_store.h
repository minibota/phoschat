#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

namespace phoschat {

class CredentialStore : public QObject {
    Q_OBJECT

public:
    using ReadCallback = std::function<void(QByteArray, QString)>;
    using WriteCallback = std::function<void(QString)>;

    using QObject::QObject;

    // Reports build support. The desktop keychain may still reject an operation.
    [[nodiscard]] virtual bool persistentAvailable() const;
    virtual void read(const QString &accountId, ReadCallback callback);
    virtual void write(const QString &accountId, const QByteArray &secret, WriteCallback callback);
    virtual void remove(const QString &accountId, WriteCallback callback);
};

} // namespace phoschat
