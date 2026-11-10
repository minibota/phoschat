#pragma once

#include <QJsonObject>
#include <QObject>

#include <memory>

class QTemporaryDir;

namespace phoschat {

class TelegramTransport : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~TelegramTransport() override = default;

    static TelegramTransport &native();

    virtual bool isAvailable() const = 0;
    virtual int createClient(const QString &profile, std::shared_ptr<QTemporaryDir> temporary) = 0;
    virtual void send(int client, const QJsonObject &request) = 0;
    virtual void closeClient(int client) = 0;
    virtual void forgetProfile(const QString &profile) = 0;

signals:
    void received(int client, const QJsonObject &event);
};

} // namespace phoschat
