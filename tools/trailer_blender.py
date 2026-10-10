#!/usr/bin/env python3
"""The trailer's 3-D shot, built from nothing by Blender.

    tools/trailer_blender.sh                  # the wrapper; see it for paths
    Blender --background --python tools/trailer_blender.py -- --out DIR \\
            --screen docs/trailer/assets/plates/world-map.png --frames 150

WHY THE SCENE IS MODELLED HERE RATHER THAN OPENED FROM A .blend. There is no
.blend in this repository, and the two fly plates the old cut used came from
somewhere nobody can now point at. That is tolerable for a README image and it
is not tolerable for a Steam trailer, which is commercial use: every frame in
it has to be something this project owns. So the scene is primitives, driven
from this file. Nothing is imported, nothing is downloaded, and re-running it
produces the same shot.

It is ALSO the answer to "static images are not cinematic". The camera moves
through a room toward a monitor, the monitor plays the game, and a fly crosses
the lens. The last second is the screen filling the frame, which is a cut the
edit can hide a transition inside -- the trailer passes THROUGH the monitor
into the game footage.

THE SCREEN IS A REAL TEXTURE. `--screen` takes a PNG or an MP4. Given the
timelapse mp4 the monitor plays the actual game evolving while the camera moves,
which is the difference between a 3-D shot of a product and a 3-D shot of a
product doing something.

ENGINE. Eevee needs a GL context, which `--background` does not always get;
Cycles always works headless and is slower. `--engine` picks, and the default
tries Eevee and falls back on its own.
"""
import math, os, sys

try:
    import bpy, mathutils                                        # noqa
except ImportError:
    sys.exit("run this inside Blender: see the module docstring")


def argv_after_dashes():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def opt(name, default=None):
    a = argv_after_dashes()
    return a[a.index(name) + 1] if name in a else default


# ------------------------------------------------------------------ helpers

def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def mat(name, base, rough=0.6, metal=0.0, emit=None, emit_str=1.0, alpha=1.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*base, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    if emit is not None:
        # 2.93 has no "Emission Strength" on every build; set both when present.
        if "Emission" in bsdf.inputs:
            bsdf.inputs["Emission"].default_value = (*emit, 1.0)
        if "Emission Strength" in bsdf.inputs:
            bsdf.inputs["Emission Strength"].default_value = emit_str
    if alpha < 1.0:
        bsdf.inputs["Alpha"].default_value = alpha
        m.blend_method = "BLEND"
    return m


def box(name, size, loc, material=None, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc, rotation=rot)
    o = bpy.context.object
    o.name = name
    o.scale = size
    if material:
        o.data.materials.append(material)
    return o


def ball(name, r, loc, material=None, scale=(1, 1, 1)):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=r, location=loc, segments=32,
                                         ring_count=16)
    o = bpy.context.object
    o.name = name
    o.scale = scale
    bpy.ops.object.shade_smooth()
    if material:
        o.data.materials.append(material)
    return o


# -------------------------------------------------------------------- scene

def screen_material(path):
    """An emissive plane carrying the picture. The monitor is the scene's main
    light source, which is why the room reads as lit BY the game."""
    m = bpy.data.materials.new("Screen")
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    emi = nt.nodes.new("ShaderNodeEmission")
    tex = nt.nodes.new("ShaderNodeTexImage")
    img = bpy.data.images.load(os.path.abspath(path))
    tex.image = img
    if path.lower().endswith((".mp4", ".mov", ".avi", ".mkv")):
        img.source = "MOVIE"
        tex.image_user.use_auto_refresh = True
        tex.image_user.frame_duration = bpy.context.scene.frame_end
        tex.image_user.use_cyclic = True
    emi.inputs["Strength"].default_value = 2.6
    nt.links.new(tex.outputs["Color"], emi.inputs["Color"])
    nt.links.new(emi.outputs["Emission"], out.inputs["Surface"])
    return m


