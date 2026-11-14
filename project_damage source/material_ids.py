# приводим номера материалов к допустимым слотам меша
def original(material):
    if material is None:return None
    value=material.original if material.is_evaluated else material
    if value is None or value.is_evaluated:
        raise ValueError('A temporary evaluated material cannot be stored on the Project Damage result')
    return value


def normalize_mesh(mesh):
    for index,material in enumerate(mesh.materials):
        value=original(material)
        if value!=material:mesh.materials[index]=value
    return mesh
