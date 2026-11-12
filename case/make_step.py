# KZ6C B02.1: approximate STEP assembly. Generate board-b02.1.step in this directory first:
#   kicad-cli pcb export step --subst-models --force -o case/board-b02.1.step hardware/kz6c-b02.kicad_pcb
#   python case/make_step.py (requires CadQuery)
# Geometry = outputs/KZ6C-B02-DRAFT/case/case.scad: origin = board centre, +Y towards USB, board top at z = 0.
# Stack: bottom 1.5 | spacer 4 | PCB 1.6 | standoffs 3 | top plate 1.5. MX/tact parts are simplified boxes.
import cadquery as cq
from pathlib import Path
out = Path(__file__).resolve().parent
XS = (-19.05, 0, 19.05)
HOLES = [(x, y) for x in (-28.5, 28.5) for y in (-28.5, 28.5)]

def plate(z, t):
    return cq.Workplane('XY').box(64, 64, t, centered=(True, True, False)).edges('|Z').fillet(3).translate((0, 0, z))

def drill(o, z, t):
    for x, y in HOLES:
        o = o.cut(cq.Workplane('XY').center(x, y).circle(1.15).extrude(t + 2).translate((0, 0, z - 1)))
    return o

board = cq.importers.importStep(str(out / 'board-b02.1.step')).translate((-100, 100, -1.51))
bb = board.val().BoundingBox()
assert abs(bb.xmin + 32) < 0.01 and abs(bb.ymax - 32) < 0.01, (bb.xmin, bb.ymax)

top = drill(plate(3.0, 1.5), 3.0, 1.5)
for x in XS:
    top = top.cut(cq.Workplane('XY').box(14, 14, 4, centered=(True, True, False)).translate((x, 7, 2.5)))
    top = top.cut(cq.Workplane('XY').box(6.8, 6.8, 4, centered=(True, True, False)).translate((x, -19, 2.5)))
spacer = plate(-5.6, 4).cut(cq.Workplane('XY').box(58, 58, 6, centered=(True, True, False)).translate((0, 0, -6.6)))
for x, y in HOLES:
    spacer = spacer.union(cq.Workplane('XY').center(x, y).circle(2.8).extrude(4).translate((0, 0, -5.6)))
spacer = drill(spacer, -5.6, 4).cut(cq.Workplane('XY').box(16, 10, 6, centered=(True, True, False)).translate((0, 30, -6.6)))
bottom = drill(plate(-7.1, 1.5), -7.1, 1.5)

a = cq.Assembly(name='KZ6C_B02_1')
a.add(board, name='PCB_B02_1')
a.add(top, name='top_plate_1.5', color=cq.Color(0.08, 0.08, 0.09))
a.add(spacer, name='spacer_4_matte_acrylic', color=cq.Color(0.85, 0.9, 1.0, 0.5))
a.add(bottom, name='bottom_1.5_white', color=cq.Color(0.92, 0.92, 0.92))
for i, (x, y) in enumerate(HOLES, 1):
    a.add(cq.Workplane('XY').center(x, y).circle(2).circle(1.1).extrude(3), name=f'standoff_{i}', color=cq.Color(.7, .7, .7))
for i, x in enumerate(XS, 1):
    a.add(cq.Workplane('XY').box(14, 14, 5, centered=(True, True, False)).translate((x, 7, 0)), name=f'MX_body_{i}', color=cq.Color(.3, .3, .3))
    a.add(cq.Workplane('XY').box(18, 18, 8, centered=(True, True, False)).translate((x, 7, 6.6)), name=f'keycap_{i}', color=cq.Color(.07, .07, .08))
    a.add(cq.Workplane('XY').box(6, 6, 3.5, centered=(True, True, False)).translate((x, -19, 0)), name=f'tact_body_T{i}', color=cq.Color(.4, .4, .4))
    a.add(cq.Workplane('XY').center(x, -19).circle(1.75).extrude(4.5).translate((0, 0, 3.5)), name=f'tact_stem_T{i}', color=cq.Color(.1, .1, .1))
a.export(str(out / 'KZ6C-B02.1-assembly.step'))
# interference check between rigid case parts and the PCB with its parts
parts = {'top': top, 'spacer': spacer, 'bottom': bottom}
for n, s in parts.items():
    v = board.val().intersect(s.val()).Volume() if hasattr(board.val(), 'intersect') else None
    print(f'{n} ∩ PCB, mm³:', round(v, 3) if v is not None else '?')
print('saved', out / 'KZ6C-B02.1-assembly.step')
