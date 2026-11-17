/*
 * Xournal++
 *
 * Splits a page in two at a given height: everything below moves to a new page inserted after it
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <string>  // for string
#include <vector>  // for vector

#include "model/Element.h"  // for Element
#include "model/PageRef.h"  // for PageRef

#include "UndoAction.h"  // for UndoAction

class Control;

class SplitPageUndoAction: public UndoAction {
public:
    /**
     * Splits the page at height y: the page ends there, and what is below y (going by each element's middle) moves to
     * a new page inserted after it. The new page is endless like the page, and at least one sheet high.
     */
    static void splitPage(Control* control, const PageRef& page, double y);

    bool undo(Control* control) override;
    bool redo(Control* control) override;
    std::string getText() override;
    std::vector<PageRef> getPages() override;

private:
    SplitPageUndoAction(const PageRef& page, const PageRef& newPage, double y, double oldHeight);

    void split(Control* control);
    void join(Control* control);

    PageRef newPage;
    double y;
    double oldHeight;
    /// Where the moved elements were on their layer before the split, per layer (in increasing order)
    std::vector<std::vector<Element::Index>> movedPositions;
};
