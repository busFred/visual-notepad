// Remote pictures in the Markdown preview, against a stub HTTP server on
// this machine: nothing is requested before the user agrees, the download
// limits hold, and hosts on this machine or a local network are never
// contacted. Runs offscreen without a network (one test uses DNS if there is
// one). The stub is on 127.0.0.1, so most tests let local hosts through with
// RemoteImages::setAllowLocalHostsForTests; the ones about local hosts don't.
#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "markdown/BlockRenderer.h"
#include "markdown/MarkdownModel.h"
#include "markdown/RemoteImages.h"
#include "markdown/VisualView.h"

#include <QBuffer>
#include <QFrame>
#include <QHostInfo>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>

#include <algorithm>
#include <functional>

using namespace vn;

namespace {

QByteArray encoded(int width, int height, const char *format)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}

// Answers GET requests on 127.0.0.1 and remembers them.
class HttpStub : public QObject
{
public:
    HttpStub()
        : m_small(encoded(40, 20, "PNG"))
        , m_wide(encoded(2000, 500, "PNG"))
        , m_huge(encoded(4500, 4500, "BMP")) // 61 MB: a valid picture far over the limit.
    {
        listen();
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                accept(socket);
        });
    }

    void listen() { QVERIFY(m_server.listen(QHostAddress::LocalHost, 0)); }
    QUrl url(const QString &path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
    }
    QString host() const { return QStringLiteral("127.0.0.1:%1").arg(m_server.serverPort()); }

    QStringList requests; // "/path", in arrival order.
    // Called for each request of the app, before it is answered.
    std::function<void()> onRequest;
    QList<QByteArray> headers; // The raw header block of each.
    int hung = 0;         // Connections held open without an answer.
    qint64 sent = 0;      // Bytes handed to the network by the last response.
    qint64 hugeSize() const { return m_huge.size(); }

private:
    void accept(QTcpSocket *socket)
    {
        socket->setParent(&m_server);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
            buffer->append(socket->readAll());
            const qsizetype end = buffer->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            const QList<QByteArray> line = buffer->left(buffer->indexOf("\r\n")).split(' ');
            const QString path = line.value(1);
            // Other programs on this machine probe new listening ports; only
            // requests of the app count.
            if (buffer->left(end).contains("VisualNotepad)")) {
                requests.append(path);
                headers.append(buffer->left(end));
                if (onRequest)
                    onRequest();
            }
            buffer->clear();
            respond(socket, path);
        });
        connect(socket, &QTcpSocket::bytesWritten, socket, [this](qint64 bytes) { sent += bytes; });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    static void send(QTcpSocket *socket, const QByteArray &status, const QByteArray &type, const QByteArray &body,
                     const QByteArray &extra = {}, qint64 declared = -1)
    {
        QByteArray head = "HTTP/1.1 " + status + "\r\nConnection: close\r\n" + extra;
        if (!type.isEmpty())
            head += "Content-Type: " + type + "\r\n";
        if (declared != 0)
            head += "Content-Length: " + QByteArray::number(declared < 0 ? body.size() : declared) + "\r\n";
        socket->write(head + "\r\n" + body);
        socket->disconnectFromHost();
    }

    static QByteArray svg(const QByteArray &content)
    {
        return R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20"><rect width="40" height="20" fill="red"/>)"
               + content + "</svg>";
    }

    static void redirect(QTcpSocket *socket, const QString &to)
    {
        send(socket, "302 Found", {}, {}, "Location: " + to.toUtf8() + "\r\n", 0);
    }

    void respond(QTcpSocket *socket, const QString &path)
    {
        if (path.startsWith(QLatin1String("/ok")))
            send(socket, "200 OK", "image/png", m_small, "Set-Cookie: session=1\r\n");
        else if (path == QLatin1String("/wide.png"))
            send(socket, "200 OK", "image/png", m_wide);
        else if (path == QLatin1String("/text"))
            send(socket, "200 OK", "text/plain", "this is not a picture");
        else if (path == QLatin1String("/missing"))
            send(socket, "404 Not Found", "text/plain", "gone");
        else if (path == QLatin1String("/big")) // Announces the size.
            send(socket, "200 OK", "image/bmp", m_huge);
        else if (path == QLatin1String("/stream")) // No size announced, all of it.
            send(socket, "200 OK", "image/bmp", m_huge, {}, 0);
        else if (path == QLatin1String("/ok.svg"))
            send(socket, "200 OK", "image/svg+xml", svg(QByteArray()));
        else if (path == QLatin1String("/unc.svg")) // The SVG plugin would open \\host\share\x.png itself.
            send(socket, "200 OK", "image/svg+xml", svg(R"(<image href="\\host\share\x.png" width="4" height="4"/>)"));
        else if (path == QLatin1String("/bmp")) // Large but within the limit.
            send(socket, "200 OK", "image/bmp", encoded(1000, 1000, "BMP"));
        else if (path == QLatin1String("/redirect"))
            redirect(socket, QStringLiteral("/ok.png"));
        else if (path == QLatin1String("/redirect-file"))
            redirect(socket, QStringLiteral("file:///etc/hostname"));
        else if (path == QLatin1String("/redirect-ftp"))
            redirect(socket, QStringLiteral("ftp://127.0.0.1/ok.png"));
        else if (path == QLatin1String("/redirect-login"))
            redirect(socket, QStringLiteral("http://user:secret@127.0.0.1/ok.png"));
        else if (path.startsWith(QLatin1String("/to/"))) // Redirects to the percent-encoded address that follows.
            redirect(socket, QUrl::fromPercentEncoding(path.mid(4).toLatin1()));
        else if (path.startsWith(QLatin1String("/hops/"))) { // Redirects N times, then a picture.
            const int left = path.mid(6).toInt();
            if (left > 0)
                redirect(socket, QStringLiteral("/hops/%1").arg(left - 1));
            else
                send(socket, "200 OK", "image/png", m_small);
        } else if (path.startsWith(QLatin1String("/hang"))) {
            ++hung; // Never answers.
        } else {
            send(socket, "400 Bad Request", "text/plain", "?");
        }
    }

    QTcpServer m_server;
    QByteArray m_small;
    QByteArray m_wide;
    QByteArray m_huge;
};

