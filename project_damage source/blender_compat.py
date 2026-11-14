# небольшие поправки для разных версий Blender
# SPDX-License-Identifier: GPL-3.0-or-later
"""Material.use_nodes is deprecated since Blender 5.0, where every material has a tree, and goes away in 6.0."""
import bpy

_NODES_ALWAYS=bpy.app.version>=(5,0,0)


def enable_nodes(material):
    if not _NODES_ALWAYS:material.use_nodes=True


def uses_nodes(material):
    return material.node_tree is not None if _NODES_ALWAYS else material.use_nodes
