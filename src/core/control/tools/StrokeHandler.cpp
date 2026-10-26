#include "StrokeHandler.h"

#include <algorithm>  // for max, min
#include <cmath>      // for ceil, pow, abs, hypot, atan2, remainder
#include <limits>     // for numeric_limits
#include <memory>     // for unique_ptr, mak...
#include <utility>    // for move
#include <vector>     // for vector

#include <gdk/gdk.h>  // for GdkEventKey

#include "control/Control.h"                                // for Control
#include "control/ToolEnums.h"                              // for DRAWING_TYPE_ST...
#include "control/ToolHandler.h"                            // for ToolHandler
#include "control/layer/LayerController.h"                  // for LayerController
#include "control/settings/Settings.h"                      // for Settings
#include "control/settings/SettingsEnums.h"                 // for EmptyLastPageAppendType
#include "control/shaperecognizer/ShapeRecognizer.h"        // for ShapeRecognizer
#include "control/tools/InputHandler.h"                     // for InputHandler::P...
#include "control/tools/SnapToGridInputHandler.h"           // for SnapToGridInput...
#include "gui/inputdevices/PositionInputData.h"             // for PositionInputData
#include "model/Document.h"                                 // for Document
#include "model/Element.h"
#include "model/Layer.h"                                    // for Layer
#include "model/LineStyle.h"                                // for LineStyle
#include "model/Stroke.h"                                   // for Stroke, STROKE_...
#include "model/XojPage.h"                                  // for XojPage
#include "undo/InsertUndoAction.h"                          // for InsertUndoAction
#include "undo/RecognizerUndoAction.h"                      // for RecognizerUndoA...
#include "undo/UndoRedoHandler.h"                           // for UndoRedoHandler
#include "util/Assert.h"                                    // for xoj_assert
#include "util/DispatchPool.h"                              // for DispatchPool
#include "util/Range.h"                                     // for Range
#include "util/Rectangle.h"                                 // for Rectangle, util
#include "view/overlays/StrokeToolFilledHighlighterView.h"  // for StrokeToolFilledHighlighterView
#include "view/overlays/StrokeToolFilledView.h"             // for StrokeToolFilledView
#include "view/overlays/StrokeToolView.h"                   // for StrokeToolView

#include "StrokeStabilizer.h"  // for Base, get

using xoj::util::Rectangle;

StrokeHandler::StrokeHandler(Control* control, const PageRef& page):
        InputHandler(control, page),
        snappingHandler(control->getSettings()),
        stabilizer(StrokeStabilizer::get(control->getSettings())),
        viewPool(std::make_shared<xoj::util::DispatchPool<xoj::view::StrokeToolView>>()) {
    snappingHandler.setPageRef(page);
}

StrokeHandler::~StrokeHandler() = default;

auto StrokeHandler::onKeyPressEvent(const KeyEvent&) -> bool { return false; }
auto StrokeHandler::onKeyReleaseEvent(const KeyEvent&) -> bool { return false; }

auto StrokeHandler::onMotionNotifyEvent(const PositionInputData& pos, double zoom) -> bool {
    if (!stroke) {
        return false;
    }

    if (pos.pressure == 0) {
        /**
         * Some devices emit a move event with pressure 0 when lifting the stylus tip
         * Ignore those events
         */
        return true;
    }

    const Point penPos(pos.x / zoom, pos.y / zoom);
    if (heldShape) {
        // The stroke has been recognized: the shape follows the pen until it is lifted
        updateHeldShape(penPos);
        return true;
    }

    stabilizer->processEvent(pos);

    if (holdRecognitionActive()) {
        holdZoom = zoom;
        watchForHold(penPos);
    }
    return true;
}

auto StrokeHandler::holdRecognitionActive() const -> bool {
    if (!allowsHoldRecognition() || !stroke) {
        return false;
    }
    const StrokeTool tool = stroke->getToolType();
    return (tool == StrokeTool::PEN || tool == StrokeTool::HIGHLIGHTER) &&
           control->getSettings()->getStrokeHoldRecognizerEnabled();
}

