# различаем варианты Fracture с общими настройками расчёта
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fracture asset identities and placement modes; one shared native network."""
TYPE_BY_ASSET = {
    'fracture_concrete': 'FRACTURE', 'fracture_wood': 'WOOD',
    'fracture_glass': 'GLASS', 'fracture_radial': 'RADIAL',
    'fracture_proximity_default':'FRACTURE',
    'fracture_proximity_radial':'RADIAL',
    'fracture_proximity_glass':'GLASS',
}
PROXIMITY_ASSETS={'fracture_proximity_default':'DEFAULT','fracture_proximity_radial':'RADIAL','fracture_proximity_glass':'GLASS'}
FRACTURE_KEYS = {'fracture', 'fracture_through', 'fracture_volume'} | set(TYPE_BY_ASSET)

def active_key(p):
    if p.fract_through_enable and p.fract_asset in {'fracture_through','fracture_volume'}:
        return 'fracture_volume' if p.fract_asset == 'fracture_volume' else 'fracture_through'
    key = getattr(p, 'fract_asset', 'fracture')
    return key if key in FRACTURE_KEYS and key != 'fracture_through' else 'fracture'

def effective_type(p, key=None):
    key = key or active_key(p)
                                                                           
                                                                        
    if key == 'fracture':return 'FRACTURE'
    return TYPE_BY_ASSET.get(key, p.fract_type)

def proximity_mode(p, key=None):
    key=key or active_key(p)
    if key in PROXIMITY_ASSETS:return PROXIMITY_ASSETS[key]
    if p.fract_mode == 'PAINT':return 'OFF'
    if p.fract_mode == 'PROXIMITY':
        return {'GLASS':'GLASS','RADIAL':'RADIAL'}.get(effective_type(p,key),'DEFAULT')
                                                                                
    return p.fract_prox_mode
