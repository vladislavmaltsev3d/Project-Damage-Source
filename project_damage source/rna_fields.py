# SPDX-License-Identifier: GPL-3.0-or-later
"""Cached RNA property lists.

The effect stacks (Boolean, Wall, Fracture) used to walk all ~2200 RNA
properties of ProjectDamageProps every time they needed their own subset.
That walk ran per source on every frame change and on every depsgraph event.
The subset of a registered class never changes, so it is computed once.

The cache is dropped whenever properties can be redefined: on register,
unregister and after ui_hints rewrites the descriptions.
"""

_CACHE = {}


def select(p, key, predicate):
    """Tuple of RNA properties of `p` accepted by `predicate`, cached by `key`."""
    rna = p.bl_rna
    slot = (rna.as_pointer(), key)
    hit = _CACHE.get(slot)
    if hit is None:
        hit = _CACHE[slot] = tuple(r for r in rna.properties if predicate(r))
    return hit


def mapping(p, key, predicate):
    """{identifier: RNA property} for the same subset as select()."""
    rna = p.bl_rna
    slot = (rna.as_pointer(), key, 'map')
    hit = _CACHE.get(slot)
    if hit is None:
        hit = _CACHE[slot] = {r.identifier: r for r in select(p, key, predicate)}
    return hit


def clear():
    _CACHE.clear()