RenderContext context(bool allow, qreal width = 400)
{
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = width;
    ctx.allowRemoteImages = allow;
    return ctx;
}

std::unique_ptr<RenderedBlock> renderHtml(const QString &html, const RenderContext &ctx)
{
    core::Block block;
    block.kind = core::BlockKind::Html;
    const QByteArray utf8 = html.toUtf8();
    block.html = rust::String(utf8.constData(), utf8.size());
    return RenderedBlock::create(block, html, ctx);
}

QString imageTag(const QUrl &url, const QString &attributes = {})
{
    return QStringLiteral("<p><img src=\"%1\" %2></p>").arg(url.toString(), attributes);
}

} // namespace

class TestRemoteImages : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void findsWhatQtWillRequest();
    void findsNamesSpelledWithFoldingLetters();
    void entityEncodedAddressesAreListedAndFetchedOnlyAfterConsent();
    void redirectRules();
    void nothingIsRequestedWithoutConsent();
    void downloadsAfterConsent();
    void wideDownloadsShrinkAndZoom();
    void downloadLimits_data();
    void downloadLimits();
    void failuresAreNotRetried();
    void sendsNoCookiesAndNoCredentials();
    void limitsConcurrentDownloads();
    void stalledDownloadTimesOut();
    void capsDistinctAddresses();
    void viewAsksBeforeLoading();
    void dontLoadNeverContacts();
    void neverPreferenceDoesNotAsk();
    void consentIsSharedByViews();
    void localImagesStillLoad();
    void addressesAreJudged_data();
    void addressesAreJudged();
    void lookedUpAddressesAreJudged();
    void localHostsAreNeverContacted();
    void namesResolvingToThisMachineAreRefused();
    void namesKeepTheirHostHeader();

private:
    std::unique_ptr<HttpStub> m_stub;
    std::unique_ptr<QTextDocument> m_doc;
    MarkdownModel *m_model = nullptr;
    // A visible view of a document with two pictures from the stub.
    std::unique_ptr<VisualView> openView(const QString &markdown);
    static QPushButton *button(QWidget *parent, const QString &text);
};

void TestRemoteImages::initTestCase()
{
    theme::apply(*qApp);
}

void TestRemoteImages::init()
{
    RemoteImages::setAllowLocalHostsForTests(true); // The stub is on 127.0.0.1.
    m_stub = std::make_unique<HttpStub>();
    RemoteImages::instance().clear();
    RemoteImages::instance().setTimeout(10000);
    Settings::instance().reset(key::remoteImages);
}

void TestRemoteImages::cleanup()
{
    if (QTest::currentTestFailed() && m_stub) {
        // What reached the stub, for failures only seen on another system.
        qInfo() << "stub requests:" << m_stub->requests << "hung:" << m_stub->hung << "sent:" << m_stub->sent;
        for (const QByteArray &head : std::as_const(m_stub->headers))
            qInfo().noquote() << "request head:" << QString::fromLatin1(head).replace(QLatin1String("\r\n"), QLatin1String(" | "));
    }
    RemoteImages::setAllowLocalHostsForTests(true);
    RemoteImages::instance().clear();
    m_doc.reset();
    m_model = nullptr;
    m_stub.reset();
}

std::unique_ptr<VisualView> TestRemoteImages::openView(const QString &markdown)
{
    m_doc = std::make_unique<QTextDocument>();
    m_doc->setDocumentLayout(new DocumentLayout(m_doc.get()));
    m_doc->setPlainText(markdown);
    m_model = new MarkdownModel(m_doc.get());
    auto view = std::make_unique<VisualView>(m_model);
    view->resize(760, 600);
    view->show();
    return view;
}

