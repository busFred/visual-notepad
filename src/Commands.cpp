#include "Commands.h"

#include "Settings.h"

#include <QCoreApplication>

namespace vn {

namespace {

QKeySequence k(const char *text)
{
    return QKeySequence(QString::fromLatin1(text), QKeySequence::PortableText);
}

// Platform-specific default, following VS Code's Windows and Linux keymaps.
QKeySequence pk(const char *windows, const char *other)
{
#ifdef Q_OS_WIN
    Q_UNUSED(other);
    return k(windows);
#else
    Q_UNUSED(windows);
    return k(other);
#endif
}

QString tr(const char *text)
{
    return QCoreApplication::translate("Commands", text);
}

QList<Command> build()
{
    const QString file = tr("File");
    const QString edit = tr("Edit");
    const QString selection = tr("Selection");
    const QString view = tr("View");
    const QString help = tr("Help");
    return {
        {QStringLiteral("file.new"), file, tr("New File"), {k("Ctrl+N")}},
        {QStringLiteral("file.newWindow"), file, tr("New Window"), {k("Ctrl+Shift+N")}},
        {QStringLiteral("file.open"), file, tr("Open…"), {k("Ctrl+O")}},
        {QStringLiteral("file.save"), file, tr("Save"), {k("Ctrl+S")}},
        {QStringLiteral("file.saveAs"), file, tr("Save As…"), {k("Ctrl+Shift+S")}},
        {QStringLiteral("file.closeTab"), file, tr("Close Tab"), {k("Ctrl+W")}},
        {QStringLiteral("file.closeWindow"), file, tr("Close Window"), {k("Ctrl+Shift+W")}},
        {QStringLiteral("file.quit"), file, tr("Quit"), {k("Ctrl+Q")}},

        {QStringLiteral("edit.undo"), edit, tr("Undo"), {k("Ctrl+Z")}},
        {QStringLiteral("edit.redo"), edit, tr("Redo"), {k("Ctrl+Y"), k("Ctrl+Shift+Z")}},
        {QStringLiteral("edit.cut"), edit, tr("Cut"), {k("Ctrl+X")}},
        {QStringLiteral("edit.copy"), edit, tr("Copy"), {k("Ctrl+C")}},
        {QStringLiteral("edit.paste"), edit, tr("Paste"), {k("Ctrl+V")}},
        {QStringLiteral("edit.find"), edit, tr("Find"), {k("Ctrl+F")}},
        {QStringLiteral("edit.replace"), edit, tr("Replace"), {k("Ctrl+H")}},
        {QStringLiteral("edit.findNext"), edit, tr("Find Next"), {k("F3")}},
        {QStringLiteral("edit.findPrevious"), edit, tr("Find Previous"), {k("Shift+F3")}},
        {QStringLiteral("edit.goToLine"), edit, tr("Go to Line…"), {k("Ctrl+G")}},
        {QStringLiteral("edit.toggleComment"), edit, tr("Toggle Line Comment"), {k("Ctrl+/")}},
        {QStringLiteral("edit.format"), edit, tr("Format Document"), {pk("Shift+Alt+F", "Ctrl+Shift+I")}},
        {QStringLiteral("edit.preferences"), edit, tr("Preferences…"), {k("Ctrl+,")}},

        {QStringLiteral("selection.selectAll"), selection, tr("Select All"), {k("Ctrl+A")}},
        {QStringLiteral("selection.expand"), selection, tr("Expand Selection"), {k("Shift+Alt+Right")}},
        {QStringLiteral("selection.shrink"), selection, tr("Shrink Selection"), {k("Shift+Alt+Left")}},
        {QStringLiteral("selection.addCursorAbove"), selection, tr("Add Cursor Above"),
         {pk("Ctrl+Alt+Up", "Shift+Alt+Up")}},
        {QStringLiteral("selection.addCursorBelow"), selection, tr("Add Cursor Below"),
         {pk("Ctrl+Alt+Down", "Shift+Alt+Down")}},
        {QStringLiteral("selection.addNextOccurrence"), selection, tr("Add Next Occurrence"), {k("Ctrl+D")}},
        {QStringLiteral("selection.selectAllOccurrences"), selection, tr("Select All Occurrences"),
         {k("Ctrl+Shift+L")}},
        {QStringLiteral("selection.copyLineUp"), selection, tr("Copy Line Up"),
         {pk("Shift+Alt+Up", "Ctrl+Shift+Alt+Up")}},
        {QStringLiteral("selection.copyLineDown"), selection, tr("Copy Line Down"),
         {pk("Shift+Alt+Down", "Ctrl+Shift+Alt+Down")}},
        {QStringLiteral("selection.moveLineUp"), selection, tr("Move Line Up"), {k("Alt+Up")}},
        {QStringLiteral("selection.moveLineDown"), selection, tr("Move Line Down"), {k("Alt+Down")}},
        {QStringLiteral("selection.deleteLine"), selection, tr("Delete Line"), {k("Ctrl+Shift+K")}},

        {QStringLiteral("view.outline"), view, tr("Outline"), {k("Ctrl+B")}},
        {QStringLiteral("view.modeSource"), view, tr("Source"), {}},
        {QStringLiteral("view.modeSplit"), view, tr("Source / Visual"), {k("Ctrl+K, V")}},
        {QStringLiteral("view.modeVisual"), view, tr("Visual Editor"), {}},
        {QStringLiteral("view.splitEditor"), view, tr("Split Editor"), {k("Ctrl+\\")}},
        {QStringLiteral("view.moveToNewWindow"), view, tr("Move Tab to New Window"), {k("Ctrl+K, O")}},
        {QStringLiteral("view.wordWrap"), view, tr("Word Wrap"), {k("Alt+Z")}},
        {QStringLiteral("view.statusBar"), view, tr("Status Bar"), {}},
        {QStringLiteral("edit.changeLanguage"), view, tr("Change Language Mode…"), {k("Ctrl+K, M")}},
        {QStringLiteral("view.zoomIn"), view, tr("Zoom In"), {k("Ctrl+="), k("Ctrl++")}},
        {QStringLiteral("view.zoomOut"), view, tr("Zoom Out"), {k("Ctrl+-")}},
        {QStringLiteral("view.zoomReset"), view, tr("Reset Zoom"), {k("Ctrl+0")}},
        {QStringLiteral("view.fullScreen"), view, tr("Full Screen"), {k("F11")}},

        {QStringLiteral("help.shortcuts"), help, tr("Keyboard Shortcuts"), {k("Ctrl+K, Ctrl+S")}},
        {QStringLiteral("help.about"), help, tr("About Visual Notepad"), {}},
    };
}

} // namespace

const QList<Command> &commands()
{
    static const QList<Command> list = build();
    return list;
}

const Command *command(const QString &id)
{
    for (const Command &c : commands()) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

QList<QKeySequence> shortcutsFor(const QString &id)
{
    Settings &settings = Settings::instance();
    if (settings.hasShortcutOverride(id))
        return settings.shortcutOverride(id);
    const Command *c = command(id);
    return c ? c->defaults : QList<QKeySequence>();
}

QStringList conflictingCommands(const QKeySequence &sequence, const QString &id)
{
    QStringList result;
    if (sequence.isEmpty())
        return result;
    for (const Command &c : commands()) {
        if (c.id != id && shortcutsFor(c.id).contains(sequence))
            result.append(c.id);
    }
    return result;
}

} // namespace vn
