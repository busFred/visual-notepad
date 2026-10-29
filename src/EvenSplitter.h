// A QSplitter whose handle is 1 px wide on screen but easy to grab, and
// whose double-click distributes the panes evenly (VS Code's sash behaviour).
#pragma once

#include <QSplitter>
#include <QSplitterHandle>

namespace vn {

class EvenSplitter : public QSplitter
{
    Q_OBJECT

public:
    explicit EvenSplitter(Qt::Orientation orientation, QWidget *parent = nullptr);

    // Gives every visible pane the same share of the splitter.
    void distributeEvenly();

    // Pixels on each side of the 1-px handle that also grab it.
    static constexpr int grabMargin = 2;

protected:
    QSplitterHandle *createHandle() override;
};

} // namespace vn