void StrokeHandler::watchForHold(const Point& penPos) {
    lastPenPoint = penPos;
    // Small movements (hand tremor, sensor noise) still count as holding still
    if (holdTimer && penPos.lineLengthTo(holdWatchPoint) * holdZoom <= HOLD_TOLERANCE) {
        return;
    }
    holdWatchPoint = penPos;
    holdTimer = g_timeout_add(static_cast<guint>(control->getSettings()->getStrokeHoldRecognizerDelay()),
                              xoj::util::wrap_for_once_v<onHoldTimeout>, this);
}

void StrokeHandler::onHoldTimeout(StrokeHandler* self) {
    self->holdTimer.consume();
    self->recognizeHeldStroke();
}

void StrokeHandler::recognizeHeldStroke() {
    if (!stroke || heldShape) {
        return;
    }
    ShapeRecognizer reco;
    auto recognized = reco.recognizePatterns(stroke.get(), control->getSettings()->getStrokeRecognizerMinSize());
    if (!recognized || recognized->getPointCount() < 2) {
        return;  // Not a shape: keep drawing freehand
    }
    recognized->setWidth(stroke->hasPressure() ? stroke->getAvgPressure() : stroke->getWidth());

    heldShapeGrab = lastPenPoint;
    heldShapeIsLine = recognized->getPointCount() == 2;
    if (heldShapeIsLine) {
        // The line starts where the stroke started, its other end follows the pen
        const Point& start = stroke->getPoint(0);
        heldShapeBase = {Point(start.x, start.y), Point(lastPenPoint.x, lastPenPoint.y)};
    } else {
        heldShapeBase = recognized->getPointVector();
        const auto box = recognized->boundingRect();
        heldShapeCenter = Point(box.x + box.width / 2, box.y + box.height / 2);
    }

    const Range oldRange(stroke->boundingRect());
    heldShape = std::move(recognized);
    updateHeldShape(lastPenPoint, oldRange);
}

void StrokeHandler::updateHeldShape(const Point& penPos) { updateHeldShape(penPos, Range(heldShape->boundingRect())); }

void StrokeHandler::updateHeldShape(const Point& penPos, Range repaint) {
    std::vector<Point> pts;
    pts.reserve(heldShapeBase.size());
    if (heldShapeIsLine) {
        const Point& start = heldShapeBase.front();
        Point end;
        const double angle = std::atan2(penPos.y - start.y, penPos.x - start.x);
        if (const double deviation = std::remainder(angle, M_PI / 2); std::abs(deviation) < SLANT_TOLERANCE) {
            // Nearly horizontal or vertical: make it exactly so (and snap the end to the grid, if enabled)
            const double length = std::hypot(penPos.x - start.x, penPos.y - start.y);
            end = snappingHandler.snapToGrid(Point(start.x + length * std::cos(angle - deviation),
                                                   start.y + length * std::sin(angle - deviation)),
                                             false);
            if (std::abs(std::remainder(angle - deviation, M_PI)) < 1e-9) {
                end.y = start.y;  // grid snapping must not tilt it again
            } else {
                end.x = start.x;
            }
        } else {
            // Same snapping as the line tool: rotation snapping and/or grid snapping, depending on the settings
            end = snappingHandler.snap(penPos, start, false);
        }
        pts.emplace_back(start.x, start.y);
        pts.emplace_back(end.x, end.y);
    } else {
        // Scale and rotate around the centre so that the point grabbed at recognition follows the pen
        const double gx = heldShapeGrab.x - heldShapeCenter.x;
        const double gy = heldShapeGrab.y - heldShapeCenter.y;
        const double px = penPos.x - heldShapeCenter.x;
        const double py = penPos.y - heldShapeCenter.y;
        const double r0 = std::hypot(gx, gy);
        const double r1 = std::hypot(px, py);
        double scale = 1.0;
        double angle = 0.0;
        if (r0 > 1e-6 && r1 > 1e-6) {
            scale = std::max(r1 / r0, MIN_HELD_SHAPE_SCALE);
            angle = std::remainder(std::atan2(py, px) - std::atan2(gy, gx), 2 * M_PI);
            // Do not rotate when the pen only moves a little: resizing alone stays straight
            angle = std::abs(angle) < HELD_SHAPE_ROTATION_DEADZONE ? 0.0 : snappingHandler.snapAngle(angle, false);
        }
        const double c = std::cos(angle) * scale;
        const double s = std::sin(angle) * scale;
        for (const Point& q: heldShapeBase) {
            const double dx = q.x - heldShapeCenter.x;
            const double dy = q.y - heldShapeCenter.y;
            pts.emplace_back(heldShapeCenter.x + c * dx - s * dy, heldShapeCenter.y + s * dx + c * dy);
        }
    }
    heldShape->setPointVector(std::move(pts));

    repaint = repaint.unite(Range(heldShape->boundingRect()));
    repaint.addPadding(heldShape->getWidth());
    this->viewPool->dispatch(xoj::view::StrokeToolView::LIVE_SHAPE_REQUEST, *heldShape, repaint);
}