QPushButton *TestRemoteImages::button(QWidget *parent, const QString &text)
{
    for (QPushButton *b : parent->findChildren<QPushButton *>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

namespace {

core::Block htmlBlock(const QString &html)
{
    core::Block block;
    block.kind = core::BlockKind::Html;
    const QByteArray utf8 = html.toUtf8();
    block.html = rust::String(utf8.constData(), utf8.size());
    return block;
}

// Pictures asked for in many spellings; the numbered files exist on the stub.
QString tricky(const QString &host)
{
    return QStringLiteral(
               "<p><img src=\"&#104;ttp://%1/ok1.png\"> <img src=\"&#x68;ttp://%1/ok2.png\">"
               "<img src=\"http&colon;//%1/ok3.png\"> <IMG SRC='http://%1/ok4.png?a=1&amp;b=2'>"
               "<img src=\"&#104;&#116;&#116;&#112;&#58;&#47;&#47;%1/ok5.png\">"
               "<img src=\"ftp://%1/no.png\"><img src=\"local/pic.png\"><img data-src=\"http://%1/lazy.png\">"
               "<a href=\"http://%1/page\">link</a></p>"
               "<table background=\"http://%1/ok6.png\"><tr><td>x</td></tr></table>"
               "<p style=\"background-image: url('http://%1/ok7.png')\">styled</p>"
               "<table><tr><td style=\"background-image: url(&#104;ttp://%1/ok8.png)\">cell</td></tr></table>")
        .arg(host);
}

} // namespace

void TestRemoteImages::findsWhatQtWillRequest()
{
    const QString host = m_stub->host();
    const RenderContext ctx = context(false);
    const QStringList found = RenderedBlock::remoteImages(htmlBlock(tricky(host)), ctx);
    // Whatever the spelling, what Qt decodes and asks for is listed: numeric
    // entities, background attributes and styles (even with entities in them).
    const auto has = [&](const char *name) {
        return std::any_of(found.begin(), found.end(), [&](const QString &url) {
            return url.startsWith(QStringLiteral("http://%1/%2.png").arg(host, QString::fromLatin1(name)));
        });
    };
    for (const char *name : {"ok1", "ok2", "ok4", "ok5", "ok6", "ok7", "ok8"})
        QVERIFY2(has(name), name);
    QVERIFY(!has("ok3")); // &colon; is not decoded by Qt, so it is no address.
    // Not requested by Qt, so not listed (and never fetched).
    for (const QString &url : found) {
        QVERIFY(!url.contains(QLatin1String("lazy")));
        QVERIFY(!url.contains(QLatin1String("no.png")));
        QVERIFY(!url.contains(QLatin1String("/page")));
    }
    QVERIFY(RenderedBlock::remoteImages(htmlBlock(QStringLiteral("<p>no pictures, see http://example.com</p>")), ctx).isEmpty());
    QVERIFY(m_stub->requests.isEmpty());
}

// Qt matches tag and attribute names ignoring case by Unicode rules, where
// the Kelvin sign is a "k" and the long s an "s": the quick look at the
// HTML's bytes for names that load pictures must not miss those.
void TestRemoteImages::findsNamesSpelledWithFoldingLetters()
{
    const QString host = m_stub->host();
    const RenderContext ctx = context(false);
    const QString kelvin = QStringLiteral("<table><tr><td bac\u212Aground=\"http://%1/k.png\">x</td></tr></table>").arg(host);
    QCOMPARE(RenderedBlock::remoteImages(htmlBlock(kelvin), ctx), QStringList{QStringLiteral("http://%1/k.png").arg(host)});
    const QString longS = QStringLiteral("<\u017Ftyle>p { background-image: url(http://%1/s.png) }</\u017Ftyle><p>x</p>").arg(host);
    QCOMPARE(RenderedBlock::remoteImages(htmlBlock(longS), ctx), QStringList{QStringLiteral("http://%1/s.png").arg(host)});
    QVERIFY(m_stub->requests.isEmpty());
}

void TestRemoteImages::entityEncodedAddressesAreListedAndFetchedOnlyAfterConsent()
{
    const QString host = m_stub->host();
    const QString html = tricky(host);
    const QStringList listed = RenderedBlock::remoteImages(htmlBlock(html), context(false));
    QVERIFY(listed.size() >= 5);
    const auto view = openView(html + QLatin1String("\n"));
    auto *banner = view->findChild<QFrame *>(QStringLiteral("RemoteImageBanner"));
    QVERIFY(banner && banner->isVisible());
    const QString text = banner->findChild<QLabel *>()->text();
    QVERIFY2(text.contains(QStringLiteral("%1 (%2)").arg(host).arg(listed.size())), qPrintable(text));
    view->grab();
    QTest::qWait(400);
    QVERIFY(m_stub->requests.isEmpty());

    QTest::mouseClick(button(banner, QStringLiteral("Load images")), Qt::LeftButton);
    QTRY_VERIFY(m_stub->requests.size() >= listed.size());
    view->grab();
    QTest::qWait(500);
    view->grab();
    // Everything fetched is something the banner listed, and nothing else.
    QStringList fetched;
    for (const QString &path : std::as_const(m_stub->requests))
        fetched.append(QStringLiteral("http://%1%2").arg(host, path));
    for (const QString &url : std::as_const(fetched))
        QVERIFY2(listed.contains(url), qPrintable(url));
    // And all of it, backgrounds included, was fetched.
    for (const QString &url : listed)
        QVERIFY2(fetched.contains(url), qPrintable(url));
}

void TestRemoteImages::redirectRules()
{
    const auto allowed = [](const char *from, const char *to) {
        return RemoteImages::redirectAllowed(QUrl(QString::fromLatin1(from)), QUrl(QString::fromLatin1(to)));
    };
    QVERIFY(allowed("http://a.example/x", "http://b.example/y"));
    QVERIFY(allowed("http://a.example/x", "https://b.example/y"));
    QVERIFY(allowed("https://a.example/x", "https://b.example/y"));
    QVERIFY(!allowed("https://a.example/x", "http://b.example/y")); // Never to less safe.
    QVERIFY(!allowed("https://a.example/x", "http://a.example/x"));
    QVERIFY(!allowed("http://a.example/x", "file:///etc/passwd"));
    QVERIFY(!allowed("http://a.example/x", "ftp://a.example/x.png"));
    QVERIFY(!allowed("http://a.example/x", "data:image/png;base64,AAAA"));
    QVERIFY(!allowed("http://a.example/x", "http://user:pw@b.example/y"));
    QVERIFY(!allowed("http://a.example/x", "http:///nohost"));
    // Only web addresses are ever fetched.
    for (const char *url : {"file:///etc/passwd", "ftp://a.example/x.png", "data:image/png;base64,AAAA", "pic.png", "http:///x"})
        QCOMPARE(RemoteImages::instance().request(QUrl(QString::fromLatin1(url))), RemoteImages::State::Failed);
    QVERIFY(m_stub->requests.isEmpty());
}

void TestRemoteImages::nothingIsRequestedWithoutConsent()
{
    const QUrl url = m_stub->url(QStringLiteral("/ok.png"));
    const auto block = renderHtml(imageTag(url), context(false));
    QVERIFY(!block->isPending());
    QTest::qWait(300);
    QVERIFY(m_stub->requests.isEmpty());
    QCOMPARE(RemoteImages::instance().state(url), RemoteImages::State::Missing);
    // Not even a picture another document already downloaded shows.
    QVERIFY(RemoteImages::instance().request(url) != RemoteImages::State::Missing);
    QTRY_COMPARE(RemoteImages::instance().state(url), RemoteImages::State::Loaded);
    const auto refused = renderHtml(imageTag(url), context(false));
    QVERIFY(!refused->pictureAt(QPointF(5, 5)));
    QCOMPARE(m_stub->requests.size(), 1);
}

void TestRemoteImages::downloadsAfterConsent()
{
    const QUrl url = m_stub->url(QStringLiteral("/ok.png"));
    const auto placeholder = renderHtml(imageTag(url), context(false));
    QSignalSpy ready(&RemoteImages::instance(), &RemoteImages::ready);
    const auto first = renderHtml(imageTag(url), context(true));
    QVERIFY(first->isPending());
    QTRY_VERIFY(ready.count() > 0);
    QCOMPARE(RemoteImages::instance().state(url), RemoteImages::State::Loaded);
    const auto done = renderHtml(imageTag(url), context(true));
    QVERIFY(!done->isPending());
    QVERIFY(done->height() != placeholder->height());
    QCOMPARE(m_stub->requests, QStringList{QStringLiteral("/ok.png")});
    // The picture is 40x20 and can be enlarged without downloading again.
    const auto picture = done->pictureAt(QPointF(10, 5));
    QVERIFY(picture);
    QCOMPARE(picture->title, QStringLiteral("ok.png"));
    QCOMPARE(done->pictureImage(*picture).size(), QSize(40, 20));
    QCOMPARE(m_stub->requests.size(), 1);
}

void TestRemoteImages::wideDownloadsShrinkAndZoom()
{
    const QUrl url = m_stub->url(QStringLiteral("/wide.png"));
    renderHtml(imageTag(url), context(true));
    QTRY_COMPARE(RemoteImages::instance().state(url), RemoteImages::State::Loaded);
    // 2000x500 in a 400 wide column keeps its shape: about 100 high.
    const auto rendered = renderHtml(imageTag(url), context(true));
    QVERIFY2(rendered->height() < 200, qPrintable(QString::number(rendered->height())));
    QVERIFY(rendered->height() > 100);
    const auto picture = rendered->pictureAt(QPointF(200, 50));
    QVERIFY(picture);
    QCOMPARE(rendered->pictureImage(*picture).size(), QSize(2000, 500));
}

void TestRemoteImages::downloadLimits_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("loads");
    QTest::newRow("picture") << QStringLiteral("/ok.png") << true;
    QTest::newRow("large but allowed") << QStringLiteral("/bmp") << true;
    QTest::newRow("redirect") << QStringLiteral("/redirect") << true;
    QTest::newRow("5 redirects") << QStringLiteral("/hops/5") << true;
    QTest::newRow("6 redirects") << QStringLiteral("/hops/6") << false;
    QTest::newRow("redirect to file") << QStringLiteral("/redirect-file") << false;
    QTest::newRow("redirect to ftp") << QStringLiteral("/redirect-ftp") << false;
    QTest::newRow("redirect with credentials") << QStringLiteral("/redirect-login") << false;
    QTest::newRow("not a picture") << QStringLiteral("/text") << false;
    QTest::newRow("SVG") << QStringLiteral("/ok.svg") << true;
    QTest::newRow("SVG naming another machine") << QStringLiteral("/unc.svg") << false;
    QTest::newRow("404") << QStringLiteral("/missing") << false;
    QTest::newRow("over 20 MiB, announced") << QStringLiteral("/big") << false;
    QTest::newRow("over 20 MiB, unannounced") << QStringLiteral("/stream") << false;
}

void TestRemoteImages::downloadLimits()
{
    QFETCH(QString, path);
    QFETCH(bool, loads);
    const QUrl url = m_stub->url(path);
    QCOMPARE(RemoteImages::instance().request(url), RemoteImages::State::Loading);
    const auto expected = loads ? RemoteImages::State::Loaded : RemoteImages::State::Failed;
    QTRY_COMPARE_WITH_TIMEOUT(RemoteImages::instance().state(url), expected, 15000);
    QCOMPARE(RemoteImages::instance().fullImage(url).isNull(), !loads);
    if (path == QLatin1String("/big") || path == QLatin1String("/stream")) {
        // The client stopped reading soon after the limit (a third of the picture).
        QTest::qWait(300);
        QVERIFY2(m_stub->sent < m_stub->hugeSize() * 3 / 4, qPrintable(QString::number(m_stub->sent)));
    }
}

void TestRemoteImages::failuresAreNotRetried()
{
    const QUrl url = m_stub->url(QStringLiteral("/text"));
    RemoteImages::instance().request(url);
    QTRY_COMPARE(RemoteImages::instance().state(url), RemoteImages::State::Failed);
    QCOMPARE(m_stub->requests.size(), 1);
    // Rendering again and again asks, but nothing goes out.
    for (int i = 0; i < 5; ++i) {
        const auto block = renderHtml(imageTag(url), context(true));
        QVERIFY(!block->isPending());
    }
    QTest::qWait(200);
    QCOMPARE(m_stub->requests.size(), 1);
    // Only an explicit new attempt tries again.
    RemoteImages::instance().clearFailures();
    RemoteImages::instance().request(url);
    QTRY_COMPARE(m_stub->requests.size(), 2);
}

void TestRemoteImages::sendsNoCookiesAndNoCredentials()
{
    RemoteImages::instance().request(m_stub->url(QStringLiteral("/ok1.png")));
    QTRY_COMPARE(m_stub->requests.size(), 1);
    QTRY_COMPARE(RemoteImages::instance().state(m_stub->url(QStringLiteral("/ok1.png"))), RemoteImages::State::Loaded);
    RemoteImages::instance().request(m_stub->url(QStringLiteral("/ok2.png"))); // After a Set-Cookie.
    QTRY_COMPARE(m_stub->requests.size(), 2);
    for (const QByteArray &head : std::as_const(m_stub->headers)) {
        const QByteArray lower = head.toLower();
        QVERIFY(!lower.contains("cookie:"));
        QVERIFY(!lower.contains("authorization:"));
        QVERIFY(!lower.contains("referer:"));
        QVERIFY(lower.contains("accept: image/*"));
        QVERIFY(lower.contains("user-agent: mozilla/5.0 (compatible; visualnotepad)")); // No version.
    }
}

void TestRemoteImages::limitsConcurrentDownloads()
{
    for (int i = 0; i < 10; ++i)
        RemoteImages::instance().request(m_stub->url(QStringLiteral("/hang%1").arg(i)));
    QTRY_COMPARE(m_stub->hung, RemoteImages::maxConcurrent);
    QTest::qWait(300);
    QCOMPARE(m_stub->hung, RemoteImages::maxConcurrent);
}

void TestRemoteImages::stalledDownloadTimesOut()
{
    RemoteImages::instance().setTimeout(300);
    const QUrl url = m_stub->url(QStringLiteral("/hang"));
    RemoteImages::instance().request(url);
    QTRY_COMPARE_WITH_TIMEOUT(RemoteImages::instance().state(url), RemoteImages::State::Failed, 5000);
}

void TestRemoteImages::capsDistinctAddresses()
{
    for (int i = 0; i < RemoteImages::maxAddresses; ++i)
        QCOMPARE(RemoteImages::instance().request(m_stub->url(QStringLiteral("/ok-%1.png").arg(i))), RemoteImages::State::Loading);
    const QUrl beyond = m_stub->url(QStringLiteral("/ok-beyond.png"));
    QCOMPARE(RemoteImages::instance().request(beyond), RemoteImages::State::Failed);
    QTRY_COMPARE_WITH_TIMEOUT(m_stub->requests.size(), RemoteImages::maxAddresses, 15000);
    QTest::qWait(200);
    QCOMPARE(m_stub->requests.size(), RemoteImages::maxAddresses);
    QVERIFY(!m_stub->requests.contains(QStringLiteral("/ok-beyond.png")));
    // Known addresses keep working; a placeholder stays for the rest.
    QCOMPARE(RemoteImages::instance().request(m_stub->url(QStringLiteral("/ok-0.png"))), RemoteImages::State::Loaded);
    QVERIFY(!renderHtml(imageTag(beyond), context(true))->isPending());
}

void TestRemoteImages::viewAsksBeforeLoading()
{
    const QUrl a = m_stub->url(QStringLiteral("/ok.png"));
    const QUrl b = m_stub->url(QStringLiteral("/wide.png"));
    const auto view = openView(QStringLiteral("# Title\n\n![one](%1)\n\nText.\n\n![two](%2)\n").arg(a.toString(), b.toString()));
    auto *banner = view->findChild<QFrame *>(QStringLiteral("RemoteImageBanner"));
    QVERIFY(banner);
    QVERIFY(banner->isVisible());
    // It lists the hosts and how many pictures come from each.
    const QString text = banner->findChild<QLabel *>()->text();
    QVERIFY2(text.contains(m_stub->host() + QLatin1String(" (2)")), qPrintable(text));
    QVERIFY(text.contains(QLatin1String("2 remote image")));
    QVERIFY(button(banner, QStringLiteral("Load images")));
    QVERIFY(button(banner, QStringLiteral("Don't load")));
    // Painting, scrolling and waiting send nothing.
    view->grab();
    QTest::qWait(400);
    QVERIFY(m_stub->requests.isEmpty());
    QCOMPARE(m_model->remoteConsent(), MarkdownModel::RemoteConsent::Undecided);

    const QImage before = view->grab().toImage();
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        before.save(dir + QStringLiteral("/remote-images-banner.png"));
    QTest::mouseClick(button(banner, QStringLiteral("Load images")), Qt::LeftButton);
    QCOMPARE(m_model->remoteConsent(), MarkdownModel::RemoteConsent::Allow);
    QVERIFY(!banner->isVisible());
    QTRY_COMPARE(m_stub->requests.size(), 2);
    QTRY_COMPARE(RemoteImages::instance().state(a), RemoteImages::State::Loaded);
    QTRY_COMPARE(RemoteImages::instance().state(b), RemoteImages::State::Loaded);
    QTRY_VERIFY(view->grab().toImage() != before);
    QTest::qWait(300);
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        view->grab().save(dir + QStringLiteral("/remote-images-loaded.png"));
    // Re-rendering after edits does not ask again or download again.
    QTextCursor cursor(m_doc.get());
    cursor.insertText(QStringLiteral("More. "));
    QTest::qWait(400);
    view->grab();
    QVERIFY(!banner->isVisible());
    QCOMPARE(m_stub->requests.size(), 2);
}

void TestRemoteImages::dontLoadNeverContacts()
{
    const QUrl a = m_stub->url(QStringLiteral("/ok.png"));
    const auto view = openView(QStringLiteral("![one](%1)\n").arg(a.toString()));
    auto *banner = view->findChild<QFrame *>(QStringLiteral("RemoteImageBanner"));
    QVERIFY(banner && banner->isVisible());
    QTest::mouseClick(button(banner, QStringLiteral("Don't load")), Qt::LeftButton);
    QCOMPARE(m_model->remoteConsent(), MarkdownModel::RemoteConsent::Deny);
    QVERIFY(!banner->isVisible());
    // Edits, reparses, settings changes and hiding and showing again: no
    // bar, no request.
    QTextCursor cursor(m_doc.get());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("\nMore text.\n"));
    QTRY_VERIFY(!m_model->hasPendingEdits());
    Settings::instance().setValue(key::textLineHeight, 1.8);
    view->hide();
    view->show();
    view->grab();
    QTest::qWait(400);
    Settings::instance().reset(key::textLineHeight);
    QVERIFY(!banner->isVisible());
    QVERIFY(m_stub->requests.isEmpty());
    QCOMPARE(RemoteImages::instance().state(a), RemoteImages::State::Missing);
}

