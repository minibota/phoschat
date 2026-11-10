#pragma once

#include <QHash>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>

namespace phoschat {

class SlackOAuth final : public QObject {
    Q_OBJECT

public:
    explicit SlackOAuth(QObject *parent = nullptr);
    bool start(const QString &clientId, quint16 port, const QUrl &authorizationEndpoint,
        const QUrl &callbackOrigin = {});
    void stop();

signals:
    void authorizationRequested(const QUrl &url);
    void authorized(const QString &code, const QString &verifier, const QUrl &redirect);
    void failed(const QString &message);

private:
    void acceptConnections();
    void readRequest(QTcpSocket &socket);
    void respond(QTcpSocket &socket, QByteArrayView status, QByteArrayView message);
    void stopListening(QTcpSocket *completingSocket);

    QTcpServer server_;
    QTimer timeout_;
    QHash<QTcpSocket *, QByteArray> requests_;
    QString state_;
    QString verifier_;
    QUrl redirect_;
};

} // namespace phoschat
