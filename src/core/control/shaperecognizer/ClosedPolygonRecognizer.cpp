#include "ClosedPolygonRecognizer.h"

#include <algorithm>  // for max, min, max_element
#include <cmath>      // for hypot, atan2, cos, sin, fabs, M_PI
#include <limits>     // for numeric_limits
#include <utility>    // for pair
#include <vector>     // for vector

#include "model/Point.h"   // for Point
#include "model/Stroke.h"  // for Stroke

#include "ShapeRecognizerConfig.h"  // for RDEBUG, SLANT_TOLERANCE

namespace {
// All lengths are relative to the diagonal of the stroke's bounding box.
constexpr double CLOSING_TOLERANCE = 0.2;        ///< max gap between the stroke's start and end
constexpr double SIMPLIFY_TOLERANCE = 0.07;      ///< outline simplification (Ramer-Douglas-Peucker)
constexpr double MIN_EDGE_LENGTH = 0.12;         ///< shorter edges are merged into their neighbours
constexpr double MEAN_FIT_TOLERANCE = 0.045;     ///< mean distance from the stroke to the polygon
constexpr double MAX_FIT_TOLERANCE = 0.14;       ///< max distance from the stroke to the polygon
constexpr double MIN_AREA = 0.03;                ///< relative to diagonal^2: rejects thin back-and-forth strokes
constexpr double MAX_BULGE = 0.10;               ///< max average outward bulge of the sides, relative to their length
constexpr double MIN_CORNER_SHARPNESS = 0.7;     ///< see cornerSharpness(): ellipses score below, polygons above
constexpr double MERGE_ANGLE = 30 * M_PI / 180;  ///< corners flatter than this are removed
constexpr double RIGHT_ANGLE_TOLERANCE = 20 * M_PI / 180;  ///< quadrilaterals this close to a rectangle become one
constexpr double CORNER_MARGIN = 0.15;                     ///< corners of a recognized polygon may lie this far outside
constexpr double STRICT_MEAN_FIT_TOLERANCE = 0.06;         ///< mean distance from the stroke to a recognized polygon
constexpr size_t MAX_POINTS = 1500;                        ///< longer strokes are subsampled

struct P {
    double x, y;
};

auto dist(const P& a, const P& b) -> double { return std::hypot(a.x - b.x, a.y - b.y); }

auto distToSegment(const P& p, const P& a, const P& b) -> double {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 0) {
        return dist(p, a);
    }
    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0, 1.0);
    return dist(p, {a.x + t * dx, a.y + t * dy});
}

void simplify(const std::vector<P>& pts, size_t first, size_t last, double eps, std::vector<size_t>& keep) {
    double maxD = -1;
    size_t idx = first;
    for (size_t i = first + 1; i < last; i++) {
        const double d = distToSegment(pts[i], pts[first], pts[last]);
        if (d > maxD) {
            maxD = d;
            idx = i;
        }
    }
    if (maxD > eps) {
        simplify(pts, first, idx, eps, keep);
        keep.push_back(idx);
        simplify(pts, idx, last, eps, keep);
    }
}

/// Angle between the directions a->b and b->c, in [0, pi]
auto turnAngle(const P& a, const P& b, const P& c) -> double {
    const double a1 = std::atan2(b.y - a.y, b.x - a.x);
    const double a2 = std::atan2(c.y - b.y, c.x - b.x);
    return std::fabs(std::remainder(a2 - a1, 2 * M_PI));
}

/// Whether the segments ab and cd cross each other
auto segmentsCross(const P& a, const P& b, const P& c, const P& d) -> bool {
    auto side = [](const P& o, const P& p, const P& q) {
        return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x);
    };
    return side(a, b, c) * side(a, b, d) < 0 && side(c, d, a) * side(c, d, b) < 0;
}

/// Whether a quadrilateral crosses itself (a "bow tie"); triangles never do
auto selfIntersecting(const std::vector<P>& v) -> bool {
    return v.size() == 4 && (segmentsCross(v[0], v[1], v[2], v[3]) || segmentsCross(v[1], v[2], v[3], v[0]));
}

