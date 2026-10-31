// Downloads http(s) pictures named in Markdown, on request only: nothing here
// runs until the user has agreed to load the images of a document (see
// MarkdownModel::remoteConsent). Downloads are asynchronous, bounded in
// size, time, redirects and number, and kept in a memory cache; ready() is
// emitted on the GUI thread when downloads finish, after which a render
// finds the picture (or its failure) here.
//
// Only hosts on the public internet are contacted: never this machine or a
// local network (see hostAllowed and isPublicAddress), so a document cannot
// make the app reach a router page, a development server or a cloud metadata
// address. Each host, the first and every redirect's, is looked up before the
// request and refused if any of its addresses is not public; the connection
// then goes to the address that was checked (see send()).
#pragma once

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <deque>
#include <memory>
#include <vector>

class QHostInfo;
class QNetworkProxyFactory;
class QNetworkReply;

namespace vn {

class RemoteImages : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 maxImageBytes = 20 * 1024 * 1024;
    static constexpr int maxRedirects = 5;
    static constexpr int maxConcurrent = 4;
    static constexpr int cacheKiB = 64 * 1024;
    // Times one address may be fetched in a session, so pictures evicted from
    // a full cache cannot make a document download in a loop.
    static constexpr int maxAttempts = 3;
    // Distinct addresses fetched in a session; further pictures stay missing.
    static constexpr int maxAddresses = 200;
    static constexpr int decodedCacheKiB = 32 * 1024;

    static RemoteImages &instance();

    // http or https, whatever else is wrong with it.
    static bool isWebUrl(const QUrl &url);
    // Whether a redirect from one address to another may be followed (and,
    // with an empty `from`, whether an address may be fetched at all): web
    // addresses with an allowed host (see hostAllowed) and no credentials,
    // and never from https to http.
    static bool redirectAllowed(const QUrl &from, const QUrl &to);

    // Whether a host may be contacted, judged by its spelling alone: not a
    // local name (see hostIsLocalName) and, when it is an IP address in any
    // spelling (see literalAddress), a public one. Names that pass are still
    // looked up and refused if they lead to a non-public address.
    static bool hostAllowed(const QString &host);
    // Names that only mean something on this machine or a local network:
    // "localhost" and *.localhost, *.localdomain, *.local (mDNS), *.internal,
    // *.lan, *.home.arpa, and single-label names ("intranet", "router") that
    // the resolver completes with the local search domain. Case and a final
    // dot do not matter.
    static bool hostIsLocalName(const QString &host);
    // The IP address a host spells, or null for a name. Besides the usual
    // forms this takes the ones resolvers accept for IPv4 (inet_aton):
    // decimal "2130706433", hex "0x7f000001", octal "0177.0.0.1", short
    // "127.1" and mixes of these, which would otherwise reach the address
    // they spell.
    static QHostAddress literalAddress(const QString &host);
    // True for an address on the public internet. False for this machine
    // (127/8, ::1), private networks (10/8, 172.16/12, 192.168/16,
    // fc00::/7), link-local (169.254/16 with cloud metadata services,
    // fe80::/10), shared carrier-grade NAT (100.64/10), unspecified and
    // "this network" (0/8, ::), multicast (224/4, ff00::/8), reserved and
    // broadcast (240/4), documentation and benchmarking ranges, IPv6
    // site-local (fec0::/10), and IPv6 outside global unicast (2000::/3).
    // IPv6 forms that carry an IPv4 address (mapped ::ffff:a.b.c.d,
    // compatible ::a.b.c.d, NAT64 64:ff9b::/96, 6to4 2002::/16) are judged
    // by that address; Teredo and the other IETF ranges in 2001::/23 are
    // refused.
    static bool isPublicAddress(const QHostAddress &address);
    // Whether a name that looked up to these addresses may be contacted:
    // some address, and every one public.
    static bool resolvedAddressesAllowed(const QList<QHostAddress> &addresses);
    // For tests only: lets hosts on this machine (a stub server on
    // 127.0.0.1) be contacted. Off by default; no setting or file reaches it.
    static void setAllowLocalHostsForTests(bool allow);

    enum class State { Missing, Loading, Loaded, Failed };
    State state(const QUrl &url) const;
    // Starts downloading unless it was tried; returns the state afterwards.
    // Only call this for documents the user agreed to.
    State request(const QUrl &url);
    // A downloaded picture at full size; null unless Loaded. Decoded once
    // (when it arrives, on a worker thread), then kept in a bounded cache of
    // decoded pictures.
    QImage fullImage(const QUrl &url) const;
    // Failed addresses may be tried again (the user asked to load).
    void clearFailures();
    // Aborts downloads and forgets everything, including the generation.
    void clear();
    // Bumped by clear(), for caches of what was made from downloads.
    int generation() const { return m_generation; }
    // Stall timeout of a download; it may take three times as long in all.
    void setTimeout(int milliseconds) { m_timeout = milliseconds; }

Q_SIGNALS:
    // Some downloads finished, or failed.
    void ready();

private:
    struct Job;

    RemoteImages();
    ~RemoteImages() override;
    Job *findJob(quint64 id) const;
    void pump();
    void send(Job *job);
    void resolved(Job *job, const QHostInfo &info);
    void get(Job *job, const QHostAddress &address);
    bool viaProxy(const QUrl &url) const;
    void onFinished(Job *job);
    void finish(Job *job, const QImage &image);
    void releaseReply(Job *job);
    void notify();

    QNetworkAccessManager m_manager;
    QNetworkProxyFactory *m_proxies = nullptr; // Owned by m_manager.
    QCache<QString, QByteArray> m_cache; // Encoded pictures by address, KiB.
    mutable QCache<QString, QImage> m_decoded; // Their decoded form, KiB.
    QSet<QString> m_seen;                // Every address asked for.
    QSet<QString> m_loading;             // Queued and running.
    QSet<QString> m_failed;
    QHash<QString, int> m_attempts;
    std::deque<QUrl> m_queue;
    std::vector<std::unique_ptr<Job>> m_active;
    QTimer m_notify;
    int m_timeout = 10000;
    int m_generation = 0;
};

} // namespace vn
