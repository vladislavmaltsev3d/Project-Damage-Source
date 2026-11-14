// общие объявления: заполняем объём регулярной конструкцией и обрезаем её по исходной форме
#pragma once

struct VolumeFillCell
{
    int64_t i = 0, j = 0, k = 0;
    EVec lo, hi;
    uint64_t key = 0;
};

struct VolumeFillPiece
{
    m::Manifold solid;
    m::Box box;
    size_t cell = 0;
    bool held = false;
};

bool VolumeBoxesOverlap(const m::Box& a, const m::Box& b, double tolerance = 0.0)
{
    return a.min.x <= b.max.x + tolerance && a.max.x + tolerance >= b.min.x
        && a.min.y <= b.max.y + tolerance && a.max.y + tolerance >= b.min.y
        && a.min.z <= b.max.z + tolerance && a.max.z + tolerance >= b.min.z;
}

m::Manifold VolumeInwardReference(const m::Manifold& original, double inset)
{
    const auto mesh = original.GetMeshGL64();
    const auto bounds = original.BoundingBox();
    const double epsilon = std::max(1e-9, m::la::length(bounds.max - bounds.min) * 1e-10);
    auto keyOf = [&](const EVec& point)
    {
        const EVec local = (point - bounds.min) / epsilon;
        return Key(0x564F4C4E, (uint64_t)(int64_t)std::llround(local.x),
            (uint64_t)(int64_t)std::llround(local.y), (uint64_t)(int64_t)std::llround(local.z));
    };
    std::vector<EVec> points;
    points.reserve(mesh.NumVert());
    for (size_t v = 0; v < mesh.NumVert(); ++v)
        points.emplace_back(mesh.vertProperties[v * mesh.numProp], mesh.vertProperties[v * mesh.numProp + 1], mesh.vertProperties[v * mesh.numProp + 2]);
    std::unordered_map<uint64_t, EVec> normals;
    for (size_t f = 0; f < mesh.triVerts.size(); f += 3)
    {
        const EVec a = points[mesh.triVerts[f]], b = points[mesh.triVerts[f + 1]], c = points[mesh.triVerts[f + 2]];
        const EVec normal = m::la::cross(b - a, c - a);
        for (const EVec& point : {a, b, c})
        {
            auto found = normals.emplace(keyOf(point), EVec(0.0));
            found.first->second += normal;
        }
    }
    m::Manifold inner = original.Warp([&](m::vec3& point)
    {
        const auto found = normals.find(keyOf(point));
        if (found != normals.end())
        {
            const double length = m::la::length(found->second);
            if (length > 1e-12) point -= found->second * (inset / length);
        }
    });
    if (inner.Status() != m::Manifold::Error::NoError || inner.IsEmpty())
        throw std::runtime_error("Stable Volume Fill: the prepared source is too thin or invalid for a safe inner offset. Check Mesh Preparation thickness.");
    inner = inner ^ original;
    if (inner.IsEmpty())
        throw std::runtime_error("Stable Volume Fill: no interior volume remains after surface clearance. Increase Mesh Preparation thickness.");
    return inner;
}

m::Manifold VolumeCellSolid(const VolumeFillCell& cell, double chamfer)
{
    const EVec size = cell.hi - cell.lo;
    const double cut = std::clamp(chamfer, 0.0, 0.4) * std::min(size.x, std::min(size.y, size.z));
    if (cut < 1e-7) return m::Manifold::Cube(size).Translate(cell.lo);
    std::vector<m::vec3> points;
    const EVec half = size * 0.5, centre = (cell.lo + cell.hi) * 0.5;
    for (int x : {-1, 1}) for (int y : {-1, 1}) for (int z : {-1, 1})
        for (int axis = 0; axis < 3; ++axis)
            points.emplace_back(centre + EVec(x * (half.x - (axis == 0 ? 0.0 : cut)),
                y * (half.y - (axis == 1 ? 0.0 : cut)), z * (half.z - (axis == 2 ? 0.0 : cut))));
    return m::Manifold::Hull(points);
}

