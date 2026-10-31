#include "RemoteImages.h"

#include "BlockRenderer.h"

#include <QHostInfo>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThreadPool>

#include <algorithm>

namespace vn {

namespace {

bool allowLocalHosts = false; // Tests only.

QString cacheKey(const QUrl &url)
{
    return url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
}

// The system's proxy settings (Qt ignores them otherwise), but never for
// this machine, and never for an address rather than a name: requests sent
// to the address a name was checked to have (see RemoteImages::get) go
// straight there, so the proxy setting decided for the name holds.
class ProxyFactory : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override
    {
        const QString host = query.peerHostName();
        if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
            || !QHostAddress(host).isNull())
            return {QNetworkProxy::NoProxy};
        return systemProxyForQuery(query);
    }
};

// A part of an IPv4 address the way inet_aton reads it: "0x" hex, a leading
// 0 octal, else decimal. False if it is none of these.
bool ipv4Part(QStringView part, quint64 *value)
{
    int base = 10;
    if (part.size() >= 2 && part[0] == u'0' && (part[1] == u'x' || part[1] == u'X')) {
        base = 16;
        part = part.mid(2);
        if (part.isEmpty()) { // "0x" alone is 0 to inet_aton.
            *value = 0;
            return true;
        }
    } else if (part.size() >= 2 && part[0] == u'0') {
        base = 8;
        part = part.mid(1);
    }
    if (part.isEmpty() || part.size() > 16) // 16 digits cannot overflow 64 bits.
        return false;
    quint64 v = 0;
    for (const QChar ch : part) {
        const char16_t c = ch.unicode();
        int digit = 99;
        if (c >= u'0' && c <= u'9')
            digit = c - u'0';
        else if (c >= u'a' && c <= u'f')
            digit = c - u'a' + 10;
        else if (c >= u'A' && c <= u'F')
            digit = c - u'A' + 10;
        if (digit >= base)
            return false;
        v = v * base + quint64(digit);
    }
    *value = v;
    return true;
}

bool publicIPv4(quint32 a)
{
    const auto in = [a](quint32 network, int bits) { return (a >> (32 - bits)) == (network >> (32 - bits)); };
    return !(in(0x00000000, 8)       // "This network", 0.0.0.0.
             || in(0x0A000000, 8)    // Private.
             || in(0x64400000, 10)   // Carrier-grade NAT.
             || in(0x7F000000, 8)    // Loopback.
             || in(0xA9FE0000, 16)   // Link-local, cloud metadata (169.254.169.254).
             || in(0xAC100000, 12)   // Private.
             || in(0xC0000000, 24)   // IETF protocol assignments.
             || in(0xC0000200, 24)   // Documentation.
             || in(0xC0A80000, 16)   // Private.
             || in(0xC6120000, 15)   // Benchmarking.
             || in(0xC6336400, 24)   // Documentation.
             || in(0xCB007100, 24)   // Documentation.
             || in(0xE0000000, 4)    // Multicast.
             || in(0xF0000000, 4));  // Reserved, broadcast.
}

quint32 ipv4At(const Q_IPV6ADDR &a, int at)
{
    return quint32(a[at]) << 24 | quint32(a[at + 1]) << 16 | quint32(a[at + 2]) << 8 | quint32(a[at + 3]);
}

bool addressAllowed(const QHostAddress &address)
{
    return allowLocalHosts || RemoteImages::isPublicAddress(address);
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
    int lookup = -1; // Host lookup running.
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
    m_proxies = new ProxyFactory;
    m_manager.setProxyFactory(m_proxies);
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
    if (!hostAllowed(to.host(QUrl::FullyEncoded)))
        return false;
    return !(from.scheme() == QLatin1String("https") && to.scheme() != QLatin1String("https"));
}

QHostAddress RemoteImages::literalAddress(const QString &host)
{
    QString h = host.trimmed();
    if (h.endsWith(u'.'))
        h.chop(1);
    if (h.startsWith(u'[') && h.endsWith(u']'))
        h = h.mid(1, h.size() - 2);
    if (h.isEmpty())
        return {};
    if (h.contains(u':'))
        return QHostAddress(h); // IPv6 (null if it is not one).
    const QList<QStringView> parts = QStringView(h).split(u'.');
    if (parts.size() > 4)
        return {};
    quint64 values[4] = {};
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (!ipv4Part(parts[i], &values[i]))
            return {};
    }
    // The last part fills the bytes the others leave: a, a.b (8.24),
    // a.b.c (8.8.16), a.b.c.d.
    const int last = int(parts.size()) - 1;
    quint64 address = 0;
    for (int i = 0; i < last; ++i) {
        if (values[i] > 255)
            return {};
        address |= values[i] << (24 - 8 * i);
    }
    if (values[last] >= (quint64(1) << (32 - 8 * last)))
        return {};
    address |= values[last];
    return QHostAddress(quint32(address));
}

