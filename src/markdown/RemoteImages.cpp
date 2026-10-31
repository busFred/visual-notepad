#include "RemoteImages.h"

#include <QBuffer>
#include <QImageReader>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace vn {

namespace {

QString cacheKey(const QUrl &url)
{
    return url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
}

// The system's proxy settings (Qt ignores them otherwise), but never for
// this machine.
class ProxyFactory : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override
    {
        const QString host = query.peerHostName();
        if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || host.startsWith(QLatin1String("127."))
            || host == QLatin1String("::1"))
            return {QNetworkProxy::NoProxy};
        return systemProxyForQuery(query);
    }
};

QImage decode(const QByteArray &data)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    return reader.canRead() ? reader.read() : QImage();
}

} // namespace

struct RemoteImages::Job
{
    quint64 id = 0;
    QUrl url;     // As requested.
    QUrl current; // After the redirects so far.
    int redirects = 0;
    QByteArray data;
    QNetworkReply *reply = nullptr;
    bool rejected = false; // Too big or too slow: aborted by us.
};

RemoteImages &RemoteImages::instance()
{
    static RemoteImages images;
    return images;
}

RemoteImages::~RemoteImages() = default;

RemoteImages::RemoteImages()
    : m_cache(cacheKiB)
    , m_decoded(decodedCacheKiB)
{
    m_manager.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    m_manager.setProxyFactory(new ProxyFactory);
    m_notify.setSingleShot(true);
    m_notify.setInterval(30);
    connect(&m_notify, &QTimer::timeout, this, &RemoteImages::ready);
}

bool RemoteImages::isWebUrl(const QUrl &url)
{
    return url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https");
}

bool RemoteImages::redirectAllowed(const QUrl &from, const QUrl &to)
{
    if (!isWebUrl(to) || !to.isValid() || to.host().isEmpty() || !to.userInfo().isEmpty())
        return false;
    return !(from.scheme() == QLatin1String("https") && to.scheme() != QLatin1String("https"));
}

RemoteImages::State RemoteImages::state(const QUrl &url) const
{
    const QString key = cacheKey(url);
    if (m_cache.contains(key))
        return State::Loaded;
    if (m_failed.contains(key))
        return State::Failed;
    return m_loading.contains(key) ? State::Loading : State::Missing;
}

RemoteImages::State RemoteImages::request(const QUrl &url)
{
    if (const State s = state(url); s != State::Missing)
        return s;
    const QString key = cacheKey(url);
    if (!redirectAllowed(QUrl(), url) || (!m_seen.contains(key) && m_seen.size() >= maxAddresses))
        return State::Failed; // Not remembered: there is no end to those.
    m_seen.insert(key);
    if (++m_attempts[key] > maxAttempts) {
        m_failed.insert(key);
        return State::Failed;
    }
    m_loading.insert(key);
    m_queue.push_back(url);
    pump();
    return State::Loading;
}

QImage RemoteImages::fullImage(const QUrl &url) const
{
    const QString key = cacheKey(url);
    if (!m_cache.contains(key))
        return {};
    if (const QImage *hit = m_decoded.object(key))
        return *hit;
    const QByteArray *data = m_cache.object(key);
    if (!data)
        return {};
    const QImage image = decode(*data);
    if (!image.isNull())
        m_decoded.insert(key, new QImage(image), qMax<qsizetype>(1, image.sizeInBytes() / 1024));
    return image;
}

void RemoteImages::clearFailures()
{
    m_failed.clear();
    m_attempts.clear();
}

void RemoteImages::clear()
{
    for (const auto &job : m_active) {
        if (job->reply) {
            job->reply->disconnect(this);
            job->reply->abort();
            job->reply->deleteLater();
        }
    }
    m_active.clear();
    m_queue.clear();
    m_loading.clear();
    m_failed.clear();
    m_attempts.clear();
    m_cache.clear();
    m_decoded.clear();
    m_seen.clear();
    m_notify.stop();
    ++m_generation;
}