auto polygonArea(const std::vector<P>& v) -> double {
    double area = 0;
    for (size_t i = 0; i < v.size(); i++) {
        const P& a = v[i];
        const P& b = v[(i + 1) % v.size()];
        area += a.x * b.y - b.x * a.y;
    }
    return std::fabs(area) / 2;
}

/**
 * How sharp the polygon's corners are in the stroke: the direction change of the stroke close to each corner,
 * relative to the polygon's own corner angles. About 1 for a hand-drawn polygon (the stroke turns at the corners),
 * clearly less for an ellipse traced with a polygon (the stroke turns gradually everywhere).
 */
auto cornerSharpness(const std::vector<P>& loop, const std::vector<P>& v) -> double {
    constexpr long N = 240;      // resampled points along the stroke
    constexpr long HEADING = 5;  // direction is measured over +-2% of the perimeter, smoothing out jitter
    constexpr long CORNER = 12;  // turning counted within +-5% of the perimeter around each corner
    double perimeter = 0;
    for (size_t i = 0; i + 1 < loop.size(); i++) {
        perimeter += dist(loop[i], loop[i + 1]);
    }
    if (perimeter <= 0 || loop.size() < 2) {
        return 0;
    }
    std::vector<P> rs;  // the stroke resampled at equal distances
    double done = 0;
    size_t j = 0;
    for (long k = 0; k < N; k++) {
        const double target = perimeter * static_cast<double>(k) / N;
        while (j + 2 < loop.size() && done + dist(loop[j], loop[j + 1]) < target) {
            done += dist(loop[j], loop[j + 1]);
            j++;
        }
        const double seg = dist(loop[j], loop[j + 1]);
        const double t = seg > 0 ? std::clamp((target - done) / seg, 0.0, 1.0) : 0.0;
        rs.push_back({loop[j].x + t * (loop[j + 1].x - loop[j].x), loop[j].y + t * (loop[j + 1].y - loop[j].y)});
    }
    auto at = [&rs](long i) -> const P& { return rs[static_cast<size_t>(((i % N) + N) % N)]; };
    auto heading = [&at](long i) {
        const P& a = at(i - HEADING);
        const P& b = at(i + HEADING);
        return std::atan2(b.y - a.y, b.x - a.x);
    };
    double strokeTurn = 0, polygonTurn = 0;
    for (size_t i = 0; i < v.size(); i++) {
        long nearest = 0;
        double nearestDist = std::numeric_limits<double>::max();
        for (long k = 0; k < N; k++) {
            if (const double d = dist(rs[static_cast<size_t>(k)], v[i]); d < nearestDist) {
                nearestDist = d, nearest = k;
            }
        }
        strokeTurn += std::fabs(std::remainder(heading(nearest + CORNER) - heading(nearest - CORNER), 2 * M_PI));
        polygonTurn += turnAngle(v[(i + v.size() - 1) % v.size()], v[i], v[(i + 1) % v.size()]);
    }
    return polygonTurn > 0 ? strokeTurn / polygonTurn : 0;
}

