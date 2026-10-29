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

// "Save &As…" -> "Save As…"; "&&" is a literal ampersand.
QString stripMnemonic(const QString &text)
{
    QString result;
    result.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('&') && i + 1 < text.size())
            ++i;
        result += text[i];
    }
    return result;
}

QList<Command> build()
{
    const QString file = tr("File");
    const QString edit = tr("Edit");
    const QString selection = tr("Selection");
    const QString format = tr("Format");
    const QString view = tr("View");
    const QString help = tr("Help");
    // Titles carry the menu mnemonic; each letter is unique within its menu
    // (a repeated one only selects in QMenu, it doesn't trigger).
    QList<Command> list = {
        {QStringLiteral("file.new"), file, tr("&New File"), {k("Ctrl+N")}},
        {QStringLiteral("file.newWindow"), file, tr("New &Window"), {k("Ctrl+Shift+N")}},
        {QStringLiteral("file.open"), file, tr("&Open…"), {k("Ctrl+O")}},
        {QStringLiteral("file.save"), file, tr("&Save"), {k("Ctrl+S")}},
        {QStringLiteral("file.saveAs"), file, tr("Save &As…"), {k("Ctrl+Shift+S")}},
        {QStringLiteral("file.closeTab"), file, tr("&Close Tab"), {k("Ctrl+W")}},
        {QStringLiteral("file.closeWindow"), file, tr("Clos&e Window"), {k("Ctrl+Shift+W")}},
        {QStringLiteral("file.quit"), file, tr("&Quit"), {k("Ctrl+Q")}},

        {QStringLiteral("edit.undo"), edit, tr("&Undo"), {k("Ctrl+Z")}},
        {QStringLiteral("edit.redo"), edit, tr("&Redo"), {k("Ctrl+Y"), k("Ctrl+Shift+Z")}},
        {QStringLiteral("edit.cut"), edit, tr("Cu&t"), {k("Ctrl+X")}},
        {QStringLiteral("edit.copy"), edit, tr("&Copy"), {k("Ctrl+C")}},
        {QStringLiteral("edit.paste"), edit, tr("&Paste"), {k("Ctrl+V")}},
        {QStringLiteral("edit.find"), edit, tr("&Find"), {k("Ctrl+F")}},
        {QStringLiteral("edit.replace"), edit, tr("Rep&lace"), {k("Ctrl+H")}},
        {QStringLiteral("edit.findNext"), edit, tr("Find &Next"), {k("F3")}},
        {QStringLiteral("edit.findPrevious"), edit, tr("Find Pre&vious"), {k("Shift+F3")}},
        {QStringLiteral("edit.findNextSelection"), edit, tr("Find Next &Selection"), {k("Ctrl+F3")}},
        {QStringLiteral("edit.findPreviousSelection"), edit, tr("Find Previous Selecti&on"), {k("Ctrl+Shift+F3")}},
        {QStringLiteral("edit.goToLine"), edit, tr("&Go to Line…"), {k("Ctrl+G")}},
        {QStringLiteral("edit.toggleComment"), edit, tr("Toggle Line Co&mment"), {k("Ctrl+/")}},
        {QStringLiteral("edit.indentLines"), edit, tr("&Indent Line"), {k("Ctrl+]")}},
        {QStringLiteral("edit.outdentLines"), edit, tr("Out&dent Line"), {k("Ctrl+[")}},
        {QStringLiteral("edit.trimTrailingWhitespace"), edit, tr("Trim Trailing &Whitespace"),
         {k("Ctrl+K, Ctrl+X")}},
        {QStringLiteral("edit.format"), edit, tr("Form&at Document"), {pk("Shift+Alt+F", "Ctrl+Shift+I")}},
        {QStringLiteral("edit.preferences"), edit, tr("Pr&eferences…"), {k("Ctrl+,")}},

        {QStringLiteral("selection.selectAll"), selection, tr("&Select All"), {k("Ctrl+A")}},
        {QStringLiteral("selection.expand"), selection, tr("&Expand Selection"), {k("Shift+Alt+Right")}},
        {QStringLiteral("selection.shrink"), selection, tr("S&hrink Selection"), {k("Shift+Alt+Left")}},
        {QStringLiteral("selection.addCursorAbove"), selection, tr("&Add Cursor Above"),
         {pk("Ctrl+Alt+Up", "Shift+Alt+Up")}},
        {QStringLiteral("selection.addCursorBelow"), selection, tr("A&dd Cursor Below"),
         {pk("Ctrl+Alt+Down", "Shift+Alt+Down")}},
        {QStringLiteral("selection.addNextOccurrence"), selection, tr("Add &Next Occurrence"), {k("Ctrl+D")}},
        {QStringLiteral("selection.selectAllOccurrences"), selection, tr("Select All &Occurrences"),
         {k("Ctrl+Shift+L")}},
        {QStringLiteral("selection.addCursorsToLineEnds"), selection, tr("Add C&ursors to Line Ends"),
         {k("Shift+Alt+I")}},
        {QStringLiteral("selection.moveLastToNextMatch"), selection, tr("&Move Last Selection to Next Find Match"),
         {k("Ctrl+K, Ctrl+D")}},
        {QStringLiteral("selection.expandLine"), selection, tr("Expand &Line Selection"), {k("Ctrl+L")}},
        {QStringLiteral("selection.cursorUndo"), selection, tr("&Cursor Undo"), {}},
        {QStringLiteral("selection.cursorRedo"), selection, tr("Cursor &Redo"), {}},
        {QStringLiteral("selection.copyLineUp"), selection, tr("Co&py Line Up"),
         {pk("Shift+Alt+Up", "Ctrl+Shift+Alt+Up")}},
        {QStringLiteral("selection.copyLineDown"), selection, tr("Copy Line Do&wn"),
         {pk("Shift+Alt+Down", "Ctrl+Shift+Alt+Down")}},
        {QStringLiteral("selection.moveLineUp"), selection, tr("Mo&ve Line Up"), {k("Alt+Up")}},
        {QStringLiteral("selection.moveLineDown"), selection, tr("Move L&ine Down"), {k("Alt+Down")}},
        {QStringLiteral("selection.deleteLine"), selection, tr("Dele&te Line"), {k("Ctrl+Shift+K")}},

        {QStringLiteral("format.bold"), format, tr("&Bold"), {k("Ctrl+B")}},
        {QStringLiteral("format.italic"), format, tr("&Italic"), {k("Ctrl+I")}},
        {QStringLiteral("format.underline"), format, tr("&Underline"), {k("Ctrl+U")}},
        {QStringLiteral("format.strikethrough"), format, tr("&Strikethrough"), {k("Alt+Shift+5")}},
        {QStringLiteral("format.inlineCode"), format, tr("Inline &Code"), {k("Ctrl+Shift+`")}},
        {QStringLiteral("format.heading1"), format, tr("Heading &1"), {k("Ctrl+1")}},
        {QStringLiteral("format.heading2"), format, tr("Heading &2"), {k("Ctrl+2")}},
        {QStringLiteral("format.heading3"), format, tr("Heading &3"), {k("Ctrl+3")}},
        {QStringLiteral("format.heading4"), format, tr("Heading &4"), {k("Ctrl+4")}},
        {QStringLiteral("format.heading5"), format, tr("Heading &5"), {k("Ctrl+5")}},
        {QStringLiteral("format.heading6"), format, tr("Heading &6"), {k("Ctrl+6")}},
        {QStringLiteral("format.headingUp"), format, tr("&Promote Heading"), {}},
        {QStringLiteral("format.headingDown"), format, tr("&Demote Heading"), {}},
        {QStringLiteral("format.quote"), format, tr("&Quote"), {k("Ctrl+Shift+Q")}},
        {QStringLiteral("format.orderedList"), format, tr("&Numbered List"), {k("Ctrl+Shift+[")}},
        {QStringLiteral("format.bulletList"), format, tr("Bulleted &List"), {k("Ctrl+Shift+]")}},
        {QStringLiteral("format.taskList"), format, tr("&Task List"), {k("Alt+C")}},
        {QStringLiteral("format.link"), format, tr("Lin&k"), {}},
        {QStringLiteral("format.codeBlock"), format, tr("Code Bl&ock"), {}},

        {QStringLiteral("view.outline"), view, tr("&Outline"), {k("Ctrl+Shift+B")}},
        {QStringLiteral("view.modeSource"), view, tr("&Source"), {}},
        {QStringLiteral("view.modeSplit"), view, tr("Source / &Visual"), {k("Ctrl+K, V")}},
        {QStringLiteral("view.modeVisual"), view, tr("Visual &Editor"), {}},
        {QStringLiteral("view.splitEditor"), view, tr("Spl&it Editor"), {k("Ctrl+\\")}},
        // VS Code binds all four to Ctrl+K Ctrl+\ (editorActions.ts
        // SplitEditorLeftAction etc.), which clash with each other and with
        // Split Editor Orthogonal; they have no default here.
        {QStringLiteral("view.splitEditorUp"), view, tr("Split Editor &Up"), {}},
        {QStringLiteral("view.splitEditorDown"), view, tr("Split Editor &Down"), {}},
        {QStringLiteral("view.splitEditorLeft"), view, tr("Split Editor &Left"), {}},
        {QStringLiteral("view.splitEditorRight"), view, tr("Split Editor &Right"), {}},
        // No default in VS Code either (editorActions.ts FocusNextGroup).
        {QStringLiteral("view.focusNextGroup"), view, tr("Focus &Next Editor Group"), {}},
        {QStringLiteral("view.focusPreviousGroup"), view, tr("Focus &Previous Editor Group"), {}},
        {QStringLiteral("view.moveToNewWindow"), view, tr("&Move Tab to New Window"), {k("Ctrl+K, O")}},
        {QStringLiteral("view.copyToNewWindow"), view, tr("&Copy Tab to New Window"), {}},
        {QStringLiteral("view.scrollPageUp"), view, tr("Scroll Page Up"), {k("Alt+PgUp")}},
        {QStringLiteral("view.scrollPageDown"), view, tr("Scroll Page Down"), {k("Alt+PgDown")}},
        {QStringLiteral("view.wordWrap"), view, tr("&Word Wrap"), {k("Alt+Z")}},
        {QStringLiteral("view.statusBar"), view, tr("St&atus Bar"), {}},
        {QStringLiteral("edit.changeLanguage"), view, tr("Change Lan&guage Mode…"), {k("Ctrl+K, M")}},
        {QStringLiteral("view.zoomIn"), view, tr("&Zoom In"), {k("Ctrl+="), k("Ctrl++")}},
        {QStringLiteral("view.zoomOut"), view, tr("Zoom Ou&t"), {k("Ctrl+-")}},
        {QStringLiteral("view.zoomReset"), view, tr("Reset Zoom"), {k("Ctrl+0")}},
        {QStringLiteral("view.fullScreen"), view, tr("&Full Screen"), {k("F11")}},

        {QStringLiteral("help.shortcuts"), help, tr("&Keyboard Shortcuts"), {k("Ctrl+K, Ctrl+S")}},
        {QStringLiteral("help.about"), help, tr("&About Visual Notepad"), {}},
    };
    for (Command &c : list) {
        c.menuText = c.text;
        c.text = stripMnemonic(c.text);
    }
    return list;
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

// What Qt does when two commands clash (all commands are QActions in
// MainWindow; QShortcutMap::find, qtbase src/gui/kernel/qshortcutmap.cpp):
//  - Identical sequences: both entries land in "identicals" (find(), ~line
//    430-445); dispatchEvent() (~line 561) sends the shortcut event to each
//    in turn and QAction::event (qaction.cpp ~line 1046) sees
//    QShortcutEvent::isAmbiguous(), logs "Ambiguous shortcut overload" and
//    does not trigger. Neither command runs.
//  - A single key equal to the first key(s) of another command's chord
//    ("Ctrl+K" vs "Ctrl+K, V"): the single key is an ExactMatch and the chord
//    only a PartialMatch. find() ignores partials once identicals exist
//    ("We don't need partials, if we have identicals", ~line 438), so the
//    exact one fires immediately and the chord can never be completed.
//    Only the chord is lost, but the chord's owner is unusable all the same.
//  - If the single key's command is disabled, find() keeps the partial and
//    consumes the key (~line 456), so the chord works but the single-key
//    command does not; commands are enabled and disabled at run time, so the
//    outcome is not predictable and we report the clash either way.
static bool clashes(const QKeySequence &a, const QKeySequence &b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    // matches() is Exact for equal, Partial when a is a proper prefix of b.
    return a.matches(b) != QKeySequence::NoMatch || b.matches(a) != QKeySequence::NoMatch;
}

QList<ShortcutConflict> shortcutConflicts(const QString &id)
{
    QList<ShortcutConflict> result;
    const QList<QKeySequence> mine = shortcutsFor(id);
    for (const Command &c : commands()) {
        if (c.id == id)
            continue;
        const QList<QKeySequence> theirs = shortcutsFor(c.id);
        for (const QKeySequence &a : mine) {
            for (const QKeySequence &b : theirs) {
                if (clashes(a, b))
                    result.append({c.id, a, b});
            }
        }
    }
    return result;
}

QStringList menuTitles()
{
    // Notepad's Format menu takes O (Alt+F is File).
    return {tr("&File"), tr("&Edit"), tr("&Selection"), tr("F&ormat"), tr("&View"), tr("&Help")};
}

QString menuForMnemonic(const QKeySequence &sequence)
{
    for (const QString &title : menuTitles()) {
        if (!sequence.isEmpty() && sequence == QKeySequence::mnemonic(title))
            return stripMnemonic(title);
    }
    return {};
}

QStringList conflictingCommands(const QKeySequence &sequence, const QString &id)
{
    QStringList result;
    for (const Command &c : commands()) {
        if (c.id == id || result.contains(c.id))
            continue;
        for (const QKeySequence &other : shortcutsFor(c.id)) {
            if (clashes(sequence, other)) {
                result.append(c.id);
                break;
            }
        }
    }
    return result;
}

} // namespace vn
