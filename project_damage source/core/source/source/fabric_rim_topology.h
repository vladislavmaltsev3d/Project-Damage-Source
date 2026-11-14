// общие объявления: соединяем края разрыва с окружающей сеткой ткани
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fabric_surface {

inline double Cross(const manifold::vec2& a, const manifold::vec2& b) {
    return a.x * b.y - a.y * b.x;
}

template<class Triangles, class Mesh>
manifold::SimplePolygon ConformRim(const manifold::SimplePolygon& loop,
        const manifold::vec3& origin, const manifold::vec3& tangent,
        const manifold::vec3& bitangent, const Mesh& mesh, const Triangles& tris,
        const std::vector<char>& front, const std::vector<char>& back, double tolerance) {
    using V2 = manifold::vec2;
    manifold::SimplePolygon result;
    auto project = [&](Int32 vertex) {
        const manifold::vec3 d = manifold::vec3(mesh.pts[vertex]) - origin;
        return V2(manifold::la::dot(d, tangent), manifold::la::dot(d, bitangent));
    };
    for (size_t i = 0; i < loop.size(); ++i) {
        const V2 a = loop[i], d = loop[(i + 1) % loop.size()] - a;
        const double len = manifold::la::length(d);
        if (len <= tolerance) continue;
        std::vector<double> knots{0.0};
        const double eps = tolerance / len;
        for (size_t j = 0; j < tris.size(); ++j) {
            if (!front[j] && !back[j]) continue;
            for (int k = 0; k < 3; ++k) {
                const V2 b = project(tris[j].v[k]);
                const V2 e = project(tris[j].v[(k + 1) % 3]) - b;
                const double den = Cross(d, e);
                if (std::abs(den) <= tolerance * std::max(len, manifold::la::length(e))) {
                    if (std::abs(Cross(b - a, d)) > tolerance * len) continue;
                    for (const V2& p : {b, b + e}) {
                        const double t = manifold::la::dot(p - a, d) / (len * len);
                        if (t > eps && t < 1.0 - eps) knots.push_back(t);
                    }
                    continue;
                }
                const double t = Cross(b - a, e) / den;
                const double u = Cross(b - a, d) / den;
                if (t > eps && t < 1.0 - eps && u >= -eps && u <= 1.0 + eps)
                    knots.push_back(t);
            }
        }
        std::sort(knots.begin(), knots.end());
        double last = -1.0;
        for (double t : knots) {
            if (last >= 0.0 && t - last <= eps) continue;
            result.push_back(a + d * t);
            last = t;
        }
    }
    return result;
}

struct CellHash {
    size_t operator()(const std::array<int64_t, 3>& p) const {
        return size_t(uint64_t(p[0]) * 73856093ULL ^ uint64_t(p[1]) * 19349663ULL ^ uint64_t(p[2]) * 83492791ULL);
    }
};

class Weld {
    std::unordered_map<std::array<int64_t, 3>, std::vector<Int32>, CellHash> cells;
    std::vector<EVec>& points;
    double epsilon;
public:
    Weld(std::vector<EVec>& output, double tolerance) : points(output), epsilon(tolerance) {}
    Int32 Add(const manifold::vec3& p) {
        const std::array<int64_t, 3> cell = {
            int64_t(std::floor(p.x / epsilon)), int64_t(std::floor(p.y / epsilon)), int64_t(std::floor(p.z / epsilon))};
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    auto found = cells.find({cell[0] + x, cell[1] + y, cell[2] + z});
                    if (found == cells.end()) continue;
                    for (Int32 id : found->second)
                        if (manifold::la::length(manifold::vec3(points[size_t(id)]) - p) <= epsilon)
                            return id;
                }
        const Int32 id = Int32(points.size());
        points.push_back(EVec(p.x, p.y, p.z));
        cells[cell].push_back(id);
        return id;
    }
};