/// Makes near-rectangles proper rectangles, and rotates shapes that are nearly level (within SLANT_TOLERANCE of
/// horizontal/vertical) so that they are exactly level.
void regularize(std::vector<P>& v) {
    P c{0, 0};
    for (const P& p: v) {
        c.x += p.x, c.y += p.y;
    }
    c.x /= static_cast<double>(v.size()), c.y /= static_cast<double>(v.size());

    bool rectangular = v.size() == 4;
    for (size_t i = 0; rectangular && i < 4; i++) {
        const double turn = turnAngle(v[(i + 3) % 4], v[i], v[(i + 1) % 4]);
        rectangular = std::fabs(turn - M_PI / 2) <= RIGHT_ANGLE_TOLERANCE;
    }
    if (rectangular) {
        // average edge direction modulo 90 degrees (circular mean of 4 * angle)
        double sx = 0, sy = 0;
        for (size_t i = 0; i < 4; i++) {
            const P& a = v[i];
            const P& b = v[(i + 1) % 4];
            const double angle = std::atan2(b.y - a.y, b.x - a.x);
            const double len = dist(a, b);
            sx += len * std::cos(4 * angle), sy += len * std::sin(4 * angle);
        }
        double theta = std::atan2(sy, sx) / 4;  // in [-45, 45] degrees
        if (std::fabs(theta) < SLANT_TOLERANCE) {
            theta = 0;
        }
        const double ct = std::cos(theta), st = std::sin(theta);
        // In the rectangle's own coordinates each side lies between two corners: put it at their average
        std::vector<double> us, ws;
        for (const P& p: v) {
            us.push_back((p.x - c.x) * ct + (p.y - c.y) * st);
            ws.push_back(-(p.x - c.x) * st + (p.y - c.y) * ct);
        }
        std::sort(us.begin(), us.end());
        std::sort(ws.begin(), ws.end());
        const double lo1 = (us[0] + us[1]) / 2, hi1 = (us[2] + us[3]) / 2;
        const double lo2 = (ws[0] + ws[1]) / 2, hi2 = (ws[2] + ws[3]) / 2;
        auto back = [&](double u, double w) -> P { return {c.x + u * ct - w * st, c.y + u * st + w * ct}; };
        v = {back(lo1, lo2), back(hi1, lo2), back(hi1, hi2), back(lo1, hi2)};
        return;
    }

    // Other shapes: make the side closest to horizontal/vertical exactly level, if it is nearly so
    double best = M_PI;
    for (size_t i = 0; i < v.size(); i++) {
        const P& a = v[i];
        const P& b = v[(i + 1) % v.size()];
        const double deviation = std::remainder(std::atan2(b.y - a.y, b.x - a.x), M_PI / 2);
        if (std::fabs(deviation) < std::fabs(best)) {
            best = deviation;
        }
    }
    if (std::fabs(best) < SLANT_TOLERANCE && best != 0) {
        const double ct = std::cos(-best), st = std::sin(-best);
        for (P& p: v) {
            const double dx = p.x - c.x, dy = p.y - c.y;
            p = {c.x + dx * ct - dy * st, c.y + dx * st + dy * ct};
        }
    }
}
}  // namespace

void ClosedPolygonRecognizer::straighten(Stroke* polygon) {
    const auto& pts = polygon->getPointVector();
    if (pts.size() < 4 || pts.front().x != pts.back().x || pts.front().y != pts.back().y) {
        return;  // only closed polygons
    }
    std::vector<P> v;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        v.push_back({pts[i].x, pts[i].y});
    }
    regularize(v);
    std::vector<Point> out;
    for (const P& p: v) {
        out.emplace_back(p.x, p.y);
    }
    out.emplace_back(v[0].x, v[0].y);
    polygon->setPointVector(std::move(out));
}

auto ClosedPolygonRecognizer::fitsStroke(const Stroke* stroke, const Stroke* polygon) -> bool {
    const auto& sp = stroke->getPointVector();
    const auto& pp = polygon->getPointVector();
    if (sp.empty() || pp.size() < 2) {
        return false;
    }
    double minX = sp[0].x, maxX = sp[0].x, minY = sp[0].y, maxY = sp[0].y;
    for (const auto& p: sp) {
        minX = std::min(minX, p.x), maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y), maxY = std::max(maxY, p.y);
    }
    const double diag = std::hypot(maxX - minX, maxY - minY);
    if (diag <= 0) {
        return false;
    }
    // no corner far outside the stroke's bounding box
    const double margin = CORNER_MARGIN * diag;
    for (const auto& p: pp) {
        if (p.x < minX - margin || p.x > maxX + margin || p.y < minY - margin || p.y > maxY + margin) {
            RDEBUG("polygon corner (%.0f, %.0f) far from the stroke", p.x, p.y);
            return false;
        }
    }
    // the stroke follows the polygon's outline
    const size_t step = std::max<size_t>(1, sp.size() / MAX_POINTS);
    double sum = 0;
    size_t count = 0;
    for (size_t k = 0; k < sp.size(); k += step) {
        const P q{sp[k].x, sp[k].y};
        double d = std::numeric_limits<double>::max();
        for (size_t i = 0; i + 1 < pp.size(); i++) {
            d = std::min(d, distToSegment(q, {pp[i].x, pp[i].y}, {pp[i + 1].x, pp[i + 1].y}));
        }
        sum += d;
        count++;
    }
    const double mean = sum / static_cast<double>(count) / diag;
    if (mean > STRICT_MEAN_FIT_TOLERANCE) {
        RDEBUG("polygon does not follow the stroke (mean %.3f)", mean);
        return false;
    }
    return true;
}