void StrokeHandler::paintTo(Point point) {
    if (this->hasPressure && point.z > 0.0) {
        point.z *= this->stroke->getWidth();
    }

    size_t pointCount = stroke->getPointCount();

    if (pointCount > 0) {
        Point endPoint = stroke->getPoint(pointCount - 1);
        double distance = point.lineLengthTo(endPoint);
        if (distance < PIXEL_MOTION_THRESHOLD) {  //(!validMotion(point, endPoint)) {
            if (pointCount == 1 && this->hasPressure && endPoint.z < point.z) {
                // Record the possible increase in pressure for the first point
                this->stroke->setLastPressure(point.z);
                this->viewPool->dispatch(xoj::view::StrokeToolView::THICKEN_FIRST_POINT_REQUEST, point.z);
            }
            return;
        }
        if (this->hasPressure) {
            /**
             * Both device and tool are pressure sensitive
             */
            if (const double widthDelta = point.z - endPoint.z;
                - widthDelta > MAX_WIDTH_VARIATION || widthDelta > MAX_WIDTH_VARIATION) {
                /**
                 * If the width variation is to big, decompose into shorter segments.
                 * Those segments can not be shorter than PIXEL_MOTION_THRESHOLD
                 */
                double nbSteps = std::min(std::ceil(std::abs(widthDelta) / MAX_WIDTH_VARIATION),
                                          std::floor(distance / PIXEL_MOTION_THRESHOLD));
                double stepLength = 1.0 / nbSteps;
                Point increment((point.x - endPoint.x) * stepLength, (point.y - endPoint.y) * stepLength,
                                widthDelta * stepLength);
                endPoint.z += increment.z;

                for (int i = 1; i < static_cast<int>(nbSteps); i++) {  // The last step is done below
                    endPoint.x += increment.x;
                    endPoint.y += increment.y;
                    endPoint.z += increment.z;
                    drawSegmentTo(endPoint);
                }
            }
        }
    }
    drawSegmentTo(point);
}

void StrokeHandler::drawSegmentTo(const Point& point) {

    this->stroke->addPoint(this->hasPressure ? point : Point(point.x, point.y));
    this->viewPool->dispatch(xoj::view::StrokeToolView::ADD_POINT_REQUEST, this->stroke->getPointVector().back());
    return;
}

void StrokeHandler::onSequenceCancelEvent() {
    holdTimer.cancel();
    if (this->stroke) {
        Range rg(this->stroke->boundingRect());
        if (heldShape) {
            rg = rg.unite(Range(heldShape->boundingRect()));
            rg.addPadding(heldShape->getWidth());
            heldShape.reset();
        }
        this->viewPool->dispatchAndClear(xoj::view::StrokeToolView::CANCELLATION_REQUEST, rg);
        stroke.reset();
    }
}

