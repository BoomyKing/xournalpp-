/*
 * Xournal++
 *
 * Part of the Xournal shape recognizer
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>

class Stroke;

/**
 * @brief A more forgiving recognizer for closed, hand-drawn triangles and quadrilaterals.
 *
 * The main recognizer fits at most four straight segments to the stroke, starting where the stroke starts. It
 * therefore fails on closed shapes that start in the middle of a side, start with a small hook, overshoot their
 * starting point or have a kink in one side. This recognizer is used as a fallback for closed strokes: it cuts the
 * stroke where it closes, starts it at a corner, simplifies the outline and merges nearly collinear pieces. Only
 * strokes that stay close to the resulting triangle or quadrilateral are accepted, so circles and handwriting are
 * not affected.
 */
class ClosedPolygonRecognizer {
public:
    ClosedPolygonRecognizer() = delete;

    /// Returns the recognized triangle or quadrilateral (closed: last point == first point), or nullptr.
    static auto recognize(const Stroke* stroke) -> std::unique_ptr<Stroke>;

    /// Makes a recognized near-rectangle a proper rectangle and levels polygons that are nearly level
    static void straighten(Stroke* polygon);

    /// Whether the polygon follows the stroke closely and none of its corners is far away from it
    static auto fitsStroke(const Stroke* stroke, const Stroke* polygon) -> bool;
};