bool RemoteImages::hostIsLocalName(const QString &host)
{
    QString h = host.trimmed().toLower();
    if (h.endsWith(u'.'))
        h.chop(1);
    if (h.isEmpty())
        return true;
    if (!literalAddress(h).isNull())
        return false; // An address, not a name (see isPublicAddress).
    if (!h.contains(u'.'))
        return true; // Completed with the local search domain, if anything.
    for (const char *suffix : {"localhost", "localdomain", "local", "internal", "lan", "home.arpa"}) {
        const QLatin1String s(suffix);
        if (h == s || (h.endsWith(s) && h.at(h.size() - s.size() - 1) == u'.'))
            return true;
    }
    return false;
}

bool RemoteImages::isPublicAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
        return publicIPv4(address.toIPv4Address());
    if (address.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    const Q_IPV6ADDR a = address.toIPv6Address();
    const auto zero = [&a](int from, int to) {
        for (int i = from; i < to; ++i) {
            if (a[i] != 0)
                return false;
        }
        return true;
    };
    // IPv4-compatible (and so ::, ::1), mapped and translated forms.
    if (zero(0, 12) || (zero(0, 10) && a[10] == 0xff && a[11] == 0xff)
        || (zero(0, 8) && a[8] == 0xff && a[9] == 0xff && a[10] == 0 && a[11] == 0))
        return publicIPv4(ipv4At(a, 12));
    // NAT64: the well-known prefix carries an IPv4 address; the local-use one is local.
    if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xff && a[3] == 0x9b)
        return zero(4, 12) && publicIPv4(ipv4At(a, 12));
    // Only global unicast is public.
    if ((a[0] & 0xe0) != 0x20)
        return false; // fc00::/7, fe80::/10, fec0::/10, ff00::/8, 100::/64 and the rest.
    // 6to4 carries an IPv4 address.
    if (a[0] == 0x20 && a[1] == 0x02)
        return publicIPv4(ipv4At(a, 2));
    // IETF ranges (Teredo 2001::/32 hides an address, ORCHID, benchmarking)
    // and documentation (2001:db8::/32, 3fff::/20).
    if (a[0] == 0x20 && a[1] == 0x01 && (a[2] & 0xfe) == 0)
        return false;
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0d && a[3] == 0xb8)
        return false;
    if (a[0] == 0x3f && a[1] == 0xff && (a[2] & 0xf0) == 0)
        return false;
    return true;
}

bool RemoteImages::hostAllowed(const QString &host)
{
    if (host.trimmed().isEmpty())
        return false;
    if (allowLocalHosts)
        return true;
    if (const QHostAddress address = literalAddress(host); !address.isNull())
        return isPublicAddress(address);
    return !hostIsLocalName(host);
}

void RemoteImages::setAllowLocalHostsForTests(bool allow)
{
    allowLocalHosts = allow;
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
    // Not web, or a host on this machine or a local network: never contacted.
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
    const QImage image = decodePicture(*data); // SVGs naming other machines are refused.
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
        if (job->lookup >= 0)
            QHostInfo::abortHostLookup(job->lookup);
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

RemoteImages::Job *RemoteImages::findJob(quint64 id) const
{
    for (const auto &job : m_active) {
        if (job->id == id)
            return job.get();
    }
    return nullptr;
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
        const quint64 id = raw->id;
        m_active.push_back(std::move(job));
        send(raw); // May finish it already (a refused address): raw is not used again.
        // The whole download, lookups, redirects and decoding included, has
        // a limit too.
        QTimer::singleShot(m_timeout * 3, this, [this, id] {
            Job *job = findJob(id);
            if (!job)
                return;
            job->rejected = true;
            if (job->reply)
                job->reply->abort();
            else
                finish(job, QImage());
        });
    }
}

// Every hop, the first request and each redirect, looks its host up first
// (on Qt's lookup threads) and goes on only if every address it has is
// public; then the request goes to an address that was checked (see get()).
// Letting Qt connect by name would look the name up again: a name whose
// owner answers with a public address to the first lookup and a local one to
// the second ("DNS rebinding") would get through.
void RemoteImages::send(Job *job)
{
    job->data.clear();
    const QString host = job->current.host(QUrl::FullyEncoded);
    if (const QHostAddress literal = literalAddress(host); !literal.isNull())
        return get(job, literal);
    const quint64 id = job->id;
    job->lookup = QHostInfo::lookupHost(host, this, [this, id](const QHostInfo &info) {
        if (Job *job = findJob(id))
            resolved(job, info);
    });
}