void StrokeHandler::finalizeStroke(double pressure) {
    if (!stroke) {
        return;
    }

    /**
     * The stabilizer may have added a gap between the end of the stroke and the input device
     * Fill this gap.
     */
    stabilizer->finalizeStroke();

    // Backward compatibility and also easier to handle for me;-)
    // I cannot draw a line with one point, to draw a visible line I need two points,
    // twice the same Point is also OK
    if (auto const& pv = stroke->getPointVector(); pv.size() == 1) {
        const Point pt = pv.front();  // Make a copy, otherwise stroke->addPoint(pt); in UB
        if (this->hasPressure) {
            // Pressure inference provides a pressure value to the last event. Most devices set this value to 0.
            const double newPressure = std::max(pt.z, pressure * this->stroke->getWidth());
            this->stroke->setLastPressure(newPressure);
            this->viewPool->dispatch(xoj::view::StrokeToolView::THICKEN_FIRST_POINT_REQUEST, newPressure);
        }
        stroke->addPoint(pt);
    }

    stroke->freeUnusedPointItems();
}

void StrokeHandler::onButtonReleaseEvent(const PositionInputData& pos, double zoom) {
    holdTimer.cancel();
    if (!stroke) {
        return;
    }
    finalizeStroke(pos.pressure);

    Layer* layer = page->getSelectedLayer();

    UndoRedoHandler* undo = control->getUndoRedoHandler();
    undo->addUndoAction(std::make_unique<InsertUndoAction>(page, layer, stroke.get()));

    Settings* settings = control->getSettings();
    if (settings->getEmptyLastPageAppend() == EmptyLastPageAppendType::OnDrawOfLastPage) {
        auto* doc = control->getDocument();
        doc->lock_shared();
        auto pdfPageCount = doc->getPdfPageCount();
        auto lastPage = doc->getPageCount() - 1;
        doc->unlock_shared();
        if (pdfPageCount == 0) {
            auto currentPage = control->getCurrentPageNo();
            if (currentPage == lastPage) {
                control->insertNewPage(currentPage + 1, true);
            }
        }
    }

    if (heldShape) {
        // The pen was held still and the stroke recognized: keep the shape where the pen left it
        strokeRecognizerDetected(std::move(heldShape), layer);
        return;
    }

    ToolHandler* h = control->getToolHandler();
    if (h->getDrawingType() == DRAWING_TYPE_SHAPE_RECOGNIZER) {
        ShapeRecognizer reco;

        auto recognized = reco.recognizePatterns(stroke.get(), control->getSettings()->getStrokeRecognizerMinSize());

        if (recognized) {
            // strokeRecognizerDetected handles the repainting and the deletion of the views.
            strokeRecognizerDetected(std::move(recognized), layer);
            return;
        }
    }

    auto ptr = stroke.get();
    Document* doc = control->getDocument();
    doc->lock();
    layer->addElement(std::move(stroke));
    doc->unlock();

    // Blitt the stroke to the page's buffer and delete all views.
    // Passing the empty Range() as no actual redrawing is necessary at this point
    this->viewPool->dispatchAndClear(xoj::view::StrokeToolView::FINALIZATION_REQUEST, Range());

    page->fireElementChanged(ptr);
}

