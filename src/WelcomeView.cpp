#include "WelcomeView.h"

#include "Commands.h"
#include "Settings.h"
#include "Theme.h"

#include <QDir>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace vn {

namespace {

constexpr int maxRecent = 6;

QPushButton *linkButton(const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(QStringLiteral("WelcomeLink"));
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

void clear(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        if (item->layout())
            clear(item->layout());
        delete item;
    }
}

} // namespace

WelcomeView::WelcomeView(QWidget *parent)
    : QWidget(parent)
    , m_commands(new QVBoxLayout)
    , m_recent(new QVBoxLayout)
{
    setObjectName(QStringLiteral("Welcome"));
    setAttribute(Qt::WA_StyledBackground);

    auto *icon = new QLabel(this);
    icon->setPixmap(QIcon(QStringLiteral(":/vn/app/visnotepad.svg")).pixmap(QSize(112, 112)));
    icon->setAlignment(Qt::AlignCenter);
    auto *fade = new QGraphicsOpacityEffect(icon);
    fade->setOpacity(0.35);
    icon->setGraphicsEffect(fade);

    auto *title = new QLabel(tr("Visual Notepad"), this);
    title->setObjectName(QStringLiteral("WelcomeTitle"));
    title->setAlignment(Qt::AlignCenter);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.6);
    title->setFont(titleFont);

    m_commands->setSpacing(2);
    m_recent->setSpacing(2);

    auto *column = new QVBoxLayout;
    column->setSpacing(6);
    column->addWidget(icon);
    column->addWidget(title);
    column->addSpacing(18);
    column->addLayout(m_commands);
    column->addSpacing(18);
    column->addLayout(m_recent);

    auto *outer = new QGridLayout(this);
    outer->setRowStretch(0, 2);
    outer->setRowStretch(2, 3);
    outer->setColumnStretch(0, 1);
    outer->setColumnStretch(2, 1);
    outer->addLayout(column, 1, 1);
}

void WelcomeView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Shortcuts and recent files may have changed since last time.
    rebuild();
}

void WelcomeView::rebuild()
{
    clear(m_commands);
    clear(m_recent);

    static const char *const ids[] = {"file.new", "file.open", "edit.preferences", "help.shortcuts"};
    for (const char *id : ids) {
        const Command *command = vn::command(QLatin1String(id));
        if (!command)
            continue;
        auto *row = new QHBoxLayout;
        QString text = command->text;
        text.remove(QChar(0x2026)); // "Open…" reads better without the ellipsis here.
        auto *link = linkButton(text, this);
        const QList<QKeySequence> keys = shortcutsFor(command->id);
        auto *keyLabel = new QLabel(keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::NativeText), this);
        keyLabel->setObjectName(QStringLiteral("WelcomeKey"));
        row->addWidget(link);
        row->addStretch(1);
        row->addSpacing(32);
        row->addWidget(keyLabel);
        m_commands->addLayout(row);
        connect(link, &QPushButton::clicked, this, [this, id = command->id] { Q_EMIT commandRequested(id); });
    }

    const QStringList files = Settings::instance().recentFiles();
    if (files.isEmpty())
        return;
    auto *heading = new QLabel(tr("Recent"), this);
    heading->setObjectName(QStringLiteral("WelcomeHeading"));
    m_recent->addWidget(heading);
    for (const QString &path : files.mid(0, maxRecent)) {
        const QFileInfo info(path);
        auto *row = new QHBoxLayout;
        auto *link = linkButton(info.fileName(), this);
        link->setToolTip(QDir::toNativeSeparators(path));
        auto *folder = new QLabel(QDir::toNativeSeparators(info.absolutePath()), this);
        folder->setObjectName(QStringLiteral("WelcomeKey"));
        folder->setMaximumWidth(320);
        folder->setText(folder->fontMetrics().elidedText(folder->text(), Qt::ElideMiddle, 320));
        row->addWidget(link);
        row->addSpacing(12);
        row->addWidget(folder);
        row->addStretch(1);
        m_recent->addLayout(row);
        connect(link, &QPushButton::clicked, this, [this, path] { Q_EMIT recentFileRequested(path); });
    }
}

} // namespace vn
