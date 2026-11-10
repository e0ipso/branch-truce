#pragma once

#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlNetworkAccessManagerFactory>

namespace comparison_scene {
// A network access manager factory whose managers record every request and fail it, so a case can
// check that the scene asked for nothing. Managers may live on the image loader's thread.
class RecordingNetwork final : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager *create(QObject *parent) override { return new Manager(*this, parent); }
    [[nodiscard]] QList<QUrl> requests() const
    {
        const QMutexLocker lock(&mutex_);
        return requests_;
    }

private:
    class Manager final : public QNetworkAccessManager {
    public:
        Manager(RecordingNetwork &log, QObject *parent) : QNetworkAccessManager(parent), log_(log) {}

    protected:
        QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *data) override
        {
            log_.record(request.url());
            // A scheme no manager serves, so the request fails without leaving the process.
            return QNetworkAccessManager::createRequest(operation, QNetworkRequest(QUrl(QStringLiteral("refused:"))),
                                                        data);
        }

    private:
        RecordingNetwork &log_;
    };
    void record(const QUrl &url)
    {
        const QMutexLocker lock(&mutex_);
        requests_.append(url);
    }
    mutable QMutex mutex_;
    QList<QUrl> requests_;
};

} // namespace comparison_scene