void TestRemoteImages::neverPreferenceDoesNotAsk()
{
    Settings::instance().setValue(key::remoteImages, QStringLiteral("never"));
    const auto view = openView(QStringLiteral("![one](%1)\n").arg(m_stub->url(QStringLiteral("/ok.png")).toString()));
    view->grab();
    QTest::qWait(300);
    auto *banner = view->findChild<QFrame *>(QStringLiteral("RemoteImageBanner"));
    QVERIFY(!banner || !banner->isVisible());
    QVERIFY(m_stub->requests.isEmpty());
    // Even a session decision to load does not override it.
    m_model->setRemoteConsent(MarkdownModel::RemoteConsent::Allow);
    view->grab();
    QTest::qWait(300);
    QVERIFY(m_stub->requests.isEmpty());
    // Back to asking: the bar comes up when the document was undecided.
    m_model->setRemoteConsent(MarkdownModel::RemoteConsent::Undecided);
    Settings::instance().reset(key::remoteImages);
    banner = view->findChild<QFrame *>(QStringLiteral("RemoteImageBanner"));
    QVERIFY(banner && banner->isVisible());
    QVERIFY(m_stub->requests.isEmpty());
}

void TestRemoteImages::consentIsSharedByViews()
{
    const auto view = openView(QStringLiteral("![one](%1)\n").arg(m_stub->url(QStringLiteral("/ok.png")).toString()));
    VisualView second(m_model);
    second.resize(600, 400);
    second.show();
    auto *first = view->findChild<QFrame *>(QStringLiteral("RemoteImageBanner"));
    auto *other = second.findChild<QFrame *>(QStringLiteral("RemoteImageBanner"));
    QVERIFY(first && first->isVisible() && other && other->isVisible());
    QTest::mouseClick(button(other, QStringLiteral("Don't load")), Qt::LeftButton);
    QVERIFY(!first->isVisible());
    QVERIFY(!other->isVisible());
    QTest::qWait(200);
    QVERIFY(m_stub->requests.isEmpty());
}