void BuildStableVolumeFill(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
    const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, const m::Manifold* left)
{
    out = BreakRebarMesh();
    if (!p.enable || p.material != BREAK_FILL_BRICKS || source.IsEmpty() || p.missing >= 1.0 || (cutters.empty() && !p.showAll)) return;
    for (double value : {p.scale, p.angle, p.volumeClearance, p.volumeMinLength, p.volumeMinSection,
        p.volumeOriginX, p.volumeOriginY, p.volumeOriginZ, p.plankWidth, p.plankThickness, p.plankGap,
        p.brickLength, p.brickHeight, p.brickDepth, p.mortar, p.embed, p.splinter, p.trim,
        p.broken, p.brickBroken, p.chamfer, p.mortarRecess, p.missing})
        if (!std::isfinite(value)) throw std::invalid_argument("Stable Volume Fill settings must be finite");
    const bool brick = true;
    const double scale = std::clamp(p.scale, 0.01, 100.0);
    const double clearance = std::max(0.0, p.volumeClearance * scale);
    const m::Box sourceBox = source.BoundingBox();
    const EVec dimensions = sourceBox.max - sourceBox.min;
    int axis = p.volumeAxis - 1;
    if (axis < 0 || axis > 2)
    {
        axis = 0;
        if (dimensions.y < dimensions[axis]) axis = 1;
        if (dimensions.z < dimensions[axis]) axis = 2;
    }
    EVec normal(0.0); normal[axis] = 1.0;
    const EVec objectUp(0.0, 0.0, 1.0);
    EVec vertical = objectUp - normal * m::la::dot(objectUp, normal);
    vertical = Normalized(vertical, AnyPerp(normal));
    EVec horizontal = Normalized(m::la::cross(vertical, normal), AnyPerp(normal));
    const double angle = p.angle * kPi / 180.0;
    horizontal = Rotate(horizontal, normal, angle);
    vertical = Normalized(m::la::cross(normal, horizontal), vertical);
    if (!brick && p.plankDir == 1)
    {
        const EVec previous = horizontal;
        horizontal = vertical;
        vertical = -previous;
    }
    const EVec origin(p.volumeOriginX * scale, p.volumeOriginY * scale, p.volumeOriginZ * scale);
    const m::mat3x4 toLocal(EVec(horizontal.x, vertical.x, normal.x),
        EVec(horizontal.y, vertical.y, normal.y), EVec(horizontal.z, vertical.z, normal.z),
        EVec(-m::la::dot(horizontal, origin), -m::la::dot(vertical, origin), -m::la::dot(normal, origin)));
    const m::Manifold original = source.Transform(toLocal);
    const m::Box bounds = original.BoundingBox();
    const double width = std::max(0.01, (brick ? p.brickHeight : p.plankWidth) * scale);
    const double thickness = std::max(0.01, (brick ? p.brickDepth : p.plankThickness) * scale);
    const double length = std::max(0.01, p.brickLength * scale);
    const double joint = std::max(0.0, (brick ? p.mortar : p.plankGap) * scale);
    const EVec pitch(length + joint + clearance, width + joint + clearance, thickness + joint + clearance);
    const double embed = std::max(0.0, p.embed * scale);
    const double pad = embed + (brick ? std::max(length, std::max(width, thickness)) : width + thickness);
    std::vector<m::Manifold> localCuts, windows;
    std::vector<m::Box> windowBoxes;
    for (const m::Manifold& cutter : cutters)
    {
        if (cutter.IsEmpty()) continue;
        const m::Manifold cut = cutter.Transform(toLocal);
        const m::Box cb = cut.BoundingBox();
        if (!VolumeBoxesOverlap(cb, bounds)) continue;
        localCuts.push_back(cut);
        const EVec lo = m::la::max(bounds.min, cb.min - EVec(pad));
        const EVec hi = m::la::min(bounds.max, cb.max + EVec(pad));
        if (hi.x <= lo.x || hi.y <= lo.y || hi.z <= lo.z) continue;
        windows.push_back(m::Manifold::Cube(hi - lo).Translate(lo));
        windowBoxes.emplace_back(lo, hi);
    }
    if (!p.showAll && localCuts.empty()) return;
    const m::Manifold removed = p.showAll ? original : original ^ m::Manifold::BatchBoolean(localCuts, m::OpType::Add);
    if (removed.IsEmpty() || removed.Volume() <= 1e-10) return;
    const double surfaceInset = std::min(0.01 * scale, 0.005 * std::min(width, thickness));
    const m::Manifold interior = VolumeInwardReference(original, surfaceInset);
    const m::Manifold target = p.showAll ? interior : interior ^ m::Manifold::BatchBoolean(windows, m::OpType::Add);
    if (target.IsEmpty()) return;
    const m::Box region = target.BoundingBox();
    const m::Manifold remaining = left == nullptr ? original - removed : left->Transform(toLocal);
    const uint64_t seed = Key((uint64_t)(uint32_t)mainSeed, (uint64_t)(uint32_t)p.seed, 0x564F4C55);
    const int budget = std::clamp(p.volumeBudget, 16, 50000);
    auto gridIndex = [](double value) -> int64_t
    {
        if (!std::isfinite(value) || std::fabs(value) > 9e12)
            throw std::invalid_argument("Stable Volume Fill: grid coordinates exceed the safe range. Move Grid Origin closer to the model or increase part dimensions.");
        return (int64_t)value;
    };
    const int64_t j0 = gridIndex(std::floor((region.min.y - 0.5 * width) / pitch.y));
    const int64_t j1 = gridIndex(std::ceil((region.max.y + 0.5 * width) / pitch.y));
    const int64_t k0 = gridIndex(std::floor((region.min.z - 0.5 * thickness) / pitch.z));
    const int64_t k1 = gridIndex(std::ceil((region.max.z + 0.5 * thickness) / pitch.z));
    if ((long double)(j1 - j0 + 1) * (k1 - k0 + 1) > 8.0L * budget)
        throw std::runtime_error("Stable Volume Fill: too many rows. Increase board/brick dimensions or the Candidate Budget.");
    std::vector<VolumeFillCell> cells;
    for (int64_t k = k0; k <= k1; ++k) for (int64_t j = j0; j <= j1; ++j)
    {
        const double shift = brick && (j & 1) ? 0.5 : 0.0;
        const int64_t i0 = brick ? gridIndex(std::floor((region.min.x - 0.5 * length) / pitch.x - shift)) : 0;
        const int64_t i1 = brick ? gridIndex(std::ceil((region.max.x + 0.5 * length) / pitch.x - shift)) : 0;
        if ((long double)(i1 - i0 + 1) > 8.0L * budget)
            throw std::runtime_error("Stable Volume Fill: too many columns. Increase Brick Length or the Candidate Budget.");
        for (int64_t i = i0; i <= i1; ++i)
        {
            const EVec centre((i + shift) * pitch.x, j * pitch.y, k * pitch.z);
            VolumeFillCell cell;
            cell.i = i; cell.j = j; cell.k = k;
            cell.lo = centre - EVec(length, width, thickness) * 0.5;
            cell.hi = centre + EVec(length, width, thickness) * 0.5;
            if (!brick) { cell.lo.x = bounds.min.x; cell.hi.x = bounds.max.x; }
            const m::Box box(cell.lo, cell.hi);
            if (!VolumeBoxesOverlap(box, region)) continue;
            bool inWindow = p.showAll;
            for (const m::Box& window : windowBoxes) inWindow |= VolumeBoxesOverlap(box, window);
            if (!inWindow) continue;
            if (cells.size() >= (size_t)budget)
                throw std::runtime_error("Stable Volume Fill: Candidate Budget exceeded. Increase the budget or part dimensions; no incomplete layout was applied.");
            cell.key = Key(seed, (uint64_t)i, (uint64_t)j, (uint64_t)k);
            cells.push_back(cell);
        }
    }
    std::vector<std::vector<VolumeFillPiece>> results(cells.size());
    std::vector<m::Manifold> mortars(cells.size());
    const double minimumLength = std::max(0.0, p.volumeMinLength * scale);
    const double minimumSection = std::clamp(p.volumeMinSection, 0.0, 1.0);
    ProjectDamageParallelFor(0, (int64_t)cells.size(), [&](int64_t at)
    {
        const VolumeFillCell& cell = cells[(size_t)at];
        if (U01(cell.key, 1) < std::clamp(p.missing, 0.0, 1.0)) return;
        const m::Manifold shape = VolumeCellSolid(cell, brick ? p.chamfer : 0.0);
        m::Manifold clipped = shape ^ target;
        if (clipped.IsEmpty()) return;
        m::Manifold mortar;
        if (brick && p.mortarJoints && joint > 1e-6)
        {
            const EVec expansion(joint * 0.5, joint * 0.5, 0.0);
            EVec outerLo = cell.lo - expansion, outerHi = cell.hi + expansion;
            const double recess = std::clamp(p.mortarRecess, 0.0, 0.49) * thickness;
            outerLo.z += recess; outerHi.z -= recess;
            if (outerHi.z > outerLo.z)
                mortar = (m::Manifold::Cube(outerHi - outerLo).Translate(outerLo) - shape) ^ target;
        }
        const double probability = std::clamp(brick ? p.brickBroken : p.broken, 0.0, 1.0);
        if (U01(cell.key, 2) < probability)
        {
            const m::Manifold exposed = clipped ^ removed;
            if (!exposed.IsEmpty())
            {
                const m::Box eb = exposed.BoundingBox();
                const double x = eb.min.x + (eb.max.x - eb.min.x) * Range(cell.key, 3, 0.3, 0.7);
                const double gap = clearance + std::min(width, eb.max.x - eb.min.x) * std::clamp(p.trim, 0.0, 1.0) * 0.5;
                const EVec cutNormal = Normalized(EVec(1.0, Range(cell.key, 4, -0.35, 0.35) * p.splinter,
                    Range(cell.key, 5, -0.35, 0.35) * p.splinter), EVec(1, 0, 0));
                const double offset = m::la::dot(cutNormal, EVec(x, (eb.min.y + eb.max.y) * 0.5, (eb.min.z + eb.max.z) * 0.5));
                const m::Manifold halves = clipped.TrimByPlane(cutNormal, offset + gap * 0.5)
                    + clipped.TrimByPlane(-cutNormal, -offset + gap * 0.5);
                clipped = halves;
                if (!mortar.IsEmpty()) mortar = mortar.TrimByPlane(cutNormal, offset + gap * 0.5)
                    + mortar.TrimByPlane(-cutNormal, -offset + gap * 0.5);
            }
        }
        for (const m::Manifold& piece : clipped.Decompose())
        {
            const m::Box box = piece.BoundingBox();
            const EVec size = box.max - box.min;
            if (size.x < minimumLength || size.y < width * minimumSection || size.z < thickness * minimumSection) continue;
            const double volume = piece.Volume();
            if (volume <= 1e-8 || volume < 0.01 * size.x * width * thickness) continue;
            const m::Manifold exposed = piece ^ removed;
            if (!p.showAll && (exposed.IsEmpty() || exposed.Volume() < std::max(1e-8, volume * 1e-6))) continue;
            VolumeFillPiece result;
            result.solid = piece; result.box = box; result.cell = (size_t)at;
            result.held = p.showAll || !p.dropLoose || (!remaining.IsEmpty() && (piece ^ remaining).Volume() > std::max(1e-8, volume * 1e-6));
            results[(size_t)at].push_back(std::move(result));
        }
        mortars[(size_t)at] = mortar;
    });
    std::vector<VolumeFillPiece> pieces;
    for (auto& row : results) for (auto& piece : row) pieces.push_back(std::move(piece));
    if (brick && p.dropLoose)
    {
        auto gridKey = [](int64_t i, int64_t j, int64_t k) { return Key(0x564F4C4E, (uint64_t)i, (uint64_t)j, (uint64_t)k); };
        std::unordered_map<uint64_t, std::vector<size_t>> neighbours;
        std::vector<size_t> queue;
        for (size_t i = 0; i < pieces.size(); ++i)
        {
            const auto& cell = cells[pieces[i].cell];
            neighbours[gridKey(cell.i, cell.j, cell.k)].push_back(i);
            if (pieces[i].held) queue.push_back(i);
        }
        for (size_t q = 0; q < queue.size(); ++q)
        {
            const auto& a = pieces[queue[q]];
            const auto& ac = cells[a.cell];
            for (int di = -2; di <= 2; ++di) for (int dj = -1; dj <= 1; ++dj) for (int dk = -1; dk <= 1; ++dk)
            {
                if (di == 0 && dj == 0 && dk == 0) continue;
                const auto found = neighbours.find(gridKey(ac.i + di, ac.j + dj, ac.k + dk));
                if (found == neighbours.end()) continue;
                for (size_t i : found->second)
                {
                    auto& b = pieces[i]; if (b.held) continue;
                    const auto& bc = cells[b.cell];
                    if (bc.i != ac.i + di || bc.j != ac.j + dj || bc.k != ac.k + dk) continue;
                    if (!VolumeBoxesOverlap(a.box, b.box, joint + clearance + 1e-6)) continue;
                    b.held = true; queue.push_back(i);
                }
            }
        }
    }
    SolidTest removedTest;
    removedTest.Build(removed);
    auto append = [&](const m::Manifold& local, Int32 part)
    {
        if (local.IsEmpty()) return;
        const auto mesh = local.GetMeshGL64();
        const Int32 base = (Int32)out.points.size();
        for (size_t v = 0; v < mesh.NumVert(); ++v)
        {
            const EVec point(mesh.vertProperties[v * mesh.numProp], mesh.vertProperties[v * mesh.numProp + 1], mesh.vertProperties[v * mesh.numProp + 2]);
            out.points.push_back(origin + horizontal * point.x + vertical * point.y + normal * point.z);
            out.exposure.push_back(removedTest.Inside(point) ? 1.0f : 0.0f);
        }
        for (size_t f = 0; f < mesh.triVerts.size(); f += 3)
        {
            const Int32 a = base + mesh.triVerts[f], b = base + mesh.triVerts[f + 1], c = base + mesh.triVerts[f + 2];
            out.polys.push_back({a, b, c, c}); out.polyBar.push_back(out.bars); out.polyPart.push_back(part);
        }
        ++out.bars;
    };
    std::vector<bool> keptCell(cells.size(), false);
    for (const auto& piece : pieces)
    {
        if (p.dropLoose && !piece.held) continue;
        append(piece.solid, 0);
        keptCell[piece.cell] = true;
    }
    for (size_t c = 0; c < cells.size(); ++c)
        if (keptCell[c] && !mortars[c].IsEmpty())
            for (const auto& part : mortars[c].Decompose()) append(part, 1);
}