auto ClosedPolygonRecognizer::recognize(const Stroke* stroke) -> std::unique_ptr<Stroke> {
    const auto& src = stroke->getPointVector();
    if (src.size() < 10) {
        return nullptr;
    }
    const size_t step = std::max<size_t>(1, src.size() / MAX_POINTS);
    std::vector<P> pts;
    for (size_t i = 0; i < src.size(); i += step) {
        pts.push_back({src[i].x, src[i].y});
    }
    if (pts.back().x != src.back().x || pts.back().y != src.back().y) {
        pts.push_back({src.back().x, src.back().y});
    }
    const size_t n = pts.size();

    double minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
    for (const P& p: pts) {
        minX = std::min(minX, p.x), maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y), maxY = std::max(maxY, p.y);
    }
    const double diag = std::hypot(maxX - minX, maxY - minY);
    if (diag <= 0) {
        return nullptr;
    }

    // 1. Where does the stroke close? Cut off any lead-in hook or overshoot past that point.
    size_t closeA = 0, closeB = n - 1;
    double closeD = std::numeric_limits<double>::max();
    for (size_t i = 0; i <= n / 4; i++) {
        for (size_t j = n - 1 - n / 4; j < n; j++) {
            if (j <= i) {
                continue;
            }
            const double d = dist(pts[i], pts[j]);
            if (d < closeD) {
                closeD = d, closeA = i, closeB = j;
            }
        }
    }
    if (closeD > CLOSING_TOLERANCE * diag) {
        RDEBUG("closed polygon: not closed");
        return nullptr;
    }
    std::vector<P> loop(pts.begin() + static_cast<std::ptrdiff_t>(closeA),
                        pts.begin() + static_cast<std::ptrdiff_t>(closeB) + 1);

    // 2. Start at a corner: the point farthest from the centre
    P c{0, 0};
    for (const P& p: loop) {
        c.x += p.x, c.y += p.y;
    }
    c.x /= static_cast<double>(loop.size()), c.y /= static_cast<double>(loop.size());
    const auto far = std::max_element(loop.begin(), loop.end(),
                                      [&c](const P& a, const P& b) { return dist(a, c) < dist(b, c); });
    std::vector<P> ring(far, loop.end());
    ring.insert(ring.end(), loop.begin(), far);
    ring.push_back(ring.front());

    // 3. Simplify the outline and merge nearly straight or tiny pieces
    std::vector<size_t> keep{0};
    simplify(ring, 0, ring.size() - 1, SIMPLIFY_TOLERANCE * diag, keep);
    std::vector<P> v;
    for (size_t i: keep) {
        v.push_back(ring[i]);
    }
    bool changed = true;
    while (changed && v.size() > 3) {
        changed = false;
        for (size_t i = 0; i < v.size() && v.size() > 3; i++) {
            const P& prev = v[(i + v.size() - 1) % v.size()];
            const P& next = v[(i + 1) % v.size()];
            if (turnAngle(prev, v[i], next) < MERGE_ANGLE || dist(prev, v[i]) < MIN_EDGE_LENGTH * diag) {
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                break;
            }
        }
    }
    // How far the stroke strays from a polygon: mean and max distance, relative to the diagonal
    auto fit = [&loop, diag](const std::vector<P>& poly) -> std::pair<double, double> {
        double sum = 0, worst = 0;
        for (const P& p: loop) {
            double d = std::numeric_limits<double>::max();
            for (size_t i = 0; i < poly.size(); i++) {
                d = std::min(d, distToSegment(p, poly[i], poly[(i + 1) % poly.size()]));
            }
            sum += d;
            worst = std::max(worst, d);
        }
        return {sum / static_cast<double>(loop.size()) / diag, worst / diag};
    };
    auto fits = [](std::pair<double, double> f) {
        return f.first <= MEAN_FIT_TOLERANCE && f.second <= MAX_FIT_TOLERANCE;
    };

    // Prefer the simplest polygon that still fits: a wobbly triangle side must not become a fourth corner
    while (v.size() > 3) {
        size_t best = 0;
        double bestMean = std::numeric_limits<double>::max();
        std::pair<double, double> bestFit;
        for (size_t i = 0; i < v.size(); i++) {
            std::vector<P> fewer = v;
            fewer.erase(fewer.begin() + static_cast<std::ptrdiff_t>(i));
            const auto f = fit(fewer);
            if (f.first < bestMean) {
                bestMean = f.first, best = i, bestFit = f;
            }
        }
        if (!fits(bestFit)) {
            break;
        }
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(best));
    }

    if (v.size() != 3 && v.size() != 4) {
        RDEBUG("closed polygon: %zu corners", v.size());
        return nullptr;
    }
    if (selfIntersecting(v)) {
        RDEBUG("closed polygon: crosses itself");
        return nullptr;
    }
    if (polygonArea(v) < MIN_AREA * diag * diag) {
        RDEBUG("closed polygon: too thin");
        return nullptr;
    }

    // 4. The stroke must actually follow the polygon
    if (const auto f = fit(v); !fits(f)) {
        RDEBUG("closed polygon: poor fit (mean %.3f, max %.3f)", f.first, f.second);
        return nullptr;
    }

    // 5. ...with straight sides and real corners. A hand-drawn straight side wobbles to both sides, while a circle or
    // ellipse traced with a polygon bulges outwards along every side and has no sharp corners.
    {
        P pc{0, 0};
        for (const P& p: v) {
            pc.x += p.x, pc.y += p.y;
        }
        pc.x /= static_cast<double>(v.size()), pc.y /= static_cast<double>(v.size());
        std::vector<double> bulge(v.size(), 0.0);
        std::vector<size_t> count(v.size(), 0);
        for (const P& p: loop) {
            size_t edge = 0;
            double d = std::numeric_limits<double>::max();
            for (size_t i = 0; i < v.size(); i++) {
                const double di = distToSegment(p, v[i], v[(i + 1) % v.size()]);
                if (di < d) {
                    d = di, edge = i;
                }
            }
            const P& a = v[edge];
            const P& b = v[(edge + 1) % v.size()];
            const double len = dist(a, b);
            if (len <= 0) {
                continue;
            }
            double nx = (b.y - a.y) / len, ny = -(b.x - a.x) / len;  // a normal of the side...
            if (nx * (a.x - pc.x) + ny * (a.y - pc.y) < 0) {         // ...pointing away from the centre
                nx = -nx, ny = -ny;
            }
            bulge[edge] += (p.x - a.x) * nx + (p.y - a.y) * ny;
            count[edge]++;
        }
        double total = 0;
        for (size_t i = 0; i < v.size(); i++) {
            const double len = dist(v[i], v[(i + 1) % v.size()]);
            if (count[i] > 0 && len > 0) {
                total += bulge[i] / static_cast<double>(count[i]) / len;
            }
        }
        const double average = total / static_cast<double>(v.size());
        const double sharpness = cornerSharpness(loop, v);
        if (average > MAX_BULGE || sharpness < MIN_CORNER_SHARPNESS) {
            RDEBUG("closed polygon: round sides (bulge %.3f, corner sharpness %.2f)", average, sharpness);
            return nullptr;
        }
    }

    // 6. Tidy up: proper rectangles, straightened when nearly level
    regularize(v);

    auto s = std::make_unique<Stroke>();
    s->applyStyleFrom(stroke);
    for (const P& p: v) {
        s->addPoint(Point(p.x, p.y));
    }
    s->addPoint(Point(v[0].x, v[0].y));
    RDEBUG("closed polygon: %zu corners", v.size());
    return s;
}