void TestRemoteImages::localImagesStillLoad()
{
    QTemporaryDir dir;
    QImage picture(60, 30, QImage::Format_RGB32);
    picture.fill(Qt::blue);
    QVERIFY(picture.save(dir.filePath(QStringLiteral("local.png"))));
    RenderContext ctx = context(false);
    ctx.baseDirectory = dir.path();
    const auto block = renderHtml(QStringLiteral("<p><img src=\"local.png\"></p>"), ctx);
    QVERIFY(!block->isPending());
    const auto point = block->pictureAt(QPointF(10, 5));
    QVERIFY(point);
    QCOMPARE(point->path, dir.filePath(QStringLiteral("local.png")));
    QVERIFY(point->url.isEmpty());
}

void TestRemoteImages::addressesAreJudged_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<bool>("allowed");
    const auto row = [](const char *host, bool allowed) {
        QTest::newRow(*host ? host : "(empty)") << QString::fromLatin1(host) << allowed;
    };
    // Public.
    row("93.184.216.34", true);
    row("8.8.8.8", true);
    row("2606:4700::1111", true);
    row("2a00:1450:4001:80b::200e", true);
    row("1572395042", true); // 93.184.216.34 in decimal.
    row("example.com", true);
    row("images.example.org.", true);
    row("localhost.example.com", true); // Only the last labels count.
    row("example.local.com", true);
    row("mylan.com", true);
    // Loopback, any spelling.
    row("127.0.0.1", false);
    row("127.255.255.254", false);
    row("2130706433", false); // Decimal.
    row("0x7f000001", false); // Hex.
    row("0x7F.1", false);
    row("0177.0.0.1", false); // Octal.
    row("017700000001", false);
    row("127.1", false); // Short.
    row("127.0.1", false);
    row("127.0.0.1.", false);
    row("::1", false);
    row("[::1]", false);
    row("0:0:0:0:0:0:0:1", false);
    // Private, link-local (cloud metadata), carrier-grade NAT.
    row("10.0.0.1", false);
    row("10.255.255.255", false);
    row("172.15.255.255", true);
    row("172.16.0.0", false);
    row("172.31.255.255", false);
    row("172.32.0.0", true);
    row("192.167.255.255", true);
    row("192.168.0.1", false);
    row("192.169.0.0", true);
    row("3232235777", false); // 192.168.1.1.
    row("169.254.169.254", false);
    row("2852039166", false); // 169.254.169.254.
    row("169.253.255.255", true);
    row("100.63.255.255", true);
    row("100.64.0.0", false);
    row("100.127.255.255", false);
    row("100.128.0.0", true);
    row("fc00::1", false);
    row("fd12:3456:789a::1", false);
    row("fe80::1", false);
    row("febf::1", false);
    row("fec0::1", false); // Old site-local.
    // Unspecified, multicast, reserved, broadcast.
    row("0.0.0.0", false);
    row("0", false);
    row("0.1.2.3", false);
    row("::", false);
    row("224.0.0.1", false);
    row("239.255.255.250", false);
    row("223.255.255.255", true);
    row("ff02::1", false);
    row("240.0.0.1", false);
    row("255.255.255.255", false);
    // IPv4 inside IPv6.
    row("::ffff:127.0.0.1", false);
    row("::ffff:7f00:1", false);
    row("::ffff:10.0.0.1", false);
    row("::ffff:169.254.169.254", false);
    row("::ffff:192.168.1.1", false);
    row("::ffff:93.184.216.34", true);
    row("::127.0.0.1", false);
    row("::ffff:0:127.0.0.1", false);
    row("64:ff9b::7f00:1", false);
    row("64:ff9b::a9fe:a9fe", false);
    row("64:ff9b::5db8:d822", true); // 93.184.216.34.
    row("64:ff9b:1::1", false);
    row("2002:7f00:1::1", false); // 6to4 of 127.0.0.1.
    row("2002:c0a8:101::1", false); // 6to4 of 192.168.1.1.
    row("2002:5db8:d822::1", true); // 6to4 of 93.184.216.34.
    row("2001:0:4136:e378::1", false); // Teredo.
    row("2001:db8::1", false); // Documentation.
    // Names of this machine and local networks.
    row("localhost", false);
    row("LOCALHOST.", false);
    row("foo.localhost", false);
    row("localhost.localdomain", false);
    row("printer.local", false);
    row("metadata.google.internal", false);
    row("nas.lan", false);
    row("router.home.arpa", false);
    row("intranet", false); // Single label.
    row("intranet.", false);
    row("", false);
}