template<class Result>
void StitchRim(Result& out, manifold::MeshGL& mesh, double tolerance) {
    using V = manifold::vec3;
    auto key = [](Int32 a, Int32 b) {
        return (uint64_t(uint32_t(std::min(a, b))) << 32) | uint32_t(std::max(a, b));
    };
    std::unordered_map<uint64_t, int> uses;
    for (const auto& f : out.polys) {
        const Int32 v[4] = {f.a, f.b, f.c, f.d};
        const int n = f.c == f.d ? 3 : 4;
        for (int k = 0; k < n; ++k) ++uses[key(v[k], v[(k + 1) % n])];
    }
    std::vector<char> boundary(out.points.size(), false);
    for (const auto& e : uses) if (e.second == 1) {
        boundary[size_t(e.first >> 32)] = true;
        boundary[size_t(uint32_t(e.first))] = true;
    }
    std::array<std::vector<Int32>, 3> sorted;
    for (size_t i = 0; i < boundary.size(); ++i) if (boundary[i]) sorted[0].push_back(Int32(i));
    sorted[1] = sorted[2] = sorted[0];
    for (int k = 0; k < 3; ++k)
        std::sort(sorted[k].begin(), sorted[k].end(), [&](Int32 a, Int32 b) { return V(out.points[a])[k] < V(out.points[b])[k]; });
    std::unordered_map<uint64_t, std::vector<std::pair<double, Int32>>> splits;
    for (const auto& e : uses) {
        if (e.second != 1) continue;
        const Int32 a = Int32(e.first >> 32), b = Int32(uint32_t(e.first));
        const V origin(out.points[a]), d = V(out.points[b]) - origin;
        const double length2 = manifold::la::dot(d, d);
        if (length2 <= tolerance * tolerance) continue;
        int axis = 0;
        for (int k = 1; k < 3; ++k) if (std::abs(d[k]) > std::abs(d[axis])) axis = k;
        const double lo = std::min(origin[axis], origin[axis] + d[axis]) - tolerance;
        const double hi = std::max(origin[axis], origin[axis] + d[axis]) + tolerance;
        auto it = std::lower_bound(sorted[axis].begin(), sorted[axis].end(), lo,
            [&](Int32 id, double value) { return V(out.points[id])[axis] < value; });
        for (; it != sorted[axis].end() && V(out.points[*it])[axis] <= hi; ++it) {
            const Int32 id = *it;
            if (id == a || id == b) continue;
            const V offset = V(out.points[id]) - origin;
            const double t = manifold::la::dot(offset, d) / length2;
            const double eps = tolerance / std::sqrt(length2);
            if (t <= eps || t >= 1.0 - eps) continue;
            if (manifold::la::length(offset - d * t) <= tolerance) splits[e.first].push_back({t, id});
        }
    }
    if (splits.empty()) return;
    for (auto& e : splits) std::sort(e.second.begin(), e.second.end());
    auto originalPolys = std::move(out.polys);
    auto originalBase = std::move(out.basePolyOf);
    auto originalEffects = std::move(out.effectIds);
    auto originalCorners = std::move(out.propCorner);
    auto corner = [&](Int32 vertex, const std::array<float, 5>& properties) {
        const Int32 id = Int32(mesh.NumVert());
        const V p(out.points[vertex]);
        mesh.vertProperties.insert(mesh.vertProperties.end(), {(float)p.x, (float)p.y, (float)p.z, properties[3], properties[4]});
        return id;
    };
    for (size_t i = 0; i < originalPolys.size(); ++i) {
        const auto& f = originalPolys[i];
        const Int32 ids[4] = {f.a, f.b, f.c, f.d};
        const int n = f.c == f.d ? 3 : 4;
        std::vector<Int32> ring, corners;
        std::array<float, 5> centerProps{};
        V center(0.0);
        for (int k = 0; k < n; ++k) {
            const int next = (k + 1) % n;
            const Int32 a = ids[k], b = ids[next], ca = originalCorners[i * 4 + k], cb = originalCorners[i * 4 + next];
            ring.push_back(a); corners.push_back(ca);
            center += V(out.points[a]) / double(n);
            for (int p = 0; p < 5; ++p) centerProps[p] += mesh.vertProperties[size_t(ca) * 5 + p] / float(n);
            auto found = splits.find(key(a, b));
            if (found == splits.end()) continue;
            auto emit = [&](const std::pair<double, Int32>& split) {
                const double t = a < b ? split.first : 1.0 - split.first;
                std::array<float, 5> props;
                for (int p = 0; p < 5; ++p) props[p] = float(mesh.vertProperties[size_t(ca) * 5 + p] * (1.0 - t) + mesh.vertProperties[size_t(cb) * 5 + p] * t);
                ring.push_back(split.second); corners.push_back(corner(split.second, props));
            };
            if (a < b) for (const auto& split : found->second) emit(split);
            else for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) emit(*it);
        }
        if (ring.size() == size_t(n)) {
            out.polys.push_back(f); out.basePolyOf.push_back(originalBase[i]); out.effectIds.push_back(originalEffects[i]);
            out.propCorner.insert(out.propCorner.end(), originalCorners.begin() + i * 4, originalCorners.begin() + (i + 1) * 4);
            continue;
        }
        const Int32 middle = Int32(out.points.size());
        out.points.push_back(EVec(center.x, center.y, center.z));
        const Int32 middleCorner = corner(middle, centerProps);
        for (size_t k = 0; k < ring.size(); ++k) {
            const size_t next = (k + 1) % ring.size();
            if (ring[k] == ring[next]) continue;
            out.polys.push_back({ring[k], ring[next], middle, middle});
            out.basePolyOf.push_back(originalBase[i]); out.effectIds.push_back(originalEffects[i]);
            out.propCorner.insert(out.propCorner.end(), {corners[k], corners[next], middleCorner, NOTOK});
        }
    }
}

}
