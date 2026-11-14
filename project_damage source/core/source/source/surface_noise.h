// общие объявления: добавляем шум с учётом поверхности объекта
// ProjectDamage procedural surface noise - GPL-3.0-or-later; see LICENSE.
//
// The five procedural surface controls the Boolean break surface has (Noise
// Scale, Detail, Roughness, Distortion, Strength), shared by the damage
// assets: Concrete spall (Detail), Lath and Brick back surface (a procedural
// displacement next to the Texture one, break_texture.cpp) and the Hanging
// Hole walls. Every default reproduces the old surfaces to the bit.
#pragma once

#include "project_damage_noise.h"

#include <algorithm>
#include <cmath>

namespace pd_surface_noise {

                                                                          
                                                                       
inline double Fbm(EVec q, double detail, double gain)
{
	detail = std::clamp(detail, 1.0, 8.0);
	const int whole = (int)std::floor(detail);
	const double frac = detail - double(whole);
	double sum = 0.0, amp = 1.0, norm = 0.0;
	for (int o = 0; o < whole; ++o) {
		sum += amp * (2.0 * ProjectDamageNoise(q) - 1.0);
		norm += amp;
		amp *= gain;
		q = q * 2.03 + EVec(17.13, -9.31, 5.77);
	}
	if (frac > 0.0) {
		sum += frac * amp * (2.0 * ProjectDamageNoise(q) - 1.0);
		norm += frac * amp;
	}
	return sum / norm;
}

                                                                         
                                                                  
inline EVec Warp(const EVec& p, double feat, double amount, const EVec& off)
{
	if (!(amount > 0.0) || !(feat > 0.0)) return p;
	const EVec q = p / (3.0 * feat) + off;
	return p + EVec(Fbm(q + EVec(13.1, 2.7, 7.3), 3.0, 0.5),
		Fbm(q + EVec(4.2, 19.4, -3.1), 3.0, 0.5),
		Fbm(q + EVec(-8.7, 5.2, 23.6), 3.0, 0.5)) * (feat * std::min(amount, 4.0));
}

                                                                           
                                                                            
                                                           
inline double Height(const EVec& p, const EVec& offset, double scale, double detail, double gain, double warp)
{
	const double s = std::max(scale, 1e-6);
	const EVec w = Warp(p - offset, s, warp, EVec(31.7, -12.9, 4.4));
	                                                                                     
	const double h = 0.5 + 1.4 * Fbm(w / s + EVec(0.37, 0.71, 0.13), detail, std::clamp(gain, 0.0, 0.95));
	return std::clamp(h, 0.0, 1.0);
}

}                               