void TestRemoteImages::addressesAreJudged()
{
    QFETCH(QString, host);
    QFETCH(bool, allowed);
    RemoteImages::setAllowLocalHostsForTests(false);
    QCOMPARE(RemoteImages::hostAllowed(host), allowed);
    if (const QHostAddress address = RemoteImages::literalAddress(host); !address.isNull()) {
        QCOMPARE(RemoteImages::isPublicAddress(address), allowed);
        QVERIFY(!RemoteImages::hostIsLocalName(host)); // An address is no name.
    } else if (!host.isEmpty()) {
        QCOMPARE(RemoteImages::hostIsLocalName(host), !allowed);
    }
    if (allowed)
        return; // Public hosts are not contacted from a test.
    // Refused for a picture and as a redirect target, before any request.
    const QString authority = host.contains(u':') && !host.startsWith(u'[') ? u'[' + host + u']' : host;
    const QUrl url(QStringLiteral("http://%1/x.png").arg(authority));
    QVERIFY(!RemoteImages::redirectAllowed(QUrl(QStringLiteral("http://example.com/")), url));
    QCOMPARE(RemoteImages::instance().request(url), RemoteImages::State::Failed);
}

void TestRemoteImages::lookedUpAddressesAreJudged()
{
    RemoteImages::setAllowLocalHostsForTests(false);
    const auto allowed = [](std::initializer_list<const char *> addresses) {
        QList<QHostAddress> list;
        for (const char *a : addresses)
            list.append(QHostAddress(QString::fromLatin1(a)));
        return RemoteImages::resolvedAddressesAllowed(list);
    };
    QVERIFY(allowed({"93.184.216.34"}));
    QVERIFY(allowed({"93.184.216.34", "2606:2800:220:1:248:1893:25c8:1946"}));
    QVERIFY(!allowed({}));
    // Any local address among them refuses the name.
    QVERIFY(!allowed({"93.184.216.34", "127.0.0.1"}));
    QVERIFY(!allowed({"10.0.0.5", "93.184.216.34"}));
    QVERIFY(!allowed({"93.184.216.34", "::ffff:169.254.169.254"}));
    QVERIFY(!allowed({"2606:4700::1111", "fd00::1"}));
}

