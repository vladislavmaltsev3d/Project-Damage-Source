// общие объявления: повторяем случайную последовательность, совместимую с Python
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

// py_random.h - CPython-compatible random.Random for non-negative 32-bit seeds.
//
// Derived from the MT19937 reference implementation mt19937ar.c,
// Copyright (C) 1997 - 2002, Makoto Matsumoto and Takuji Nishimura,
// All rights reserved. Redistributed under the BSD 3-clause license;
// full notice in cpp/THIRD-PARTY-LICENSES.txt (ships with the plugin).
// Seed derivation and random() output follow CPython's random module
// (behavioral compatibility only, no CPython code).
#pragma once
#include <cstdint>

class PyRandom
{
public:
    explicit PyRandom(uint32_t seed) { SeedInt(seed); }
                                                         
    double Random()
    {
        const uint32_t a = GenrandInt32() >> 5;             
        const uint32_t b = GenrandInt32() >> 6;             
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }
    double Uniform(double lo, double hi) { return lo + (hi - lo) * Random(); }

private:
    static const int N = 624;
    uint32_t mt_[624];
    int mti_ = 625;

    void InitGenrand(uint32_t s)
    {
        mt_[0] = s;
        for (mti_ = 1; mti_ < N; mti_++)
            mt_[mti_] = 1812433253u * (mt_[mti_ - 1] ^ (mt_[mti_ - 1] >> 30)) + (uint32_t)mti_;
    }
    void InitByArray(const uint32_t* key, int keyLen)
    {
        InitGenrand(19650218u);
        int i = 1, j = 0;
        int k = (N > keyLen) ? N : keyLen;
        for (; k; k--) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
            i++; j++;
            if (i >= N) { mt_[0] = mt_[N - 1]; i = 1; }
            if (j >= keyLen) j = 0;
        }
        for (k = N - 1; k; k--) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
            i++;
            if (i >= N) { mt_[0] = mt_[N - 1]; i = 1; }
        }
        mt_[0] = 0x80000000u;
    }
                                                                             
                                                                            
    void SeedInt(uint32_t seed)
    {
        uint32_t key[1] = { seed };
        InitByArray(key, 1);
    }
    uint32_t GenrandInt32()
    {
        static const uint32_t MATRIX_A = 0x9908b0dfu, UPPER = 0x80000000u, LOWER = 0x7fffffffu;
        uint32_t y;
        if (mti_ >= N) {
            if (mti_ == N + 1) InitGenrand(5489u);
            int kk;
            for (kk = 0; kk < N - 397; kk++) {
                y = (mt_[kk] & UPPER) | (mt_[kk + 1] & LOWER);
                mt_[kk] = mt_[kk + 397] ^ (y >> 1) ^ ((y & 1u) ? MATRIX_A : 0u);
            }
            for (; kk < N - 1; kk++) {
                y = (mt_[kk] & UPPER) | (mt_[kk + 1] & LOWER);
                mt_[kk] = mt_[kk + (397 - N)] ^ (y >> 1) ^ ((y & 1u) ? MATRIX_A : 0u);
            }
            y = (mt_[N - 1] & UPPER) | (mt_[0] & LOWER);
            mt_[N - 1] = mt_[396] ^ (y >> 1) ^ ((y & 1u) ? MATRIX_A : 0u);
            mti_ = 0;
        }
        y = mt_[mti_++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= (y >> 18);
        return y;
    }
};
