// общие объявления: превращаем нарисованные контуры в области влияния
// ProjectDamage grease-pencil paint regions - GPL-3.0-or-later; see LICENSE.
//
// Shared by Coating Peeling and Rust. The painted mask is a set of closed
// Grease Pencil contours, exactly the input the Paint Damage Surface mode
// already reads (CoreInput::surfacePaint, built by paint.py: source-local cm,
// one outward region normal per contour). Each contour becomes a planar
// frame; a surface point belongs to a contour when
//   * its distance to the contour plane is <= reach (Projection Distance),
//   * its surface normal agrees with the contour normal (dot >= minAgree), so
//     a stroke on the front never paints the back of a thin part, and
//   * its projection lies inside the 2D polygon (even/odd rule).
// Signed() returns the 2D distance to the nearest contour edge, + inside and
// - outside, over every contour that passes the first two tests (max = union).
#pragma once
#include "project_damage_core.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN

class PaintRegions {
public:
    struct Frame {
        m::vec3 o, n, t, b;
        std::vector<m::vec2> xy;
        m::vec3 lo, hi;                                               
        double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
        double inradius = 0.0;                                        
    };

                                                                               
                                                             
    void Build(const std::vector<SurfacePaintRegion>* regions, double reach, double minAgree = 0.5)
    {
        frames_.clear();
        reach_ = std::max(1e-3, reach);
        minAgree_ = minAgree;
        if (regions == nullptr) return;
        for (const SurfacePaintRegion& r : *regions) {
            std::vector<m::vec3> pts;
            for (const EVec& q : r.points)
                if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)
                    && (pts.empty() || m::la::length(q - pts.back()) > 1e-9))
                    pts.push_back(q);
            if (pts.size() > 3 && m::la::length(pts.front() - pts.back()) <= 1e-9) pts.pop_back();
            if (pts.size() < 3) continue;
            Frame f;
            const double nl = m::la::length(r.normal);
            f.n = nl > 1e-12 ? m::vec3(r.normal) / nl : m::vec3(0, 0, 1);
            f.o = m::vec3(0.0);
            for (const auto& q : pts) f.o += q;
            f.o /= double(pts.size());
            const m::vec3 axis = std::abs(f.n.z) < 0.9 ? m::vec3(0, 0, 1) : m::vec3(0, 1, 0);
            f.t = m::la::normalize(m::la::cross(axis, f.n));
            f.b = m::la::cross(f.n, f.t);
            f.lo = f.hi = pts[0];
            for (const auto& q : pts) {
                const m::vec3 d = q - f.o;
                const double x = m::la::dot(d, f.t), y = m::la::dot(d, f.b);
                f.xy.push_back(m::vec2(x, y));
                f.xmin = std::min(f.xmin, x); f.xmax = std::max(f.xmax, x);
                f.ymin = std::min(f.ymin, y); f.ymax = std::max(f.ymax, y);
                f.lo = m::la::min(f.lo, q); f.hi = m::la::max(f.hi, q);
            }
            if ((f.xmax - f.xmin) * (f.ymax - f.ymin) <= 1e-9) continue;
            constexpr int G = 48;
            for (int i = 0; i < G; ++i)
                for (int j = 0; j < G; ++j) {
                    const double x = f.xmin + (f.xmax - f.xmin) * (i + 0.5) / G;
                    const double y = f.ymin + (f.ymax - f.ymin) * (j + 0.5) / G;
                    f.inradius = std::max(f.inradius, Clearance(f, x, y));
                }
            if (f.inradius <= 0.0) continue;
            frames_.push_back(std::move(f));
        }
    }

    bool Empty() const { return frames_.empty(); }
    const std::vector<Frame>& Frames() const { return frames_; }
    double Reach() const { return reach_; }

                                                                         
                                                                            
                                
                                                                           
                                  
    double Signed(const m::vec3& p, const m::vec3& n, double cap, double* frac = nullptr) const
    {
        double best = -cap, bestFrac = -1.0;
        for (const Frame& f : frames_) {
            const m::vec3 d = p - f.o;
            if (std::abs(m::la::dot(d, f.n)) > reach_) continue;
            if (m::la::dot(n, f.n) < minAgree_) continue;
            const double x = m::la::dot(d, f.t), y = m::la::dot(d, f.b);
            if (x < f.xmin - cap || x > f.xmax + cap || y < f.ymin - cap || y > f.ymax + cap) continue;
            const double s = std::max(-cap, Signed2D(f, x, y));
            if (s > best) { best = s; bestFrac = s / f.inradius; }
        }
        if (frac) *frac = bestFrac;
        return best;
    }

    bool Inside(const m::vec3& p, const m::vec3& n) const { return Signed(p, n, 1e-6) > 0.0; }

                                                                          
    bool Near(const m::vec3& lo, const m::vec3& hi, double margin) const
    {
        for (const Frame& f : frames_) {
                                                                              
            const m::vec3 g(margin + reach_ * std::abs(f.n.x), margin + reach_ * std::abs(f.n.y),
                            margin + reach_ * std::abs(f.n.z));
            if (lo.x > f.hi.x + g.x || hi.x < f.lo.x - g.x || lo.y > f.hi.y + g.y || hi.y < f.lo.y - g.y
                || lo.z > f.hi.z + g.z || hi.z < f.lo.z - g.z) continue;
            return true;
        }
        return false;
    }

private:
    static double Signed2D(const Frame& f, double x, double y)
    {
        bool inside = false;
        double nearest = 1e300;
        for (size_t i = 0, j = f.xy.size() - 1; i < f.xy.size(); j = i++) {
            const m::vec2 a = f.xy[j], b = f.xy[i];
            if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
            const m::vec2 v(x - a.x, y - a.y), ab = b - a;
            const double den = m::la::dot(ab, ab);
            const double t = den > 0.0 ? std::max(0.0, std::min(1.0, m::la::dot(v, ab) / den)) : 0.0;
            nearest = std::min(nearest, m::la::length(v - ab * t));
        }
        return inside ? nearest : -nearest;
    }
    static double Clearance(const Frame& f, double x, double y) { return std::max(0.0, Signed2D(f, x, y)); }

    std::vector<Frame> frames_;
    double reach_ = 5.0, minAgree_ = 0.5;
};

PROJECT_DAMAGE_NS_MACRO_END
