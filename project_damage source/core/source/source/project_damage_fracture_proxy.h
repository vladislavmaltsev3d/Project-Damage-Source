// общие объявления: готовим упрощённую поверхность для направления трещин
// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to project_damage_fracture.cpp. A low-pass geometric guide keeps
// triangle correspondence: no welding across sheets, no remesh of the target.
// The BVH lifts cutter spines back to the original detailed surface.

struct FrSurfaceProxy
{
	FrTriMesh original;
	ProjectDamageBvhTree tree;
	std::vector<Int32> component;

	void Build(FrTriMesh& mesh, double radius)
	{
		original = mesh;
		const size_t nv = mesh.verts.size();
		std::vector<std::vector<Int32>> adj(nv);
		std::vector<bool> pinned(nv, false);
		std::map<std::pair<Int32, Int32>, int> edges;
		for (const auto& t : mesh.tris)
			for (int e = 0; e < 3; ++e)
				++edges[std::minmax(t.v[e], t.v[(e + 1) % 3])];
		for (const auto& item : edges)
		{
			const Int32 a = item.first.first, b = item.first.second;
			adj[a].push_back(b); adj[b].push_back(a);
			                                                                 
			if (item.second != 2 || m::la::length(mesh.verts[a] - mesh.verts[b]) > 2.0 * radius)
				pinned[a] = pinned[b] = true;
		}
		std::vector<m::vec3> vn(nv), next(nv);
		std::vector<double> limit(nv, radius);
		for (size_t v = 0; v < nv; ++v)
		{
			double length = 0.0;
			for (Int32 n : adj[v]) length += m::la::length(mesh.verts[n] - mesh.verts[v]);
			if (!adj[v].empty()) limit[v] = std::min(radius, length / adj[v].size());
		}
		for (int pass = 0; pass < 16; ++pass)
		{
			std::fill(vn.begin(), vn.end(), m::vec3(0.0));
			for (const auto& t : mesh.tris)
			{
				const m::vec3 n = m::la::cross(mesh.verts[t.v[1]] - mesh.verts[t.v[0]], mesh.verts[t.v[2]] - mesh.verts[t.v[0]]);
				for (Int32 v : t.v) vn[v] = vn[v] + n;
			}
			next = mesh.verts;
			for (size_t v = 0; v < nv; ++v)
			{
				if (pinned[v] || adj[v].empty()) continue;
				m::vec3 avg(0.0);
				for (Int32 n : adj[v]) avg = avg + mesh.verts[n];
				const m::vec3 n = FrNorm(vn[v]);
				                                                                   
				next[v] = mesh.verts[v] + n * (0.45 * m::la::dot(avg / double(adj[v].size()) - mesh.verts[v], n));
				const m::vec3 delta = next[v] - original.verts[v];
				const double d = m::la::length(delta);
				if (d > limit[v]) next[v] = original.verts[v] + delta * (limit[v] / d);
			}
			                                                                  
			                                                                     
			bool valid = false;
			for (int trial = 0; trial < 8; ++trial)
			{
				valid = true;
				for (size_t ti = 0; ti < mesh.tris.size(); ++ti)
				{
					const auto& t = mesh.tris[ti];
					const m::vec3 n = m::la::cross(next[t.v[1]] - next[t.v[0]], next[t.v[2]] - next[t.v[0]]);
					const m::vec3 old = m::la::cross(mesh.verts[t.v[1]] - mesh.verts[t.v[0]], mesh.verts[t.v[2]] - mesh.verts[t.v[0]]);
					if (m::la::length(old) > 1e-14 && m::la::dot(n, FrNorm(old)) < 0.2 * m::la::length(old)) { valid = false; break; }
				}
				if (valid) break;
				for (size_t v = 0; v < nv; ++v) next[v] = (next[v] + mesh.verts[v]) * 0.5;
			}
			if (!valid) break;
			mesh.verts.swap(next);
		}
		std::vector<ProjectDamageBvhTree::AABB> boxes(mesh.tris.size());
		for (size_t ti = 0; ti < mesh.tris.size(); ++ti)
		{
			const auto& t = mesh.tris[ti];
			mesh.nrm[ti] = FrNorm(m::la::cross(mesh.verts[t.v[1]] - mesh.verts[t.v[0]], mesh.verts[t.v[2]] - mesh.verts[t.v[0]]));
			for (Int32 v : t.v) boxes[ti].Grow(mesh.verts[v]);
		}
		                                                                       
		FrSmoothNormals(mesh, radius);
		tree.Build(boxes);
		component.assign(mesh.tris.size(), -1);
		for (size_t ti = 0; ti < mesh.tris.size(); ++ti)
		{
			if (component[ti] >= 0) continue;
			std::vector<Int32> todo{Int32(ti)}; component[ti] = Int32(ti);
			while (!todo.empty())
			{
				const Int32 t = todo.back(); todo.pop_back();
				for (Int32 n : mesh.tris[t].nbr)
					if (n >= 0 && component[n] < 0) { component[n] = Int32(ti); todo.push_back(n); }
			}
		}
	}

	                                                                       
	                                                                         
