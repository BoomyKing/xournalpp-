/*
 * Xournal++
 *
 * Undo action for dragging a corner of a line or polygon
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <string>  // for string
#include <vector>  // for vector

#include "model/PageRef.h"  // for PageRef
#include "model/Point.h"    // for Point

#include "UndoAction.h"  // for UndoAction

class Control;
class Stroke;

class ShapeEditUndoAction: public UndoAction {
public:
    ShapeEditUndoAction(const PageRef& page, Stroke* stroke, std::vector<Point> before, std::vector<Point> after);

    bool undo(Control* control) override;
    bool redo(Control* control) override;
    std::string getText() override;

private:
    void setPoints(const std::vector<Point>& points, Control* control);

    Stroke* stroke;
    std::vector<Point> before;
    std::vector<Point> after;
};