bool RemoteImages::resolvedAddressesAllowed(const QList<QHostAddress> &addresses)
{
    if (addresses.isEmpty())
        return false;
    // One local address and the name is not trusted at all: which one a
    // connection would use is not ours to say.
    return std::all_of(addresses.begin(), addresses.end(), addressAllowed);
}

void RemoteImages::resolved(Job *job, const QHostInfo &info)
{
    job->lookup = -1;
    const QList<QHostAddress> addresses = info.addresses();
    if (job->rejected || info.error() != QHostInfo::NoError || !resolvedAddressesAllowed(addresses))
        return finish(job, QImage());
    // One address is used, so no other is tried if it cannot be reached (Qt
    // would try them all by name). IPv4 first: IPv6 is the one more often
    // broken on a network that has both.
    const auto ipv4 = std::find_if(addresses.begin(), addresses.end(), [](const QHostAddress &address) {
        return address.protocol() == QAbstractSocket::IPv4Protocol;
    });
    get(job, ipv4 != addresses.end() ? *ipv4 : addresses.first());
}

bool RemoteImages::viaProxy(const QUrl &url) const
{
    const QList<QNetworkProxy> proxies = m_proxies->queryProxy(QNetworkProxyQuery(url));
    return !proxies.isEmpty() && proxies.first().type() != QNetworkProxy::NoProxy
           && proxies.first().type() != QNetworkProxy::DefaultProxy;
}

void RemoteImages::get(Job *job, const QHostAddress &address)
{
    if (!addressAllowed(address))
        return finish(job, QImage());
    QUrl target = job->current;
    QNetworkRequest request;
    if (!viaProxy(job->current)) {
        // Connect to the checked address, with the name where the server
        // and TLS look for it: the Host header, the certificate check and
        // SNI (Qt takes the server name from the verify name). HTTP/2 is
        // off because it would send the address as :authority instead.
        target.setHost(address.toString());
        request.setRawHeader("Host", job->current.authority(QUrl::FullyEncoded | QUrl::RemoveUserInfo).toLatin1());
        if (job->current.scheme() == QLatin1String("https"))
            request.setPeerVerifyName(job->current.host(QUrl::FullyEncoded));
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    }
    // Through a proxy the proxy connects, by name: it looks the name up
    // itself, so the check above is all there is (see docs/design.md).
    request.setUrl(target);
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
    // Abort as soon as the body is over the limit, announced or not.
    const auto tooBig = [this, id, reply](qint64 size) {
        if (size <= maxImageBytes)
            return;
        if (Job *job = findJob(id))
            job->rejected = true;
        reply->abort();
    };
    connect(reply, &QNetworkReply::readyRead, this, [this, id, reply, tooBig] {
        Job *job = findJob(id);
        if (!job)
            return;
        job->data += reply->readAll();
        tooBig(job->data.size());
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [tooBig](qint64 received, qint64 total) { tooBig(qMax(received, total)); });
    connect(reply, &QNetworkReply::finished, this, [this, id] {
        if (Job *job = findJob(id))
            onFinished(job);
    });
}

void RemoteImages::releaseReply(Job *job)
{
    if (job->lookup >= 0) {
        QHostInfo::abortHostLookup(job->lookup);
        job->lookup = -1;
    }
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
        // The target's host is checked by name here and by address in send().
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
    // Decoding a large picture takes a while: on a worker thread. The job
    // keeps its slot meanwhile; a job finished by the time limit or by
    // clear() is gone when the picture comes back, which is then dropped.
    releaseReply(job);
    const quint64 id = job->id;
    // (The global pool is drained when the application object goes, before
    // this static instance does.)
    QThreadPool::globalInstance()->start([this, id, data = job->data] {
        const QImage image = decodePicture(data); // SVGs naming other machines are refused.
        QMetaObject::invokeMethod(this, [this, id, image] {
            if (Job *job = findJob(id))
                finish(job, image);
        }, Qt::QueuedConnection);
    });
}

void RemoteImages::finish(Job *job, const QImage &image)
{
    const QString key = cacheKey(job->url);
    bool ok = !image.isNull();
    if (ok) {
        // Decoded once, on arrival; the encoded bytes stay for evictions from the decoded cache.
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
