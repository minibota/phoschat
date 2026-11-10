#include "providers/telegram/telegram_transport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QLibrary>
#include <QMutex>
#include <QPointer>
#include <QStringList>
#include <QTemporaryDir>
#include <QVersionNumber>
#include <QWaitCondition>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace phoschat {
namespace {

    using namespace Qt::StringLiterals;
    using namespace std::chrono_literals;

    QStringList libraryCandidates()
    {
        const QDir executable(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
        return { executable.absoluteFilePath(u"tdjson.dll"_s) };
#elif defined(Q_OS_MACOS)
        return { executable.absoluteFilePath(u"../Frameworks/libtdjson.dylib"_s),
            executable.absoluteFilePath(u"libtdjson.dylib"_s), u"libtdjson.dylib"_s };
#else
        return { executable.absoluteFilePath(u"../lib/libtdjson.so"_s),
            executable.absoluteFilePath(u"../lib/phoschat/libtdjson.so"_s),
            executable.absoluteFilePath(u"../lib64/phoschat/libtdjson.so"_s),
            executable.absoluteFilePath(u"libtdjson.so"_s), u"libtdjson.so.1.8.38"_s, u"libtdjson.so"_s };
#endif
    }

    void removeProfile(const QString &path)
    {
        const QFileInfo profile(path);
        if (profile.isAbsolute() && !profile.isSymLink() && !QFileInfo(profile.absolutePath()).isSymLink())
            QDir(path).removeRecursively();
    }

    class NativeTelegramTransport final : public TelegramTransport {
    public:
        explicit NativeTelegramTransport(QObject *parent)
            : TelegramTransport(parent)
        {
            for (const auto &candidate : libraryCandidates()) {
                auto library = std::make_unique<QLibrary>(candidate);
                library->setLoadHints(QLibrary::PreventUnloadHint);
                if (!library->load())
                    continue;
                const auto create = reinterpret_cast<Create>(library->resolve("td_create_client_id"));
                const auto send = reinterpret_cast<Send>(library->resolve("td_send"));
                const auto receive = reinterpret_cast<Receive>(library->resolve("td_receive"));
                const auto execute = reinterpret_cast<Execute>(library->resolve("td_execute"));
                if (!create || !send || !receive || !execute)
                    continue;

                const char *response = execute(R"({"@type":"getOption","name":"version"})");
                const auto version = response
                    ? QJsonDocument::fromJson(QByteArray(response)).object().value(u"value"_s).toString()
                    : QString {};
                if (QVersionNumber::fromString(version) < QVersionNumber(1, 8, 38))
                    continue;
                execute(R"({"@type":"setLogStream","log_stream":{"@type":"logStreamEmpty"}})");
                execute(R"({"@type":"setLogVerbosityLevel","new_verbosity_level":0})");
                library_ = std::move(library);
                create_ = create;
                send_ = send;
                receive_ = receive;
                break;
            }
        }

        ~NativeTelegramTransport() override
        {
            QList<int> ids;
            {
                const QMutexLocker lock(&mutex_);
                ids = clients_.keys();
            }
            for (const int id : ids)
                closeClient(id);
            if (worker_.joinable()) {
                stopping_.store(true);
                {
                    const QMutexLocker lock(&mutex_);
                    clientsChanged_.wakeOne();
                }
                worker_.join();
            }
        }

        bool isAvailable() const override { return library_ != nullptr; }

        int createClient(const QString &profile, std::shared_ptr<QTemporaryDir> temporary) override
        {
            if (!isAvailable())
                return 0;

            const QMutexLocker lock(&mutex_);
            for (const auto &client : std::as_const(clients_)) {
                if (client.profile == profile)
                    return 0;
            }
            const int id = create_();
            if (id > 0) {
                clients_.insert(id, Client { profile, std::move(temporary) });
                if (!worker_.joinable())
                    worker_ = std::thread([this] { receiveLoop(); });
                clientsChanged_.wakeOne();
            }
            return id;
        }

        void send(int client, const QJsonObject &request) override
        {
            const auto encoded = QJsonDocument(request).toJson(QJsonDocument::Compact);
            send_(client, encoded.constData());
        }

        void closeClient(int id) override
        {
            {
                const QMutexLocker lock(&mutex_);
                auto client = clients_.find(id);
                if (client == clients_.end() || client->closing)
                    return;
                client->closing = true;
            }
            send(id, { { u"@type"_s, u"close"_s } });
        }

        void forgetProfile(const QString &profile) override
        {
            const QMutexLocker lock(&mutex_);
            for (auto &client : clients_) {
                if (client.profile == profile) {
                    client.eraseAfterClose = true;
                    return;
                }
            }
            removeProfile(profile);
        }

    private:
        using Create = int (*)();
        using Send = void (*)(int, const char *);
        using Receive = const char *(*)(double);
        using Execute = const char *(*)(const char *);
        using Clock = std::chrono::steady_clock;

        struct Client {
            QString profile;
            // Keep the temporary directory until TDLib has flushed and closed its files.
            std::shared_ptr<QTemporaryDir> temporary;
            bool closing {};
            bool eraseAfterClose {};
        };

        void receiveLoop()
        {
            auto deadline = Clock::time_point::max();
            while (true) {
                {
                    const QMutexLocker lock(&mutex_);
                    while (clients_.isEmpty() && !stopping_.load())
                        clientsChanged_.wait(&mutex_);
                    if (stopping_.load()) {
                        if (deadline == Clock::time_point::max())
                            deadline = Clock::now() + 3s;
                        if (clients_.isEmpty() || Clock::now() >= deadline)
                            break;
                    }
                }
                const char *payload = receive_(0.1);
                if (!payload)
                    continue;

                const auto event = QJsonDocument::fromJson(QByteArray(payload)).object();
                const int id = event.value(u"@client_id"_s).toInt();
                if (id <= 0)
                    continue;

                const auto type = event.value(u"@type"_s).toString();
                if (type == u"updateAuthorizationState"
                    && event.value(u"authorization_state"_s).toObject().value(u"@type"_s).toString()
                        == u"authorizationStateClosed") {
                    const QMutexLocker lock(&mutex_);
                    const auto client = clients_.take(id);
                    if (client.eraseAfterClose)
                        removeProfile(client.profile);
                }

                if (type == u"updateFile" || stopping_.load())
                    continue;
                QMetaObject::invokeMethod(
                    this, [this, id, event] { emit received(id, event); }, Qt::QueuedConnection);
            }
        }

        std::unique_ptr<QLibrary> library_;
        Create create_ {};
        Send send_ {};
        Receive receive_ {};
        QMutex mutex_;
        QWaitCondition clientsChanged_;
        QHash<int, Client> clients_;
        std::atomic_bool stopping_ {};
        std::thread worker_;
    };

} // namespace

TelegramTransport &TelegramTransport::native()
{
    static QPointer<NativeTelegramTransport> transport;
    if (!transport)
        transport = new NativeTelegramTransport(QCoreApplication::instance());
    return *transport;
}

} // namespace phoschat
