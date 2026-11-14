// общие объявления: ограничиваем повреждение областью рисунка
                                                                                  
  
                                                                           
                                                                              
                                                                             
                                                                              
                                                   
  
                                                                              
                                                                  

#pragma once

#include "project_damage_host.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct DrawMaskInput
{
	std::vector<std::vector<EVec>> paths;                              
	double radius = 5.0;                                                   
	double soft = 0.5;                                                                    
	bool invert = false;                                                  
	double size = 0.0;                                                                  
	bool heat = true;                                                      
};

class DrawStrokeMask : public IFieldMask
{
public:
	                                                                        
	                                                                 
	bool Build(const DrawMaskInput& in, const IFieldMask* base)
	{
		base_ = (base != nullptr && base->Active()) ? base : nullptr;
		invert_ = in.invert;
		size_ = std::clamp(in.size, 0.0, 1.0);
		radius_ = std::max(in.radius, 1e-6);
		inner_ = radius_ * (1.0 - std::clamp(in.soft, 0.0, 1.0));
		segA_.clear(); segB_.clear(); grid_.clear();
		for (const auto& path : in.paths)
		{
			if (path.size() == 1)
			{
				if (Finite(path[0])) { segA_.push_back(path[0]); segB_.push_back(path[0]); }
				continue;
			}
			for (size_t i = 1; i < path.size(); ++i)
				if (Finite(path[i - 1]) && Finite(path[i]))
				{
					segA_.push_back(path[i - 1]);
					segB_.push_back(path[i]);
				}
		}
		if (segA_.empty())
			return false;
		cell_ = radius_;
		                                                                      
		                                        
		for (int pass = 0; pass < 32; ++pass)
		{
			uint64_t cells = 0;
			for (size_t s = 0; s < segA_.size(); ++s)
			{
				int64_t lo[3], hi[3];
				SegCells(s, lo, hi);
				cells += (uint64_t)(hi[0] - lo[0] + 1) * (uint64_t)(hi[1] - lo[1] + 1) * (uint64_t)(hi[2] - lo[2] + 1);
			}
			if (cells <= 2000000)
				break;
			cell_ *= 2.0;
		}
		for (size_t s = 0; s < segA_.size(); ++s)
		{
			int64_t lo[3], hi[3];
			SegCells(s, lo, hi);
			for (int64_t x = lo[0]; x <= hi[0]; ++x)
				for (int64_t y = lo[1]; y <= hi[1]; ++y)
					for (int64_t z = lo[2]; z <= hi[2]; ++z)
						grid_[Key(x, y, z)].push_back((int32_t)s);
		}
		return true;
	}

	                                                        
	double StrokeWeight(const EVec& p) const
	{
		const auto it = grid_.find(Key(Cell(p.x), Cell(p.y), Cell(p.z)));
		if (it == grid_.end())
			return 0.0;
		double best = radius_ * radius_;
		for (int32_t s : it->second)
		{
			const EVec a = segA_[(size_t)s];
			const EVec ab = segB_[(size_t)s] - a;
			const double den = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
			const EVec ap = p - a;
			double t = den > 0.0 ? (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / den : 0.0;
			t = std::clamp(t, 0.0, 1.0);
			const EVec d = ap - ab * t;
			best = std::min(best, d.x * d.x + d.y * d.y + d.z * d.z);
		}
		const double dist = std::sqrt(best);
		if (dist >= radius_)
			return 0.0;
		if (dist <= inner_)
			return 1.0;
		const double u = (radius_ - dist) / (radius_ - inner_);
		return u * u * (3.0 - 2.0 * u);
	}

	double DrawWeight(const EVec& p) const
	{
		const double w = StrokeWeight(p);
		return invert_ ? 1.0 - w : w;
	}

	bool Active() const override { return true; }

	                                                                         
	                                                        
	double Weight(const EVec& p) const override
	{
		double w = DrawWeight(p);
		if (base_ != nullptr && w > 0.0)
			w *= 1.0 - base_->Amount() * (1.0 - base_->WeightIfUsed(p));
		return std::clamp(w, 0.0, 1.0);
	}
	double Amount() const override { return 1.0; }
	double Size() const override
	{
		return std::max(size_, base_ != nullptr ? base_->Size() : 0.0);
	}

private:
	static bool Finite(const EVec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
	int64_t Cell(double v) const { return (int64_t)std::floor(v / cell_); }
	static uint64_t Key(int64_t x, int64_t y, int64_t z)
	{
		return ((uint64_t)(x & 0x1FFFFF) << 42) | ((uint64_t)(y & 0x1FFFFF) << 21) | (uint64_t)(z & 0x1FFFFF);
	}
	void SegCells(size_t s, int64_t lo[3], int64_t hi[3]) const
	{
		const EVec a = segA_[s], b = segB_[s];
		lo[0] = Cell(std::min(a.x, b.x) - radius_); hi[0] = Cell(std::max(a.x, b.x) + radius_);
		lo[1] = Cell(std::min(a.y, b.y) - radius_); hi[1] = Cell(std::max(a.y, b.y) + radius_);
		lo[2] = Cell(std::min(a.z, b.z) - radius_); hi[2] = Cell(std::max(a.z, b.z) + radius_);
	}

	const IFieldMask* base_ = nullptr;
	bool invert_ = false;
	double size_ = 0.0, radius_ = 1.0, inner_ = 0.0, cell_ = 1.0;
	std::vector<EVec> segA_, segB_;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid_;
};
