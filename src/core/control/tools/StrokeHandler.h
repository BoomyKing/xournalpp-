/*
 * Xournal++
 *
 * Handles input of strokes
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>    // for unique_ptr
#include <optional>  // for optional
#include <vector>    // for vector

#include <gdk/gdk.h>  // for GdkEventKey

#include "model/PageRef.h"          // for PageRef
#include "model/Point.h"            // for Point
#include "util/Range.h"             // for Range
#include "util/raii/GSourceURef.h"  // for GSourceURef

#include "InputHandler.h"            // for InputHandler
#include "SnapToGridInputHandler.h"  // for SnapToGridInputHandler

class Control;
class Layer;
class PositionInputData;
class Stroke;

namespace xoj::util {
template <class T>
class DispatchPool;
};

namespace xoj::view {
class OverlayView;
class Repaintable;
class StrokeToolView;
};  // namespace xoj::view

namespace StrokeStabilizer {
class Base;
class Active;
}  // namespace StrokeStabilizer

/**
 * @brief The stroke handler draws a stroke on a XojPageView
 *
 * The stroke is drawn using a cairo_surface_t* as a mask:
 * As the pointer moves on the canvas single segments are
 * drawn opaquely on the initially transparent masking
 * surface. The surface is used to mask the stroke
 * when drawing it to the XojPageView
 */
class StrokeHandler: public InputHandler {
public:
    StrokeHandler(Control* control, const PageRef& page);
    ~StrokeHandler() override;

    void onSequenceCancelEvent() override;
    bool onMotionNotifyEvent(const PositionInputData& pos, double zoom) override;
    void onButtonReleaseEvent(const PositionInputData& pos, double zoom) override;
    void onButtonPressEvent(const PositionInputData& pos, double zoom) override;
    void onButtonDoublePressEvent(const PositionInputData& pos, double zoom) override;
    bool onKeyPressEvent(const KeyEvent& event) override;
    bool onKeyReleaseEvent(const KeyEvent& event) override;

    /**
     * @brief Add a straight line to the stroke (if the movement is valid).
     * The line may be subdivided into smaller segments if the pressure variation is too big.
     * @param point The endpoint of the added line
     */
    void paintTo(Point point);

    auto createView(xoj::view::Repaintable* parent) const -> std::unique_ptr<xoj::view::OverlayView> override;

    const std::shared_ptr<xoj::util::DispatchPool<xoj::view::StrokeToolView>>& getViewPool() const;

protected:
    /**
     * @brief Unconditionally add a segment to the stroke.
     * Warning: it does not set the width properly nor test if the motion is valid. Use paintTo instead.
     * @param point The endpoint of the added segment
     */
    void drawSegmentTo(const Point& point);

    void strokeRecognizerDetected(std::unique_ptr<Stroke> recognized, Layer* layer);

    /// Finalizes the stroke using the provided pressure as last point
    void finalizeStroke(double pressure);

    /// Whether holding the pen still may turn the stroke into a shape (see Settings::getStrokeHoldRecognizerEnabled)
    virtual bool allowsHoldRecognition() const { return true; }

protected:
    Point buttonDownPoint;  // used for tapSelect and filtering - never snapped to grid.
    SnapToGridInputHandler snappingHandler;

    bool hasPressure;

private:
    /**
     * @brief Pointer to the Stabilizer instance
     */
    std::unique_ptr<StrokeStabilizer::Base> stabilizer;

    std::shared_ptr<xoj::util::DispatchPool<xoj::view::StrokeToolView>> viewPool;

    friend class StrokeStabilizer::Active;

    static constexpr double MAX_WIDTH_VARIATION = 0.3;

    /*
     * Hold to recognize: when the pen stays still for a moment while drawing, the stroke is replaced by the
     * recognized shape. Until the pen is lifted, the shape keeps following the pen: a line keeps its starting
     * point and its other end follows the pen, other shapes are scaled and rotated around their centre.
     */
    bool holdRecognitionActive() const;
    void watchForHold(const Point& penPos);
    static void onHoldTimeout(StrokeHandler* self);
    void recognizeHeldStroke();
    void updateHeldShape(const Point& penPos);
    void updateHeldShape(const Point& penPos, Range repaint);
    /// Whether the held shape can replace the stroke when the pen is lifted (and is not, e.g., a line of length 0)
    bool isUsableHeldShape() const;

    /// Pen movements smaller than this (in screen pixels) still count as holding still
    static constexpr double HOLD_TOLERANCE = 4.0;
    /// Rotations smaller than this (in radians) are ignored, so that resizing a held shape does not tilt it
    static constexpr double HELD_SHAPE_ROTATION_DEADZONE = 0.12;
    static constexpr double MIN_HELD_SHAPE_SCALE = 0.05;
    /// Held lines shorter than this (in screen pixels) are dropped in favour of the hand-drawn stroke
    static constexpr double MIN_HELD_LINE_LENGTH = 2 * HOLD_TOLERANCE;

    xoj::util::GSourceURef holdTimer;
    Point holdWatchPoint;  ///< Where the pen was when the timer was last restarted
    Point lastPenPoint;    ///< Latest pen position (not snapped)
    double holdZoom = 1.0;

    std::unique_ptr<Stroke> heldShape;    ///< The recognized shape following the pen, if any
    std::vector<Point> heldShapeBase;     ///< Its points at the moment it was recognized
    Point heldShapeGrab;                  ///< The pen position at that moment
    Point heldShapeCenter;                ///< Centre for scaling and rotating closed shapes
    std::optional<double> heldShapeTilt;  ///< For polygons: how far (in radians) their longest side is from level
    bool heldShapeIsLine = false;
};
