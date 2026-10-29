"""Render statistics (oanim.wm.set_render_stats / render_stats), what tools/render_bench reads: off by default, a list
of each render's figures once on, emptied as it is read. A background run draws nothing, so the list stays empty here;
render_bench checks the figures themselves in a private copy.

Run with:  OpenAnimate --background --python tests/test_render_stats.py
"""
import oanim

assert oanim.wm.render_stats() == [], "nothing recorded while off"
oanim.wm.set_render_stats(True)
stats = oanim.wm.render_stats()
assert isinstance(stats, list) and stats == [], stats
assert oanim.wm.render_stats() == [], "read once, the list empties"
oanim.wm.set_render_stats(False)
assert "gpu" in oanim.wm.render_stats.__doc__ and "mask_make_ms" in oanim.wm.render_stats.__doc__
print("render stats ok")