void TestRemoteImages::localHostsAreNeverContacted()
{
    HttpStub second; // Where the redirects below point.
    const QString port = QString::number(second.url({}).port());

    // The stub itself, in any spelling: refused before anything is sent,
    // also when a document asks for it with consent given.
    RemoteImages::setAllowLocalHostsForTests(false);
    const QString stubPort = QString::number(m_stub->url({}).port());
    for (const char *form : {"127.0.0.1", "localhost", "[::1]", "2130706433", "0x7f000001", "127.1", "0177.0.0.1",
                             "[::ffff:127.0.0.1]"}) {
        const QUrl url(QStringLiteral("http://%1:%2/ok.png").arg(QString::fromLatin1(form), stubPort));
        QCOMPARE(RemoteImages::instance().request(url), RemoteImages::State::Failed);
        const auto block = renderHtml(imageTag(url), context(true));
        QVERIFY(!block->isPending());
        QVERIFY(!block->pictureAt(QPointF(5, 5)));
    }
    // Not even listed by the bar: they would never load.
    QVERIFY(RenderedBlock::remoteImages(htmlBlock(imageTag(m_stub->url(QStringLiteral("/ok.png")))), context(false)).isEmpty());

    // Redirects from a host that may be contacted (the stub, let through
    // for its first answer only) to local ones: refused at once, and nothing
    // reaches the second server.
    const QStringList targets = {
        second.url(QStringLiteral("/ok.png")).toString(),
        QStringLiteral("http://localhost:%1/ok.png").arg(port),
        QStringLiteral("http://[::1]:%1/ok.png").arg(port),
        QStringLiteral("http://[::ffff:127.0.0.1]:%1/ok.png").arg(port),
        QStringLiteral("http://2130706433:%1/ok.png").arg(port),
        QStringLiteral("http://10.0.0.1/ok.png"),
        QStringLiteral("http://192.168.0.1/ok.png"),
        QStringLiteral("http://169.254.169.254/latest/meta-data/"),
        QStringLiteral("http://intranet/ok.png"),
    };
    m_stub->onRequest = [] { RemoteImages::setAllowLocalHostsForTests(false); };
    for (const QString &target : targets) {
        RemoteImages::setAllowLocalHostsForTests(true);
        const QUrl url = m_stub->url(QStringLiteral("/to/") + QString::fromLatin1(QUrl::toPercentEncoding(target)));
        QCOMPARE(RemoteImages::instance().request(url), RemoteImages::State::Loading);
        // Well within the 10 s a connection attempt would be given.
        QTRY_COMPARE_WITH_TIMEOUT(RemoteImages::instance().state(url), RemoteImages::State::Failed, 3000);
    }
    QTest::qWait(300);
    QCOMPARE(m_stub->requests.size(), targets.size());
    QVERIFY(second.requests.isEmpty());

    // With local hosts let through, the same redirect is followed: the
    // refusals above were the address check.
    m_stub->onRequest = nullptr;
    RemoteImages::setAllowLocalHostsForTests(true);
    const QUrl followed = m_stub->url(QStringLiteral("/to/") + QString::fromLatin1(QUrl::toPercentEncoding(targets.first())));
    RemoteImages::instance().clearFailures();
    QCOMPARE(RemoteImages::instance().request(followed), RemoteImages::State::Loading);
    QTRY_COMPARE(RemoteImages::instance().state(followed), RemoteImages::State::Loaded);
    QCOMPARE(second.requests, QStringList{QStringLiteral("/ok.png")});
}

