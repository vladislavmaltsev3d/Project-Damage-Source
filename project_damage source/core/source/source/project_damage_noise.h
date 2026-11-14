// общие объявления: повторяемый шум для неровностей и случайного размещения
// ProjectDamage - worn edge damage cut into mesh objects.
// Copyright (C) 2026 Cornelius Dammrich
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// This file is also conveyed by its author, the sole copyright holder,
// under a separate proprietary licence as part of the ProjectDamage plugin for
// Cinema 4D. That does not affect your rights under the GPL above.
// Modified 2026-10-15: comments removed, chip/crack/crater generation and cutter construction reworked.

#pragma once

#include "project_damage_types.h"

#include <cmath>
#include <cstdint>

namespace project_damage_noise_detail {
static const uint8_t kPerm[256] = {
	165, 10, 255, 168, 43, 57, 22, 1, 101, 214, 20, 236, 184, 83, 252, 86,
	191, 38, 169, 198, 242, 182, 64, 148, 87, 23, 243, 88, 190, 213, 235, 134,
	244, 103, 216, 109, 164, 12, 49, 54, 97, 90, 179, 95, 183, 194, 193, 186,
	231, 245, 73, 33, 133, 253, 159, 7, 120, 180, 69, 248, 4, 98, 68, 254,
	140, 106, 161, 143, 215, 132, 13, 79, 153, 107, 138, 9, 234, 175, 221, 178,
	37, 113, 241, 114, 117, 130, 172, 91, 85, 147, 39, 82, 53, 66, 111, 225,
	137, 247, 207, 29, 34, 192, 208, 202, 189, 181, 156, 240, 139, 84, 71, 250,
	55, 239, 28, 160, 166, 24, 157, 102, 119, 62, 200, 217, 26, 67, 93, 16,
	105, 203, 209, 116, 81, 197, 219, 96, 220, 129, 75, 100, 246, 44, 146, 150,
	5, 94, 158, 45, 237, 228, 56, 144, 78, 229, 226, 121, 46, 195, 63, 141,
	32, 11, 205, 196, 224, 112, 136, 154, 199, 14, 176, 201, 238, 163, 108, 222,
	51, 128, 152, 110, 77, 170, 227, 149, 76, 8, 104, 80, 118, 36, 151, 42,
	210, 135, 127, 50, 59, 145, 218, 177, 52, 173, 48, 6, 171, 162, 206, 15,
	41, 131, 204, 18, 115, 61, 126, 185, 70, 65, 21, 58, 125, 17, 174, 0,
	25, 31, 249, 35, 3, 232, 142, 60, 188, 92, 211, 2, 187, 124, 27, 122,
	74, 30, 123, 99, 47, 223, 89, 155, 212, 167, 19, 230, 251, 233, 72, 40,
};

inline int P(int i) { return kPerm[i & 255]; }

inline double Fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
inline double Lerp(double t, double a, double b) { return a + t * (b - a); }

inline double Grad(int hash, double x, double y, double z)
{
	const int h = hash & 15;
	const double u = h < 8 ? x : y;
	const double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
	return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}
}

inline double ProjectDamageNoise(const EVec& p)
{
	using namespace project_damage_noise_detail;

	const double xf = std::floor(p.x), yf = std::floor(p.y), zf = std::floor(p.z);
	const int X = (int)std::fmod(xf, 2147483648.0);
	const int Y = (int)std::fmod(yf, 2147483648.0);
	const int Z = (int)std::fmod(zf, 2147483648.0);
	const double x = p.x - xf, y = p.y - yf, z = p.z - zf;
	const double u = Fade(x), v = Fade(y), w = Fade(z);

	const int A = P(X) + Y,     AA = P(A) + Z,     AB = P(A + 1) + Z;
	const int B = P(X + 1) + Y, BA = P(B) + Z,      BB = P(B + 1) + Z;

	const double n = Lerp(w,
		Lerp(v, Lerp(u, Grad(P(AA), x, y, z), Grad(P(BA), x - 1, y, z)),
				Lerp(u, Grad(P(AB), x, y - 1, z), Grad(P(BB), x - 1, y - 1, z))),
		Lerp(v, Lerp(u, Grad(P(AA + 1), x, y, z - 1), Grad(P(BA + 1), x - 1, y, z - 1)),
				Lerp(u, Grad(P(AB + 1), x, y - 1, z - 1), Grad(P(BB + 1), x - 1, y - 1, z - 1))));

	return n * 0.5 + 0.5;
}
