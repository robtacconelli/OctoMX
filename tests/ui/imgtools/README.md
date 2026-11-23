Pixel-measurement helpers used to compare OctoMX screenshots with `reference/` (Pillow, run
with `toolchain/venv/bin/python` or any python3 with Pillow):

- `px.py row IMG y x0 x1` / `col IMG x y0 y1`: colour runs along a row/column; `zoom IMG x y w h N OUT`
- `cmp.py REF dx dy MINE x y w h scale OUT`: reference crop (client offset dx,dy) above my crop
- `cmp.py diff A dx dy B OUT`: mask of differing pixels + count
- `dark.py IMG x0 y0 x1 y1 [thr] [dy]`: bounding box of dark (text) pixels
- `ink.py IMG x0 y0 x1 y1`: bounding box of pixels differing from the top-left colour
