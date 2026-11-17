#include "StrokeHandler.h"

#include <algorithm>  // for max, min
#include <array>      // for array
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
#include "undo/DeleteUndoAction.h"                          // for DeleteUndoAction
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

namespace {
/// The thickness a shape recognized from the stroke gets: the stroke's average thickness. Points without a valid
/// pressure value are ignored, so they cannot make the shape invisible.
auto recognizedShapeWidth(const Stroke& stroke) -> double {
    if (!stroke.hasPressure()) {
        return stroke.getWidth();
    }
    double sum = 0;
    size_t count = 0;
    for (const Point& p: stroke.getPointVector()) {
        if (p.z > 0 && std::isfinite(p.z)) {
            sum += p.z;
            count++;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : stroke.getWidth();
}

// --- scribble geometry (begin) ---
/// A back-and-forth scribble (like shading) turns back at least this many times along its main direction (4 passes)
constexpr int SCRIBBLE_MIN_REVERSALS = 3;
/// A zigzag scribble ("mmmm" drawn over a word) swings across its main direction at least this many times
constexpr int ZIGZAG_MIN_REVERSALS = 4;
/// Scribbles are at least this long (pt): smaller wiggles are dots and jitter
constexpr double SCRIBBLE_MIN_LENGTH = 8;
/// A zigzag is at least this high (pt) and this part of its length, so that a shaky strike-through isn't one
constexpr double ZIGZAG_MIN_HEIGHT = 3;
constexpr double ZIGZAG_MIN_HEIGHT_PART = 0.12;
/// A turn counts once the stroke has gone back this part of its extent in that direction (smaller wiggles, like the
/// loops of handwriting, don't count)
constexpr double SCRIBBLE_REVERSAL_PART = 0.3;
/// The scribbled area is the scribble's box (along its main direction), grown by this part of its height
constexpr double SCRIBBLE_MARGIN_PART = 0.3;
constexpr double SCRIBBLE_MIN_MARGIN = 2;
/// A stroke under a scribble is erased when at least this part of its length lies in the scribbled area
constexpr double SCRIBBLE_COVERED_PART = 0.6;
/// Zigzags look a lot like handwriting (m, n, w), so they only erase what they cross at least this many times (a
/// curly bracket drawn next to text doesn't)
constexpr int ZIGZAG_MIN_CROSSINGS = 3;

enum class ScribbleKind { NONE, BACK_AND_FORTH, ZIGZAG };

/// The scribbled area: a box along the scribble's main direction (u) and across it (w = u rotated by 90 degrees)
struct ScribbleArea {
    double ux = 1, uy = 0;
    double loU = 0, hiU = 0, loW = 0, hiW = 0;

    auto along(const Point& p) const -> double { return p.x * ux + p.y * uy; }
    auto across(const Point& p) const -> double { return -p.x * uy + p.y * ux; }
    auto contains(const Point& p) const -> bool {
        const double a = along(p), c = across(p);
        return loU <= a && a <= hiU && loW <= c && c <= hiW;
    }
    /// Axis-aligned bounding box: minX, maxX, minY, maxY
    auto bounds() const -> std::array<double, 4> {
        std::array<double, 4> b{std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
                                std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()};
        for (double a: {loU, hiU}) {
            for (double c: {loW, hiW}) {
                const double x = a * ux - c * uy, y = a * uy + c * ux;
                b[0] = std::min(b[0], x);
                b[1] = std::max(b[1], x);
                b[2] = std::min(b[2], y);
                b[3] = std::max(b[3], y);
            }
        }
        return b;
    }
};

/// Counts how often the points turn back in the given direction (with hysteresis); also returns their extent in it
auto countReversals(const std::vector<Point>& pts, double ux, double uy, double& extent) -> int {
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    for (const Point& p: pts) {
        const double s = p.x * ux + p.y * uy;
        lo = std::min(lo, s);
        hi = std::max(hi, s);
    }
    extent = hi - lo;
    const double turn = SCRIBBLE_REVERSAL_PART * extent;
    if (!(turn > 0)) {
        return 0;
    }
    int reversals = 0, dir = 0;
    const double start = pts.front().x * ux + pts.front().y * uy;
    double extreme = start;
    for (const Point& p: pts) {
        const double s = p.x * ux + p.y * uy;
        if (dir == 0) {
            if (std::abs(s - start) > turn) {
                dir = s > start ? 1 : -1;
                extreme = s;
            }
        } else if ((s - extreme) * dir > 0) {
            extreme = s;
        } else if ((extreme - s) * dir > turn) {
            reversals++;
            dir = -dir;
            extreme = s;
        }
    }
    return reversals;
}

/// Whether the points are a scribble crossing something out, and which kind; also returns the scribbled area
auto classifyScribble(const std::vector<Point>& pts, ScribbleArea& area) -> ScribbleKind {
    if (pts.size() < 12) {
        return ScribbleKind::NONE;
    }
    // main direction: the principal axis of the points
    double mx = 0, my = 0;
    for (const Point& p: pts) {
        mx += p.x;
        my += p.y;
    }
    mx /= static_cast<double>(pts.size());
    my /= static_cast<double>(pts.size());
    double sxx = 0, syy = 0, sxy = 0;
    for (const Point& p: pts) {
        sxx += (p.x - mx) * (p.x - mx);
        syy += (p.y - my) * (p.y - my);
        sxy += (p.x - mx) * (p.y - my);
    }
    const double angle = 0.5 * std::atan2(2 * sxy, sxx - syy);
    area.ux = std::cos(angle);
    area.uy = std::sin(angle);

    double length = 0, height = 0;
    const int along = countReversals(pts, area.ux, area.uy, length);
    const int across = countReversals(pts, -area.uy, area.ux, height);

    ScribbleKind kind = ScribbleKind::NONE;
    if (length >= SCRIBBLE_MIN_LENGTH && along >= SCRIBBLE_MIN_REVERSALS) {
        kind = ScribbleKind::BACK_AND_FORTH;
    } else if (length >= SCRIBBLE_MIN_LENGTH && across >= ZIGZAG_MIN_REVERSALS &&
               height >= std::max(ZIGZAG_MIN_HEIGHT, ZIGZAG_MIN_HEIGHT_PART * length)) {
        kind = ScribbleKind::ZIGZAG;
    }
    if (kind == ScribbleKind::NONE) {
        return kind;
    }

    area.loU = area.loW = std::numeric_limits<double>::max();
    area.hiU = area.hiW = std::numeric_limits<double>::lowest();
    for (const Point& p: pts) {
        area.loU = std::min(area.loU, area.along(p));
        area.hiU = std::max(area.hiU, area.along(p));
        area.loW = std::min(area.loW, area.across(p));
        area.hiW = std::max(area.hiW, area.across(p));
    }
    const double margin = std::max(SCRIBBLE_MIN_MARGIN, SCRIBBLE_MARGIN_PART * height);
    area.loU -= margin;
    area.hiU += margin;
    area.loW -= margin;
    area.hiW += margin;
    return kind;
}

/// The part of the length of a path that lies in the area (sampled every half point)
auto coveredPart(const std::vector<Point>& pts, const ScribbleArea& area) -> double {
    if (pts.empty()) {
        return 0;
    }
    size_t inside = area.contains(pts.front()) ? 1 : 0, total = 1;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        const Point& a = pts[i];
        const Point& b = pts[i + 1];
        const auto n = static_cast<size_t>(std::clamp(std::hypot(b.x - a.x, b.y - a.y) / 0.5, 1.0, 200.0));
        for (size_t k = 1; k <= n; k++) {
            const double t = static_cast<double>(k) / static_cast<double>(n);
            inside += area.contains(Point(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t)) ? 1 : 0;
        }
        total += n;
    }
    return static_cast<double>(inside) / static_cast<double>(total);
}

/// How often two paths cross each other (stops counting at limit)
auto countCrossings(const std::vector<Point>& a, const std::vector<Point>& b, int limit) -> int {
    auto side = [](const Point& o, const Point& p, const Point& q) {
        return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x);
    };
    int crossings = 0;
    for (size_t i = 0; i + 1 < a.size(); i++) {
        const double minX = std::min(a[i].x, a[i + 1].x), maxX = std::max(a[i].x, a[i + 1].x);
        const double minY = std::min(a[i].y, a[i + 1].y), maxY = std::max(a[i].y, a[i + 1].y);
        for (size_t j = 0; j + 1 < b.size(); j++) {
            if (std::max(b[j].x, b[j + 1].x) < minX || std::min(b[j].x, b[j + 1].x) > maxX ||
                std::max(b[j].y, b[j + 1].y) < minY || std::min(b[j].y, b[j + 1].y) > maxY) {
                continue;
            }
            if (side(b[j], b[j + 1], a[i]) * side(b[j], b[j + 1], a[i + 1]) < 0 &&
                side(a[i], a[i + 1], b[j]) * side(a[i], a[i + 1], b[j + 1]) < 0 && ++crossings >= limit) {
                return crossings;
            }
        }
    }
    return crossings;
}
// --- scribble geometry (end) ---

auto hasFinitePoints(const Stroke& s) -> bool {
    const auto& pts = s.getPointVector();
    return std::all_of(pts.begin(), pts.end(), [](const Point& p) { return std::isfinite(p.x) && std::isfinite(p.y); });
}
}  // namespace

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
    if (!recognized || recognized->getPointCount() < 2 || !hasFinitePoints(*recognized)) {
        return;  // Not a shape: keep drawing freehand
    }
    recognized->setWidth(recognizedShapeWidth(*stroke));

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
        // Polygons are kept level while they are rotated by less than SLANT_TOLERANCE away from level; this is
        // measured on their longest side. (Rotating a circle changes nothing, it needs no such care.)
        heldShapeTilt.reset();
        if (heldShapeBase.size() <= MAX_POLYGON_SIDES + 1) {
            double longest = -1;
            for (size_t i = 0; i + 1 < heldShapeBase.size(); i++) {
                const Point& a = heldShapeBase[i];
                const Point& b = heldShapeBase[i + 1];
                if (const double len = a.lineLengthTo(b); len > longest) {
                    longest = len;
                    heldShapeTilt = std::remainder(std::atan2(b.y - a.y, b.x - a.x), M_PI / 2);
                }
            }
        }
    }

    const Range oldRange(stroke->boundingRect());
    heldShape = std::move(recognized);
    updateHeldShape(lastPenPoint, oldRange);
}

