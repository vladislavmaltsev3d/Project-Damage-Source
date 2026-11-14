# совместимость со старым порядком эффектов в сохранённых сценах
"""Compatibility for User_lib presets exported during the ordering experiment.

The combined evaluator does not read or apply an effect order. Keep these two
entry points so previously exported user presets can still be loaded.
"""


def active(source):
    return False


def load_order(source, order, ordered=True):
    if source is not None and 'project_damage_ordered_effects' in source:
        del source['project_damage_ordered_effects']