def build_fly(parent_loc):
    """A stylised fly: two spheres, two eyes, two wings. Not a model of a
    species and deliberately not one -- it reads at 24 frames a second."""
    body_m = mat("FlyBody", (0.085, 0.072, 0.060), rough=0.28)
    eye_m = mat("FlyEye", (0.62, 0.055, 0.050), rough=0.18,
                emit=(0.45, 0.03, 0.03), emit_str=1.3)
    wing_m = mat("FlyWing", (0.80, 0.84, 0.92), rough=0.08, alpha=0.30)

    bpy.ops.object.empty_add(location=parent_loc)
    rig = bpy.context.object
    rig.name = "FlyRig"

    parts = []
    parts.append(ball("FlyAbdomen", 0.055, (0, 0.055, 0), body_m,
                      scale=(1.0, 1.7, 0.95)))
    parts.append(ball("FlyThorax", 0.045, (0, -0.030, 0.006), body_m,
                      scale=(1.0, 1.1, 1.0)))
    head = ball("FlyHead", 0.034, (0, -0.090, 0.012), body_m)
    parts.append(head)
    for sx in (-1, 1):
        parts.append(ball("FlyEye%d" % sx, 0.022,
                          (sx * 0.022, -0.100, 0.016), eye_m,
                          scale=(1.0, 1.25, 1.15)))
    wings = []
    for sx in (-1, 1):
        bpy.ops.mesh.primitive_plane_add(size=1, location=(sx * 0.075, 0.0, 0.055))
        w = bpy.context.object
        w.name = "FlyWing%d" % sx
        w.scale = (0.16, 0.075, 1.0)
        w.rotation_euler = (0, 0, 0)
        w.data.materials.append(wing_m)
        wings.append(w)
    for o in parts + wings:
        o.parent = rig
    return rig, wings


def build(screen_path, frames):
    clear()
    sc = bpy.context.scene
    sc.frame_start, sc.frame_end = 1, frames

    dark = mat("Room", (0.022, 0.025, 0.033), rough=0.92)
    desk = mat("Desk", (0.030, 0.026, 0.022), rough=0.80)
    plastic = mat("Plastic", (0.020, 0.022, 0.027), rough=0.55)
    key = mat("Keys", (0.045, 0.048, 0.056), rough=0.6)

    box("Floor", (9, 9, 0.1), (0, 0, -0.05), dark)
    box("BackWall", (9, 0.1, 5), (0, 1.9, 2.4), dark)
    box("Desk", (3.0, 1.25, 0.08), (0, 0.55, 0.74), desk)
    box("DeskLegL", (0.08, 1.0, 0.74), (-1.3, 0.55, 0.37), plastic)
    box("DeskLegR", (0.08, 1.0, 0.74), (1.3, 0.55, 0.37), plastic)

    # The monitor: a bezel box, a stand, and the emissive plane just in front.
    box("Bezel", (1.52, 0.06, 0.90), (0, 1.17, 1.28), plastic)
    box("Stand", (0.30, 0.18, 0.30), (0, 1.14, 0.93), plastic)
    bpy.ops.mesh.primitive_plane_add(size=1, location=(0, 1.135, 1.28),
                                     rotation=(math.pi / 2, 0, 0))
    scr = bpy.context.object
    scr.name = "Screen"
    scr.scale = (1.44, 0.81, 1.0)              # 16:9
    scr.data.materials.append(screen_material(screen_path))

    box("Keyboard", (0.86, 0.30, 0.025), (0, 0.33, 0.80), key,
        rot=(math.radians(-4), 0, 0))

    # Light. The screen does most of it; this is a warm lamp off to one side so
    # the desk is not lit only in blue, and a very dim fill so the room's far
    # corners are not pure black (which reads as a missing wall, not as dark).
    bpy.ops.object.light_add(type="AREA", location=(-2.1, -0.6, 2.2))
    lamp = bpy.context.object
    lamp.data.energy = 48
    lamp.data.size = 1.1
    lamp.data.color = (1.0, 0.82, 0.58)
    lamp.rotation_euler = (math.radians(58), 0, math.radians(-56))
    bpy.ops.object.light_add(type="AREA", location=(2.4, -1.4, 1.6))
    fill = bpy.context.object
    fill.data.energy = 14
    fill.data.size = 2.5
    fill.data.color = (0.52, 0.62, 0.95)
    fill.rotation_euler = (math.radians(70), 0, math.radians(52))

    bpy.ops.object.light_add(type="AREA", location=(-1.9, -2.3, 1.15))
    rim = bpy.context.object
    rim.data.energy = 26
    rim.data.size = 0.7
    rim.data.color = (0.92, 0.80, 0.62)
    rim.rotation_euler = (math.radians(92), 0, math.radians(-128))

    world = bpy.data.worlds.new("W")
    sc.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.012, 0.014, 0.020, 1)

    # ---- camera: a dolly from the room into the screen ---------------------
    bpy.ops.object.camera_add(location=(1.55, -2.65, 1.62))
    cam = bpy.context.object
    sc.camera = cam
    cam.data.lens = 38
    cam.data.dof.use_dof = True
    cam.data.dof.focus_distance = 3.2
    cam.data.dof.aperture_fstop = 2.2

    bpy.ops.object.empty_add(location=(0, 1.13, 1.28))
    target = bpy.context.object
    target.name = "CamTarget"
    tc = cam.constraints.new("TRACK_TO")
    tc.target = target
    tc.track_axis = "TRACK_NEGATIVE_Z"
    tc.up_axis = "UP_Y"

    keys = [(1, (1.55, -2.65, 1.62), 3.95),
            (int(frames * 0.55), (0.52, -1.52, 1.40), 2.75),
            (frames, (0.02, -0.28, 1.29), 1.45)]
    for f, loc, focus in keys:
        sc.frame_set(f)
        cam.location = loc
        cam.data.dof.focus_distance = focus
        cam.keyframe_insert("location", frame=f)
        cam.data.dof.keyframe_insert("focus_distance", frame=f)
    for fc in cam.animation_data.action.fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = "BEZIER"
            kp.handle_left_type = kp.handle_right_type = "AUTO_CLAMPED"

    # ---- the fly: crosses the lens, then settles on the desk ---------------
    rig, wings = build_fly((-0.9, -1.15, 1.35))
    rig.scale = (0.35, 0.35, 0.35)
    path = [(1, (1.30, -1.55, 1.70), (0, 0, math.radians(146))),
            (int(frames * 0.20), (0.92, -1.42, 1.55), (0, math.radians(-15), math.radians(92))),
            (int(frames * 0.42), (0.42, -1.25, 1.34), (0, math.radians(12), math.radians(24))),
            (int(frames * 0.66), (-0.05, -0.95, 1.06), (0, 0, math.radians(-62))),
            (int(frames * 0.84), (0.33, -0.63, 0.802), (0, 0, math.radians(-152))),
            (frames, (0.42, -0.56, 0.800), (0, 0, math.radians(-168)))]
    for f, loc, rot in path:
        rig.location = loc
        rig.rotation_euler = rot
        rig.keyframe_insert("location", frame=f)
        rig.keyframe_insert("rotation_euler", frame=f)
    for fc in rig.animation_data.action.fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = "BEZIER"
            kp.handle_left_type = kp.handle_right_type = "AUTO_CLAMPED"

    # Wings: a flap every other frame. A real fly beats far faster than any
    # frame rate can show, so this is a readable stand-in, not a simulation --
    # and it stops once the fly has landed.
    land = int(frames * 0.78)
    for w, sgn in zip(wings, (1, -1)):
        for f in range(1, frames + 1):
            amp = 0.95 if f < land else 0.06
            w.rotation_euler = (sgn * amp * math.sin(f * 1.9), 0, 0)
            w.keyframe_insert("rotation_euler", frame=f)
        for fc in w.animation_data.action.fcurves:
            for kp in fc.keyframe_points:
                kp.interpolation = "LINEAR"
    return sc