void StrokeHandler::strokeRecognizerDetected(std::unique_ptr<Stroke> recognized, Layer* layer) {
    // The recognized shape gets the hand-drawn stroke's average thickness
    double width = stroke->getWidth();
    if (stroke->hasPressure()) {
        double sum = 0;
        size_t count = 0;
        for (const Point& p: stroke->getPointVector()) {
            if (p.z > 0 && std::isfinite(p.z)) {
                sum += p.z;
                count++;
            }
        }
        if (count > 0) {
            width = sum / static_cast<double>(count);
        }
    }
    recognized->setWidth(width);

    // snapping
    if (control->getSettings()->getSnapRecognizedShapesEnabled()) {
        Rectangle<double> oldSnappedBounds = recognized->getSnappedBounds();
        Point topLeft = Point(oldSnappedBounds.x, oldSnappedBounds.y);
        Point topLeftSnapped = snappingHandler.snapToGrid(topLeft, false);

        recognized->move(topLeftSnapped.x - topLeft.x, topLeftSnapped.y - topLeft.y);
        Rectangle<double> snappedBounds = recognized->getSnappedBounds();
        Point belowRight = Point(snappedBounds.x + snappedBounds.width, snappedBounds.y + snappedBounds.height);
        Point belowRightSnapped = snappingHandler.snapToGrid(belowRight, false);

        // A (nearly) horizontal or vertical line has a (nearly) zero height or width. Stretching that dimension
        // would multiply it by 0 or by a huge factor, making the shape invisible or sending it far off the page.
        auto factor = [](double snappedSize, double size) {
            constexpr double MIN_SIZE = 1.0;  // pt
            const double f = snappedSize / size;
            return (std::abs(size) >= MIN_SIZE && std::abs(snappedSize) >= MIN_SIZE && std::isfinite(f) && f > 0) ? f :
                                                                                                                    1.0;
        };
        const double fx = factor(belowRightSnapped.x - topLeftSnapped.x, snappedBounds.width);
        const double fy = factor(belowRightSnapped.y - topLeftSnapped.y, snappedBounds.height);
        // Snapping to the grid moves and stretches the shape slightly; it should not change its thickness
        recognized->scale(topLeftSnapped.x, topLeftSnapped.y, fx, fy, 0, true);
    }

    UndoRedoHandler* undo = control->getUndoRedoHandler();
    auto recognizedPtr = recognized.get();
    auto strokePtr = stroke.get();
    undo->addUndoAction(std::make_unique<RecognizerUndoAction>(page, layer, std::move(stroke), recognizedPtr));

    Document* doc = control->getDocument();
    doc->lock();
    layer->addElement(std::move(recognized));
    doc->unlock();

    Range range(recognizedPtr->getX(), recognizedPtr->getY());
    range.addPoint(recognizedPtr->getX() + recognizedPtr->getElementWidth(),
                   recognizedPtr->getY() + recognizedPtr->getElementHeight());

    range.addPoint(strokePtr->getX(), strokePtr->getY());
    range.addPoint(strokePtr->getX() + strokePtr->getElementWidth(), strokePtr->getY() + strokePtr->getElementHeight());

    this->viewPool->dispatch(xoj::view::StrokeToolView::STROKE_REPLACEMENT_REQUEST, *recognizedPtr);

    // Blitt the new stroke to the page's buffer, delete all the views and refresh the area (so the recognized stroke
    // gets displayed instead of the old one).
    this->viewPool->dispatchAndClear(xoj::view::StrokeToolView::FINALIZATION_REQUEST, range);
    page->fireElementChanged(recognizedPtr);
}

void StrokeHandler::onButtonPressEvent(const PositionInputData& pos, double zoom) {
    xoj_assert(!stroke);

    this->buttonDownPoint.x = pos.x / zoom;
    this->buttonDownPoint.y = pos.y / zoom;

    stroke = createStroke(this->control);

    this->hasPressure = this->stroke->getToolType().isPressureSensitive() && pos.pressure != Point::NO_PRESSURE;

    const double width = this->hasPressure ? pos.pressure * stroke->getWidth() : Point::NO_PRESSURE;
    stroke->addPoint(Point(this->buttonDownPoint.x, this->buttonDownPoint.y, width));

    stabilizer->initialize(this, zoom, pos);

    heldShape.reset();
    holdTimer.cancel();
    if (holdRecognitionActive()) {
        holdZoom = zoom;
        watchForHold(this->buttonDownPoint);
    }
}

void StrokeHandler::onButtonDoublePressEvent(const PositionInputData&, double) {
    // nothing to do
}

auto StrokeHandler::createView(xoj::view::Repaintable* parent) const -> std::unique_ptr<xoj::view::OverlayView> {
    xoj_assert(this->stroke);
    const Stroke& s = *this->stroke;
    if (s.getFill() != -1) {
        if (s.getToolType() == StrokeTool::HIGHLIGHTER) {
            // Filled highlighter requires to wipe the mask entirely at every iteration
            // It has a dedicated view class.
            return std::make_unique<xoj::view::StrokeToolFilledHighlighterView>(this, s, parent);
        } else {
            return std::make_unique<xoj::view::StrokeToolFilledView>(this, s, parent);
        }
    } else {
        return std::make_unique<xoj::view::StrokeToolView>(this, s, parent);
    }
}

auto StrokeHandler::getViewPool() const -> const std::shared_ptr<xoj::util::DispatchPool<xoj::view::StrokeToolView>>& {
    return viewPool;
}