auto StrokeHandler::isUsableHeldShape() const -> bool {
    if (!heldShape || !hasFinitePoints(*heldShape)) {
        return false;
    }
    if (heldShapeIsLine) {
        // A held line whose end was moved back onto its start would become a dot
        const auto& pts = heldShape->getPointVector();
        return pts.size() == 2 && pts.front().lineLengthTo(pts.back()) * holdZoom >= MIN_HELD_LINE_LENGTH;
    }
    return true;
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
        // The pen wobbles a little when it is lifted: that must neither resize nor tilt the shape
        const bool penMoved = penPos.lineLengthTo(heldShapeGrab) * holdZoom > HOLD_TOLERANCE;
        if (penMoved && r0 > 1e-6 && r1 > 1e-6) {
            scale = std::max(r1 / r0, MIN_HELD_SHAPE_SCALE);
            angle = std::remainder(std::atan2(py, px) - std::atan2(gy, gx), 2 * M_PI);
            // Do not rotate when the pen only moves a little: resizing alone stays straight
            angle = std::abs(angle) < HELD_SHAPE_ROTATION_DEADZONE ? 0.0 : snappingHandler.snapAngle(angle, false);
            if (heldShapeTilt) {
                // A polygon rotated to (nearly) level becomes exactly level
                if (const double deviation = std::remainder(*heldShapeTilt + angle, M_PI / 2);
                    std::abs(deviation) < SLANT_TOLERANCE) {
                    angle -= deviation;
                }
            }
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

    if (!heldShape && control->getSettings()->getScratchOutEnabled() && scratchOut(layer)) {
        return;  // the scribble erased the strokes under it, and isn't kept itself
    }
    if (page->isEndless() && control->getSettings()->getLineStartsNewPage() && lineStartsNewPage()) {
        return;  // the line became a page break
    }

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
        if (isUsableHeldShape()) {
            // The pen was held still and the stroke recognized: keep the shape where the pen left it
            strokeRecognizerDetected(std::move(heldShape), layer);
            return;
        }
        // E.g. the end of a held line was moved back onto its start: keep the hand-drawn stroke instead of a dot
        Range rg = Range(heldShape->boundingRect()).unite(Range(stroke->boundingRect()));
        rg.addPadding(std::max(heldShape->getWidth(), stroke->getWidth()));
        heldShape.reset();
        this->viewPool->dispatch(xoj::view::StrokeToolView::LIVE_SHAPE_REQUEST, *stroke, rg);
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

auto StrokeHandler::lineStartsNewPage() -> bool {
    const Stroke* line = (heldShape && isUsableHeldShape()) ? heldShape.get() : stroke.get();
    const auto& pts = line->getPointVector();
    if (pts.size() < 2) {
        return false;
    }
    double minX = pts.front().x, maxX = minX, minY = pts.front().y, maxY = minY, length = 0, sumY = 0;
    for (size_t i = 0; i < pts.size(); i++) {
        minX = std::min(minX, pts[i].x);
        maxX = std::max(maxX, pts[i].x);
        minY = std::min(minY, pts[i].y);
        maxY = std::max(maxY, pts[i].y);
        sumY += pts[i].y;
        if (i > 0) {
            length += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        }
    }
    const double width = maxX - minX;
    // across almost the whole page, level and straight (not a zigzag or an underline)
    if (width < LINE_BREAK_MIN_WIDTH_PART * page->getWidth() ||
        maxY - minY > std::max(LINE_BREAK_MAX_TILT, LINE_BREAK_MAX_TILT_PART * width) ||
        length > LINE_BREAK_MAX_PATH_PART * width) {
        return false;
    }
    const double y = sumY / static_cast<double>(pts.size());

    Range rg(stroke->boundingRect());
    if (heldShape) {
        rg = rg.unite(Range(heldShape->boundingRect()));
        rg.addPadding(heldShape->getWidth());
        heldShape.reset();
    }
    rg.addPadding(stroke->getWidth());
    this->viewPool->dispatchAndClear(xoj::view::StrokeToolView::CANCELLATION_REQUEST, rg);
    stroke.reset();

    // split once this input sequence is over (splitting changes the pages the input handlers belong to)
    struct Split {
        Control* control;
        PageRef page;
        double y;
    };
    g_idle_add(
            +[](gpointer d) -> gboolean {
                auto* s = static_cast<Split*>(d);
                s->control->splitPageAt(s->page, s->y);
                delete s;
                return G_SOURCE_REMOVE;
            },
            new Split{control, page, y});
    return true;
}

auto StrokeHandler::scratchOut(Layer* layer) -> bool {
    const auto& pts = stroke->getPointVector();
    ScribbleArea area;
    const ScribbleKind kind = classifyScribble(pts, area);
    if (kind == ScribbleKind::NONE) {
        return false;
    }
    const auto [minX, maxX, minY, maxY] = area.bounds();
    std::vector<const Element*> covered;
    int crossings = 0;
    for (const Element* e: layer->getElementsView()) {
        if (e->getType() != ELEMENT_STROKE) {
            continue;
        }
        const auto r = e->boundingRect();
        if (r.x > maxX || r.x + r.width < minX || r.y > maxY || r.y + r.height < minY) {
            continue;
        }
        const auto& points = static_cast<const Stroke*>(e)->getPointVector();
        if (coveredPart(points, area) >= SCRIBBLE_COVERED_PART) {
            covered.push_back(e);
            if (kind == ScribbleKind::ZIGZAG) {
                if (crossings < ZIGZAG_MIN_CROSSINGS) {
                    crossings += countCrossings(pts, points, ZIGZAG_MIN_CROSSINGS - crossings);
                }
            }
        }
    }
    if (covered.empty()) {
        return false;  // a scribble on empty paper (shading) stays
    }
    if (kind == ScribbleKind::ZIGZAG && crossings < ZIGZAG_MIN_CROSSINGS) {
        return false;  // probably handwriting next to other strokes
    }

    const auto box = stroke->boundingRect();
    Range range(box);
    range.addPadding(stroke->getWidth());
    auto undo = std::make_unique<DeleteUndoAction>(page, true);
    Document* doc = control->getDocument();
    for (const Element* e: covered) {
        Range r(e->boundingRect());
        r.addPadding(static_cast<const Stroke*>(e)->getWidth());
        range = range.unite(r);
        doc->lock();
        auto [element, pos] = layer->removeElement(e);
        doc->unlock();
        if (pos != -1) {
            undo->addElement(layer, std::move(element), pos);
        }
    }
    control->getUndoRedoHandler()->addUndoAction(std::move(undo));

    Range scribbleRange(box);
    scribbleRange.addPadding(stroke->getWidth());
    this->viewPool->dispatchAndClear(xoj::view::StrokeToolView::CANCELLATION_REQUEST, scribbleRange);
    stroke.reset();
    page->fireRangeChanged(range);
    return true;
}

void StrokeHandler::strokeRecognizerDetected(std::unique_ptr<Stroke> recognized, Layer* layer) {
    // The recognized shape gets the hand-drawn stroke's average thickness
    recognized->setWidth(recognizedShapeWidth(*stroke));

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