	m::vec3 Lift(const FrTriMesh& guide, const m::vec3& p, const m::vec3& normal, Int32 hint) const
	{
		if (tree.Empty() || hint < 0 || size_t(hint) >= component.size()) return p;
		double best = std::numeric_limits<double>::infinity();
		Int32 found = -1; m::vec3 bary(0.0);
		std::vector<Int32> todo{0};
		while (!todo.empty())
		{
			const auto& node = tree.nodes[todo.back()]; todo.pop_back();
			const m::vec3 qbox = m::la::max(node.box.lo, m::la::min(node.box.hi, p));
			const m::vec3 db = p - qbox;
			if (m::la::dot(db, db) > best) continue;
			if (!node.IsLeaf()) { todo.push_back(node.left); todo.push_back(node.right); continue; }
			for (Int32 k = node.start; k < node.start + node.count; ++k)
			{
				const Int32 ti = tree.order[k];
				if (component[ti] != component[hint] || m::la::dot(FrN(guide, ti), normal) <= 0.0) continue;
				const auto& t = guide.tris[ti];
				const auto a = guide.verts[t.v[0]], b = guide.verts[t.v[1]], c = guide.verts[t.v[2]];
				const auto e = b - a, f = c - a;
				const double ee = m::la::dot(e,e), ef = m::la::dot(e,f), ff = m::la::dot(f,f);
				const double den = ee * ff - ef * ef;
				if (den <= 1e-24) continue;
				const double pe = m::la::dot(p-a,e), pf = m::la::dot(p-a,f);
				double u = (ff*pe-ef*pf)/den, v = (ee*pf-ef*pe)/den;
				m::vec3 weights(1.0-u-v, u, v), q = a+e*u+f*v;
				if (u < 0.0 || v < 0.0 || u+v > 1.0)
				{
					double edgeBest = std::numeric_limits<double>::infinity();
					const m::vec3 points[3] = {a,b,c};
					for (int j = 0; j < 3; ++j)
					{
						const auto d = points[(j+1)%3]-points[j];
						const double s = std::max(0.0,std::min(1.0,m::la::dot(p-points[j],d)/MaxD(m::la::dot(d,d),1e-24)));
						const auto qe = points[j]+d*s, diff = p-qe;
						const double d2 = m::la::dot(diff,diff);
						if (d2 < edgeBest) { edgeBest=d2; q=qe; weights=m::vec3(0.0); weights[j]=1.0-s; weights[(j+1)%3]=s; }
					}
				}
				const auto diff = p-q; const double d2 = m::la::dot(diff,diff);
				if (d2 < best) { best=d2; found=ti; bary=weights; }
			}
		}
		if (found < 0) return p;
		const auto& t = original.tris[found];
		return original.verts[t.v[0]]*bary.x + original.verts[t.v[1]]*bary.y + original.verts[t.v[2]]*bary.z;
	}
};
