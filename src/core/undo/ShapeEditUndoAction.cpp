#include "ShapeEditUndoAction.h"

#include <utility>  // for move

#include "control/Control.h"  // for Control
#include "model/Document.h"   // for Document
#include "model/Stroke.h"     // for Stroke
#include "model/XojPage.h"    // for XojPage
#include "util/Range.h"       // for Range
#include "util/i18n.h"        // for _

ShapeEditUndoAction::ShapeEditUndoAction(const PageRef& page, Stroke* stroke, std::vector<Point> before,
                                         std::vector<Point> after):
        UndoAction("ShapeEditUndoAction"), stroke(stroke), before(std::move(before)), after(std::move(after)) {
    this->page = page;
}

void ShapeEditUndoAction::setPoints(const std::vector<Point>& points, Control* control) {
    Document* doc = control->getDocument();
    doc->lock();
    Range range(this->stroke->boundingRect());
    this->stroke->setPointVector(points);
    range = range.unite(Range(this->stroke->boundingRect()));
    doc->unlock();
    this->page->fireRangeChanged(range);
}

auto ShapeEditUndoAction::undo(Control* control) -> bool {
    setPoints(this->before, control);
    this->undone = true;
    return true;
}

auto ShapeEditUndoAction::redo(Control* control) -> bool {
    setPoints(this->after, control);
    this->undone = false;
    return true;
}

auto ShapeEditUndoAction::getText() -> std::string { return _("Edit shape"); }
