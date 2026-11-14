# общие элементы настройки рассеивания камней и пены
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rock subdivision and the Expanding Foam gap-placement explanation."""
from pathlib import Path
import bpy
import numpy as np


def subdivide_rocks(points, triangles, uvs, variation, height, levels):
    """Closed triangular Loop subdivision; level 0 is the exact old mesh.

    Each rock stays disconnected. Interpolate the existing UV and scalar fields
    without changing scatter candidates, the random seed, or shard count.
    """
    points=np.asarray(points);triangles=np.asarray(triangles,dtype=np.int32)
    uvs=np.asarray(uvs);variation=np.asarray(variation);height=np.asarray(height)
    for _ in range(max(0,min(2,int(levels)))):
        if not len(triangles):break
        a,b,c=triangles.T
        pairs=np.concatenate((triangles[:,[0,1]],triangles[:,[1,2]],triangles[:,[2,0]]))
        edges,inverse,counts=np.unique(np.sort(pairs,axis=1),axis=0,return_inverse=True,return_counts=True)
        left,right=edges.T;n=len(points)
        opposite=np.concatenate((c,a,b));opposite_sum=np.zeros((len(edges),3),np.float64)
        np.add.at(opposite_sum,inverse,points[opposite])
        odd=(points[left]+points[right])*.5
        interior=counts==2
        odd[interior]=.375*(points[left[interior]]+points[right[interior]])+.125*opposite_sum[interior]
        neighbor_sum=np.zeros((n,3),np.float64)
        np.add.at(neighbor_sum,left,points[right]);np.add.at(neighbor_sum,right,points[left])
        degree=np.bincount(edges.ravel(),minlength=n)
        beta=np.zeros(n,np.float64);good=degree>=3
        beta[good]=(.625-(.375+.25*np.cos(2*np.pi/degree[good]))**2)/degree[good]
        even=points*(1-degree*beta)[:,None]+neighbor_sum*beta[:,None]
        boundary=np.unique(edges[counts!=2]);even[boundary]=points[boundary]
        points=np.concatenate((even,odd))
        uvs=np.concatenate((uvs,(uvs[left]+uvs[right])*.5))
        variation=np.concatenate((variation,(variation[left]+variation[right])*.5))
        height=np.concatenate((height,(height[left]+height[right])*.5))
        ab,bc,ca=(inverse.reshape(3,-1)+n)
        triangles=np.concatenate((np.stack((a,ab,ca),1),np.stack((b,bc,ab),1),np.stack((c,ca,bc),1),np.stack((ab,bc,ca),1))).astype(np.int32)
    return points,triangles,uvs,variation,height


_PREVIEWS=None
HELP_IMAGE='foam_gaps_help.png'


def clear_previews():
    global _PREVIEWS
    if _PREVIEWS is not None:
        import bpy.utils.previews
        bpy.utils.previews.remove(_PREVIEWS);_PREVIEWS=None


def help_icon():
    global _PREVIEWS
    import bpy.utils.previews
    path=Path(__file__).with_name(HELP_IMAGE)
    if not path.is_file():return 0
    if _PREVIEWS is None:_PREVIEWS=bpy.utils.previews.new()
    if 'foam_gaps' not in _PREVIEWS:_PREVIEWS.load('foam_gaps',str(path),'IMAGE')
    return _PREVIEWS['foam_gaps'].icon_id


# действие «Why no foam?»
class PROJECT_DAMAGE_OT_foam_help(bpy.types.Operator):
    bl_idname='project_damage.foam_help';bl_label='Why no foam?'
    bl_description='Explain where Gaps mode can find foam and why an area may remain empty'
    def invoke(self,context,event):
        help_icon()
        return context.window_manager.invoke_props_dialog(self,width=620)
    def draw(self,context):
        layout=self.layout;icon=help_icon()
        if icon:
            row=layout.row();row.alignment='CENTER';row.template_icon(icon_value=icon,scale=8.)
        layout.label(text='Preview: '+HELP_IMAGE,icon='IMAGE_DATA')
        for line in (
            'Gaps mode looks for two nearby surfaces facing each other.',
            'A single sharp edge or a flat surface is not enough.',
            'The opening must fall between Min Gap and Max Gap.',
            'Foam grows near the exposed mouth of the gap; deep hidden parts are skipped.',
            'Samples are scattered by surface area: a tiny gap can be missed.',
            'Use a gap with enough surface area, or raise Samples for small details.',
            'Check Min Openness if the gap mouth is being rejected.',
            'Use Draw mode to place foam along a chosen path without a physical gap.',
        ):layout.label(text=line)
    def execute(self,context):return {'FINISHED'}

CLASSES=(PROJECT_DAMAGE_OT_foam_help,)