void TestRemoteImages::namesResolvingToThisMachineAreRefused()
{
    // A public name whose addresses are this machine's (localtest.me is kept
    // that way for such tests): only the lookup can tell. Needs DNS.
    const QHostInfo info = QHostInfo::fromName(QStringLiteral("localtest.me"));
    const QList<QHostAddress> addresses = info.addresses();
    if (addresses.isEmpty()
        || !std::all_of(addresses.begin(), addresses.end(), [](const QHostAddress &a) { return a.isLoopback(); }))
        QSKIP("localtest.me does not resolve to this machine here (no DNS?)");
    RemoteImages::setAllowLocalHostsForTests(false);
    QVERIFY(RemoteImages::hostAllowed(QStringLiteral("localtest.me"))); // Its name passes...
    const QUrl url(QStringLiteral("http://localtest.me:%1/ok.png").arg(m_stub->url({}).port()));
    QCOMPARE(RemoteImages::instance().request(url), RemoteImages::State::Loading);
    // ...its addresses don't.
    QTRY_COMPARE_WITH_TIMEOUT(RemoteImages::instance().state(url), RemoteImages::State::Failed, 5000);
    QTest::qWait(300);
    QVERIFY(m_stub->requests.isEmpty());
}

void TestRemoteImages::namesKeepTheirHostHeader()
{
    // A name is looked up and the request sent to the address found, still
    // naming the host it is for (local hosts let through: offline, only
    // "localhost" has an address).
    const QUrl url(QStringLiteral("http://localhost:%1/ok.png").arg(m_stub->url({}).port()));
    RemoteImages::instance().request(url);
    QTRY_COMPARE(RemoteImages::instance().state(url), RemoteImages::State::Loaded);
    QCOMPARE(m_stub->headers.size(), 1);
    const QByteArray expected = QStringLiteral("\r\nhost: localhost:%1").arg(url.port()).toLatin1();
    QVERIFY2(m_stub->headers.first().toLower().contains(expected), m_stub->headers.first().constData());
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestRemoteImages)
#include "test_remote_images.moc"
