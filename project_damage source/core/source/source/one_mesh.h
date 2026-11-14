// общие объявления: собираем части результата в один меш с материалами и UV
// ProjectDamage single-mesh guard - GPL-3.0-or-later; see LICENSE.
//
// Shared by Coating Peeling and Rust. Their damage goes into the source
// through the main Boolean (cutters subtracted, buildup added), and the
// result must stay ONE mesh: no loose specks, no hollow shells inside.
// KeepOneMesh() checks a module's own cutters and buildup against the source
// and fixes the three ways a loose part can appear:
//   * crumbs: a speck of the source cut free by several cutters meeting, or
//     a flat zero-volume sliver the Boolean leaves where faces coincide, is
//     cut away too (its padded hull joins the cutter);
//   * floating buildup: a piece of buildup that does not overlap what is
//     left of the source is dropped;
//   * pockets: a meshed surface that wavers around an exact one at mesh
//     resolution can close off a speck of air; each pocket is filled (its
//     padded hull joins the buildup).
// The cutters come back as one cutter.
#pragma once
#include "project_damage_core.h"
#include <algorithm>
#include <cmath>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN

                                                                
inline m::Manifold PadHull(const m::Manifold& piece, double pad)
{
    const m::MeshGL g = piece.GetMeshGL();
    std::vector<m::vec3> pts;
    pts.reserve(g.NumVert() * 6);
    for (size_t k = 0; k < g.NumVert(); ++k) {
        const m::vec3 p(g.vertProperties[k * g.numProp], g.vertProperties[k * g.numProp + 1],
                        g.vertProperties[k * g.numProp + 2]);
        for (int a = 0; a < 3; ++a)
            for (double sg : {-1.0, 1.0}) {
                m::vec3 q = p;
                q[a] += sg * pad;
                pts.push_back(q);
            }
    }
    return m::Manifold::Hull(pts);
}

                                                                             
                                                                            
                                                                            
                                                                             
                                                                            
                                                                       
inline void KeepOneMesh(const CoreInput& in, std::vector<m::Manifold>& cutters,
                        std::vector<m::Manifold>& buildup, double pad,
                        const m::Manifold* clip = nullptr, const m::Manifold* substrate = nullptr)
{
    m::Manifold cut;
    if (!cutters.empty()) {
        cut = m::Manifold::BatchBoolean(cutters, m::OpType::Add);
        if (clip) cut = cut ^ *clip;
    }
    cutters.clear();
    if (!in.box) {
        if (cut.NumTri() > 0) cutters.push_back(std::move(cut));
        return;
    }
    const m::Manifold& box = *in.box;
    m::Manifold remain = cut.NumTri() > 0 ? box - cut : box;

    if (cut.NumTri() > 0) {
        double smallest = 1e300;
        for (const auto& s : box.Decompose()) if (s.Volume() > 0.0) smallest = std::min(smallest, s.Volume());
        const std::vector<m::Manifold> parts = remain.Decompose();
        std::vector<m::Manifold> crumbs;
        if (parts.size() > 1) {
            double largest = 0.0;
            for (const auto& q : parts) largest = std::max(largest, q.Volume());
            const double sliver = pad * pad * pad;
            for (const auto& q : parts) {
                const double vol = q.Volume();
                                                                                       
                const bool flat = std::abs(vol) <= sliver;
                if (!flat && (vol <= 0.0 || vol >= largest || vol > 0.01 * smallest)) continue;
                if (!flat && substrate && (q ^ *substrate).Volume() > 1e-9 * smallest) continue;
                crumbs.push_back(PadHull(q, pad));
            }
        }
        if (!crumbs.empty()) {
            crumbs.push_back(cut);
            cut = m::Manifold::BatchBoolean(crumbs, m::OpType::Add);
            if (clip) cut = cut ^ *clip;
            remain = box - cut;
        }
    }

    std::vector<m::Manifold> kept;
    const double tiny = pad * pad * pad;
    for (auto& b : buildup) {
        if (b.NumTri() == 0) continue;
        for (auto& piece : b.Decompose())
            if (piece.Volume() > 0.0 && (piece ^ remain).Volume() > tiny) kept.push_back(std::move(piece));
    }
    if (!kept.empty()) {
        std::vector<m::Manifold> all = kept;
        all.push_back(remain);
        for (const auto& shell : m::Manifold::BatchBoolean(all, m::OpType::Add).Decompose())
            if (shell.Volume() < 0.0) kept.push_back(PadHull(shell, pad));
    }
    buildup = std::move(kept);
    if (cut.NumTri() > 0) cutters.push_back(std::move(cut));
}

PROJECT_DAMAGE_NS_MACRO_END