void RemoteImages::pump()
{
    while (int(m_active.size()) < maxConcurrent && !m_queue.empty()) {
        static quint64 nextId = 0;
        auto job = std::make_unique<Job>();
        job->id = ++nextId;
        job->url = job->current = m_queue.front();
        m_queue.pop_front();
        Job *raw = job.get();
        m_active.push_back(std::move(job));
        send(raw);
        // The whole download, redirects included, has a limit too.
        const quint64 id = raw->id;
        QTimer::singleShot(m_timeout * 3, this, [this, id] {
            for (const auto &job : m_active) {
                if (job->id != id)
                    continue;
                job->rejected = true;
                if (job->reply)
                    job->reply->abort();
                else
                    finish(job.get(), QImage());
                return;
            }
        });
    }
}

void RemoteImages::send(Job *job)
{
    QNetworkRequest request(job->current);
    // Redirects are checked here; no cookies, saved logins or disk cache.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(m_timeout);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (compatible; VisualNotepad)"));
    request.setRawHeader("Accept", "image/*");

    job->data.clear();
    job->reply = m_manager.get(request);
    // Read little ahead of the size check below, so an endless body is
    // abandoned soon after the limit rather than after the network is drained.
    job->reply->setReadBufferSize(1 << 20);
    const quint64 id = job->id;
    QNetworkReply *reply = job->reply;
    const auto lookup = [this, id]() -> Job * {
        for (const auto &job : m_active) {
            if (job->id == id)
                return job.get();
        }
        return nullptr;
    };
    // Abort as soon as the body is over the limit, announced or not.
    const auto tooBig = [lookup, reply](qint64 size) {
        if (size <= maxImageBytes)
            return;
        if (Job *job = lookup())
            job->rejected = true;
        reply->abort();
    };
    connect(reply, &QNetworkReply::readyRead, this, [lookup, reply, tooBig] {
        Job *job = lookup();
        if (!job)
            return;
        job->data += reply->readAll();
        tooBig(job->data.size());
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [tooBig](qint64 received, qint64 total) { tooBig(qMax(received, total)); });
    connect(reply, &QNetworkReply::finished, this, [this, lookup] {
        if (Job *job = lookup())
            onFinished(job);
    });
}

void RemoteImages::releaseReply(Job *job)
{
    if (!job->reply)
        return;
    job->reply->disconnect(this);
    job->reply->deleteLater();
    job->reply = nullptr;
}

void RemoteImages::onFinished(Job *job)
{
    QNetworkReply *reply = job->reply;
    if (job->rejected)
        return finish(job, QImage());
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QUrl target = job->current.resolved(location);
        if (location.isEmpty() || !redirectAllowed(job->current, target) || ++job->redirects > maxRedirects)
            return finish(job, QImage());
        releaseReply(job);
        job->current = target;
        return send(job);
    }
    if (reply->isOpen())
        job->data += reply->readAll();
    if (reply->error() != QNetworkReply::NoError || status != 200 || job->data.size() > maxImageBytes)
        return finish(job, QImage());
    finish(job, decode(job->data));
}

void RemoteImages::finish(Job *job, const QImage &image)
{
    const QString key = cacheKey(job->url);
    bool ok = !image.isNull();
    if (ok) {
        // Decoded once, here; the encoded bytes stay for evictions from the decoded cache.
        ok = m_cache.insert(key, new QByteArray(job->data), qMax<qsizetype>(1, job->data.size() / 1024));
        if (ok)
            m_decoded.insert(key, new QImage(image), qMax<qsizetype>(1, image.sizeInBytes() / 1024));
    }
    m_loading.remove(key);
    if (!ok)
        m_failed.insert(key);
    releaseReply(job);
    for (auto it = m_active.begin(); it != m_active.end(); ++it) {
        if (it->get() == job) {
            m_active.erase(it);
            break;
        }
    }
    notify();
    pump();
}

void RemoteImages::notify()
{
    if (!m_notify.isActive())
        m_notify.start();
}

} // namespace vn