def render(sc, out_dir, engine, res, samples):
    w, h = res
    sc.render.resolution_x, sc.render.resolution_y = w, h
    sc.render.resolution_percentage = 100
    sc.render.image_settings.file_format = "PNG"
    sc.render.filepath = os.path.join(out_dir, "f")
    sc.render.film_transparent = False
    if engine == "CYCLES":
        sc.render.engine = "CYCLES"
        sc.cycles.samples = samples
        sc.cycles.use_denoising = True
    else:
        sc.render.engine = "BLENDER_EEVEE"
        sc.eevee.taa_render_samples = samples
        sc.eevee.use_bloom = True
        sc.eevee.bloom_intensity = 0.035
        sc.eevee.use_gtao = True
        sc.eevee.use_soft_shadows = True
    os.makedirs(out_dir, exist_ok=True)
    bpy.ops.render.render(animation=True)


def main():
    out = opt("--out", "/tmp/trailer-blender")
    screen = opt("--screen", "docs/trailer/assets/plates/world-map.png")
    frames = int(opt("--frames", "150"))
    engine = opt("--engine", "EEVEE").upper()
    samples = int(opt("--samples", "48"))
    w = int(opt("--width", "1920"))
    h = int(opt("--height", "804"))
    sc = build(screen, frames)
    try:
        render(sc, out, engine, (w, h), samples)
    except Exception as exc:
        if engine == "CYCLES":
            raise
        print("[blender] %s failed (%s); falling back to Cycles" % (engine, exc))
        render(sc, out, "CYCLES", (w, h), max(samples, 48))
    print("[blender] wrote %s/f####.png  (%d frames)" % (out, frames))


if __name__ == "__main__":
    main()
