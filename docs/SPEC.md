<!-- Exported from the SkyAnimate spec doc on 2026-10-18. The live, editable version is the Claude Docs doc; update both when the plan changes. -->

# SkyAnimate — Feature Spec & Roadmap

Oct 18, 2026 · @Mary

## Overview

SkyAnimate is a private, Windows-only desktop app for 2D cut-out and vector animation: Moho's animation model inside Blender's interface and editing habits, written like Blender in C++ with an embedded Python API.

**What it takes from each**

| From Moho | From Blender |
| --- | --- |
| Smart actions (smart bones) | Fast, lag-free viewport and rendering |
| Simple layer system: groups, bones, switch layers | Mirror modifier for armatures and drawings |
| Masking | Grease Pencil-style modifier stack |
| Vector drawing and point tools | Free 3D view outside the camera; imported meshes |
| Bone binding and rigging tools | Modular editors with split views |
|  | Layout and theme; a better timeline |
|  | Modal tools and shortcuts: G move, E extrude |
|  | Custom bone shapes |

**Guiding principles**

- **Moho's data model, Blender's interaction model.** Layers, bones and smart actions behave like Moho; tools, keymaps and editors feel like Blender.
- **Everything is an operator.** Every action is a named command that keymaps, menus, undo and Python all share.
- **Python is first-class.** Tools, panels and add-ons can be written in Python, the way Moho tools are Lua scripts.
- **Small first.** Each milestone ships something usable before the next one starts.

**Not in scope:** public release, macOS and Linux builds, 3D modelling, sculpting, physics simulation, a node-based compositor, or Moho file compatibility.

**Sources:** the Moho scripting API docs scraped from mohoscripting.com, community and built-in Moho Lua tool scripts, and the Blender source tree, all in the project folder. The app is private, so Blender's GPL license only matters if it is ever distributed.

## Core concepts

The data model follows Moho: a document holds a tree of layers, almost every property is an animated channel, and actions are alternative timelines stored inside those channels. Smart bones fall out of that design almost for free.

**Document.** Frame range, frame rate, canvas size, background colour, a 3D camera (track, zoom, roll, pan/tilt channels), timeline markers, a shared style library and layer comps (saved sets of visible layers). Frame 0 is the rigging frame: the rest pose, never rendered.

**Setup frame (frame 0).** SkyAnimate keeps Moho's model: frame 0 holds the setup pose, and there are no separate rest and pose modes. Smart bones need this, because a smart action's effect is measured from its frame-0 values. Blender's clear sense of mode is added on top:

- **Setup toggle** in the viewport header, with a shortcut: it jumps to frame 0, and pressing it again returns to the frame you were on.
- **Clear signals on frame 0:** a coloured viewport border, a "Setup" label in the header, and frame 0 drawn apart at the start of the timeline.
- **Rigging tools only work on frame 0**, as in Moho: adding bones, binding points and layers, bone strength. Elsewhere they are greyed out with the tooltip "Switch to Setup (frame 0)".
- **Auto-keying never applies on frame 0**, because changes there edit the setup pose and carry into every frame.

**Layers.** Every layer has a name, a stable ID, visibility, lock, label colour, an origin, animated transform channels (position, scale, X/Y/Z rotation, shear, flip), opacity, blend mode, masking mode, an optional parent bone, a timing offset and a modifier stack. Types for our app:

| Layer type | Holds | Version |
| --- | --- | --- |
| Vector | Points, curves, shapes and styles | v1 |
| Group | Child layers; can be a masking group | v1 |
| Bone | A group that owns a skeleton and controls its children | v1 |
| Switch | A group that shows one child per frame, chosen by an animated name | v1 |
| Image | A picture, image sequence or video | v1 |
| Mesh 3D | An imported OBJ or glTF mesh placed in the scene | Later |
| Audio | A sound clip with per-frame loudness for lip-sync | Later |
| Text | Editable text turned into vector shapes | Later |
| Particle | Child layers emitted as particles | Maybe |
| Note | A sticky note in the viewport | Maybe |

**Vector drawings.** A drawing is a pool of points, curves through those points, and shapes. A point can sit on several curves, which is how Moho welds lines together. Each point has an animated position, an animated width and an optional bone binding. Curves are smooth splines driven by per-point curvature, weight and offset values rather than stored Bezier handles; we copy that scheme so curves animate the way Moho users expect. A shape is a list of curve segments with a fill and outline style. Styles cascade: a shape has its own style plus up to two shared styles from the library.

**Bones.** A skeleton is a tree of bones. Each bone has a rest position, angle, length and scale plus animated position, angle and scale channels. Bones support IK with locks and angle limits, independent angle, squash and stretch, controlling bones (one bone's angle drives another's), bone dynamics (spring and damping), colour tags and a display shape. Points bind to one bone, to all bones by distance (flexi-binding), or to a chosen subset; whole layers can bind to one bone.

**Local armature viewport.** Every armature has two places its bones can live. **Viewport bones** sit in the scene: they move the character, deform artwork and show in the normal viewport. **Local bones** live in that armature's own local armature viewport, a separate canvas that belongs only to it, and never appear in the main viewport. This is where control bones go, such as smart-bone dials, sliders and switches, so they stop floating around the workspace. Both kinds belong to the same armature and key onto the same timeline. The Add Bone tool works in either place.

- **Control-only.** Local bones drive smart actions and can be controlling bones for viewport bones, but they cannot bind or deform artwork.
- **Separate trees.** A local bone can only be parented to another local bone, never to a viewport bone, and vice versa.
- **Helper artwork.** The local viewport can hold drawings that never render: labels, slider tracks, a face image to click on. Together with the local bones this makes a rig control board.
- **An editor type.** "Armature" is an editor you can put in any split area. It follows the selected armature and can also open as a floating window. Turning a dial there updates the character in the main viewport live.

**Animated channels.** One typed channel class covers numbers, 2D and 3D vectors, colours, booleans and strings. Channels can split into separate X/Y channels, be muted, and have keys selected for the timeline.

**Interpolation: Moho modes on the surface, Blender F-curves underneath.** Each key is stored like a Blender keyframe: a value, an in-handle and an out-handle, and a per-key interpolation mode. The modes are Moho's full set plus Blender's Back easing. Most modes compute their handles automatically, so they behave exactly as in Moho; in the graph editor, dragging a handle turns the key into a Bezier key, the way Blender's auto handles become free. Moho's per-key extras sit on top: a hold length, an interval for animating on 1s, 2s or 3s, a stagger flag and a colour tag. New keys use the default mode from preferences, or copy the previous key's mode.

| Mode | What happens between this key and the next | Settings | Handles |
| --- | --- | --- | --- |
| Linear | Constant speed | — | None |
| Smooth | Eases through the key using its neighbours | — | Automatic |
| Ease In/Out | Slows into and out of the key | — | Automatic |
| Ease In | Slows only on arrival | — | Automatic |
| Ease Out | Slows only on departure | — | Automatic |
| Step | Holds the value, then jumps at the next key | — | None |
| Bezier | Custom curve | In and out angle and length per component | Free, aligned or vector, as in Blender |
| Back | Overshoots slightly, then settles (from Blender) | Overshoot amount | Formula |
| Bounce | Bounces into the next key | — | Formula |
| Elastic | Springs past the next key and settles | — | Formula |
| Noisy | Random jitter around the interpolated value | Amplitude, frequency | Formula |
| Cycle | Loops back to an earlier key | Frames back, or an absolute start frame | Copied from the cycled keys |
| Pose | Holds a value taken from an action | Which action pose | From the action |

All modes also take the hold length (frames to wait before moving) and the interval (1s, 2s, 3s). Because keys share Blender's storage, drivers can later use the same curves.

**Actions.** An action is a named, alternative timeline that lives inside every channel of a layer; the regular timeline is the mainline. Editing an action switches the whole document into that action. Actions can be inserted into the mainline as a copy or by reference, and blended.

**Smart bones.** A smart bone is an action with the same name as a bone. Inside the action, the bone's own angle is keyed over time, for example 0° at frame 0 and 90° at frame 24. On the mainline, the bone's current angle is looked up in that keyed range to find a matching action frame. Every other channel in the action is sampled at that frame, and its change from frame 0 is added on top. Influences from several smart bones always add together. A second action named after the bone plus " 2" handles rotation in the other direction.

**Masking.** Inside a masking group, layers are processed top to bottom. Each layer either adds to the running mask, subtracts from it, clears it and adds, or is clipped by it, with invisible variants of the add and subtract modes. The group decides whether everything starts shown or hidden.

**Reference layers.** A reference layer is a live copy of another layer or group, in the same file or in another .sky file. It shows the original's drawing, bones, smart bones and animation, and updates as the original changes. SkyAnimate copies Moho's rules:

- **Animated channels follow the original** until you key that channel on the reference. The channel is then overridden and marked in the timeline. "Sync channel to original" on a timeline channel drops that one override; "Sync layer to original" drops them all.
- **Settings that are not keyable stay local** to each reference: masking mode, blend mode, label colour, lock and similar. This is what makes advanced masking work. A reference of an animated body can sit in another group as an invisible "add to mask" layer, so shading, textures or outlines are clipped to the body's exact animated shape with no extra animation.
- **Referencing a group** references its whole tree; every child is itself a reference.
- **References to another file** show a warning when the original has changed. Updating offers Moho's options: add new layers, remove missing layers, replace mismatched bones, replace mismatched drawings.
- **Break reference** turns it into an ordinary layer that keeps its current state.
- **Better than Moho:** each channel stores its override flag explicitly, so a reference never silently gains keys or loses its link. Moho users report both problems ([forum thread](https://lostmarble.net/forum/viewtopic.php?t=37872)).

Sources on Moho's behaviour: the scripting API (`DuplicateLayer` by reference, `UpdateReferencedLayer`, `MohoLayerRefSyncOptions`) and [this forum thread](https://lostmarble.net/forum/viewtopic.php?t=34787).

**Tools.** A tool is an object with mouse, key and tablet callbacks, an overlay draw callback, an options bar, saved preferences, and checks that hide or grey it out by context. That maps directly onto our Python tool class.

## Feature spec

Version 1 covers 66 of the 97 features below: enough to draw, rig with smart bones, animate on a timeline and export frames. The rest are tiered Later or Maybe. Change any tier from its dropdown. "Community" means a feature that Moho users built as add-on scripts, so it becomes native here. Speed is a requirement throughout, not a feature: the viewport should play back at the document frame rate.

**Viewport and navigation**

| Feature | From | Tier |
| --- | --- | --- |
| Camera view with pan, zoom and rotate | Both | v1 |
| World/Camera view toggle: a free 3D view outside the camera to see every layer in depth (see below) | Blender | v1 |
| Onion skins | Moho | v1 |
| Grid, guides and snapping | Both | v1 |
| Split any area into more editors, and join them back | Blender | v1 |
| Left toolbar with Box Select (default) and Cursor tools | Blender | v1 |
| Separate 2D cursor (Camera view) and 3D cursor (World view) | Blender, SkyAnimate | v1 |
| Header dropdowns: transform orientation, pivot point, proportional editing | Blender | v1 |
| Snapping dropdown in the header: snap to points, curves, grid or bones | Blender | v1 |

**World/Camera view toggle.** A toggle button in the viewport header switches the view between Camera and World; Numpad 0 does the same, as in Blender.

- **Camera:** you see exactly what renders, framed by the camera. The camera tools (track, zoom, roll, pan/tilt) move the scene camera.
- **World:** a free view you can orbit, pan and zoom around the whole scene. Layers show at their real depth, the camera appears as an outlined frame you can select and move, and imported meshes are visible from any angle.
- Each viewport remembers its own World position, so toggling back returns you to where you were. In a split layout, one viewport can stay in Camera while another is in World.

**Viewport toolbar.** A Blender-style toolbar runs down the left side of the viewport. Which tools appear depends on the active layer type: Box Select and Cursor are always there, and Draw, Split Curve and Knife appear for vector layers.

| Tool | What it does | Keys and options |
| --- | --- | --- |
| Box Select (default) | Drag a rectangle to select, exactly as in Blender | Drag for a new selection, Shift-drag to add, Ctrl-drag to subtract; header buttons for set, extend, subtract, invert and intersect; A selects all, Alt+A deselects |
| Cursor | Click or drag to place the cursor | Shift+right-click places the cursor from any tool, as in Blender |
| Draw | Draw freehand strokes onto the active vector layer, like Blender's Grease Pencil Draw tool; tablet pressure sets line width. Each stroke becomes an editable curve, simplified to a few points and welded to nearby points it touches | Ctrl-drag erases, as in Blender; F resizes the brush and Shift+F changes strength; header options for radius, pressure on or off, stabiliser, auto-close and fill, and the style to draw with |
| Split Curve | Click on a line to add a point there without changing the curve's shape. Works on vector layers, and across all selected layers with multi-layer editing | K switches to this tool from any tool. Shift-click adds the point at the exact middle of the segment; Ctrl-click deletes a point while keeping the curve's shape as closely as possible. Based on the community hs\_splitsegment script |
| Knife | Like Blender's knife: click points to draw a cut path across shapes. Where the cut crosses a line a point is added, and inside a shape the cut becomes a new internal segment welded at both ends. A filled shape the cut crosses is split into separate shapes that keep its style | Ctrl+K from any tool. Click adds a cut point, double-click closes the cut, Enter or Space confirms, Esc cancels, right-click starts a new cut, Ctrl+Z removes the last cut point. Hold Shift to snap to segment midpoints, Ctrl to ignore snapping; X or Y locks the axis, A toggles angle snapping, C toggles cutting through every overlapping shape instead of only the top one |

**Selection keys, as in Blender Grease Pencil edit mode.**

- **1, 2, 3** switch the selection mode: Point, Stroke (a whole curve) or Segment (the part of a curve between two welded junctions).
- **L** selects the whole shape under the mouse; **Shift+L** deselects it.

**Two cursors.** Camera view has a 2D cursor on the canvas and World view has a 3D cursor in the scene. They are separate, and each keeps its own position. New layers, bones and imported meshes are placed at the active view's cursor, and the cursor can be the pivot point.

**Move, rotate, scale.** G moves, R rotates and S scales from any tool, as in Blender.

- Then press **X, Y or Z** to lock to that axis; press it again to switch to the other orientation. **Shift+X, Y or Z** locks to the other two axes.
- Type a number for an exact value, hold Ctrl to snap, Enter or click to confirm, Esc or right-click to cancel.
- Points and bones lie flat on their layer, so Z applies to things with depth: layers, the camera, the 3D cursor and meshes.

**Header dropdowns.** The viewport header carries Blender's three transform controls.

- **Transform orientation:** Global, Local (the layer's own axes), Parent (the parent bone), View or Cursor. Comma opens it as a pie menu.
- **Pivot point:** Bounding box centre, Median point, Individual origins, Active element or Cursor. Period opens it as a pie menu.
- **Proportional editing:** O toggles it. The dropdown picks the falloff (Smooth, Sphere, Root, Inverse square, Sharp, Linear, Constant, Random; Shift+O cycles) and Connected only (Alt+O). Scroll the mouse wheel during G, R or S to resize the influence circle. This replaces Moho's Magnet tool.

**Drawing and vector editing**

| Feature | From | Tier |
| --- | --- | --- |
| Pen tool: click to add points, auto-weld to existing points | Moho | v1 |
| Freehand and brush with tablet pressure setting line width | Moho | v1 |
| Shape primitives: rectangle, oval, polygon | Moho | v1 |
| Select points with box and lasso; transform points | Both | v1 |
| Selection modes on 1, 2, 3 (point, stroke, segment) and L to select a whole shape | Blender | v1 |
| Split Curve tool: click a line to add a point without changing its shape | Community | v1 |
| Knife tool (Ctrl+K): cut across shapes to add internal segments and split fills | Blender | v1 |
| G, R, S modal move, rotate, scale with X/Y/Z axis lock and typed values (see Viewport toolbar) | Blender | v1 |
| E to extrude a new point from an end point | Blender | v1 |
| Curvature, smooth and peak points | Moho | v1 |
| Create shape, paint bucket, fill and outline, shared styles | Moho | v1 |
| Line width per point, hide edge, delete edge | Moho | v1 |
| Stroke sculpting brushes: grab, push, smooth, thickness, strength, with falloff and tablet pressure | Blender | v1 |
| More sculpting brushes: twist, pinch, randomize, clone | Blender | Later |
| Slide point along curve, point reduction | Community | Later |
| Gradients and texture fills | Moho | Later |
| Eraser and scatter brush | Moho | Later |
| Liquid shapes: combine shapes by add, subtract, intersect | Moho | Maybe |
| Pen tablet support through Windows Ink: pressure, tilt and the eraser end | Both | v1 |
| Moho stamp brushes: brush picker, all of Moho's brush settings including line boil, .mohobrush import (see below) | Moho | v1 |

**Brushes.** SkyAnimate copies Moho's stamp brushes: a line can be drawn as a row of image stamps instead of a solid stroke. A brush is part of a line's style, so shared styles share their brush.

- **Brush picker dialog**, laid out like Moho's: a grid of brush thumbnails (with None first), multi-stamp brushes marked with three dots, the settings below, a live preview stroke, Save Brush, OK and Cancel. Alt-click a brush to carry the current settings over to it.
- **Brush files:** Moho's `.mohobrush` file is a zip holding a `brush.json` with the settings below plus one PNG stamp, or a folder of PNG stamps for a multi-stamp brush. SkyAnimate reads these files directly and saves its own brushes in the same format.
- **Library folders:** Preferences lists the folders to load brushes from. Point it at your Moho install's `Resources/Support/Common`, `Debut` and `Pro` brush folders to use Moho's brushes without copying them. They are Smith Micro's artwork, which is fine for private use, so they are never bundled with SkyAnimate.

| Setting | Stored as | What it does |
| --- | --- | --- |
| Align brush with curve | `align` | Stamps rotate to follow the direction of the line |
| Random stamp order | `randomOrder` | Multi-stamp brushes pick stamps at random instead of in sequence |
| Merge alpha | `mergedAlpha` | Overlapping stamps merge into one transparency instead of building up darker |
| Tint brush using stroke color | the style's tint flag | Stamps take the line colour |
| Line boil | `randomize` (inferred) | Stamps re-randomise over time so the line shimmers like hand-drawn animation |
| Jitter angle | `jitter`, in radians | Random rotation range per stamp; 360° means any angle |
| Spacing | `spacing` | Gap between stamps as a percentage of brush size |
| Size variation amplitude | `sizeVariationAmp` | How much stamp size varies |
| Size variation scale | `sizeVariationScale` | How quickly that size variation changes along the line |
| Randomness interval | `randomInterval` | Frames between line-boil changes |
| Angle drift | `angleDrift` | Gradual rotation of stamps along the line |
| Color drift: hue, saturation, value | the style's drift values | Random colour variation per stamp, with an eyedropper for the drift colour |

**Layers and masking**

| Feature | From | Tier |
| --- | --- | --- |
| Vector, group, bone, switch and image layers | Moho | v1 |
| Layers panel: drag to reorder, visibility, lock, label colours | Moho | v1 |
| Masking modes and masking groups | Moho | v1 |
| Blend modes and layer opacity | Both | v1 |
| Multi-layer editing: point tools work on every selected layer at once (see below) | Community | v1 |
| Layer comps | Moho | Later |
| Reference layers: live copies with per-channel overrides and local masking (see Core concepts) | Moho | v1 |
| Layer effects: blur, shadow, shading, outline | Moho | Later |
| Reference images and video to trace over or rotoscope; never rendered | Both | Later |

**Multi-layer editing.** Select several vector layers in the Layers panel (Ctrl-click or Shift-click) and the point tools work on all of them at once.

- Points of every selected layer are shown and selectable; the active layer is highlighted, the others drawn slightly dimmer.
- Box and lasso select pick points across all selected layers in one drag.
- Move, rotate and scale (including G, R, S) transform every selected point together around one shared pivot. Each point still moves correctly inside its own layer, even when the layers have different transforms or bones.
- Curvature, line width, delete, and the sculpt brushes also act on all selected layers.
- On an animated frame, keys are set in each layer that changed. The whole edit is one undo step.

**Bones and rigging**

| Feature | From | Tier |
| --- | --- | --- |
| Add, select, transform and reparent bones | Moho | v1 |
| Bind points (single bone or flexi), bind layers, bone strength | Moho | v1 |
| IK with bone locks and angle limits | Moho | v1 |
| Custom bone shapes: draw a bone as any vector shape | Blender | v1 |
| Bone colours and colour-group visibility | Both | v1 |
| Smart bones (smart actions) | Moho | v1 |
| Local armature viewport: a per-armature canvas for control bones and smart-bone dials (see Core concepts) | SkyAnimate | v1 |
| Symmetrize a rig using \_L and \_R names, including smart bone actions | Blender, community | v1 |
| Controlling bones, independent angle, squash and stretch | Moho | Later |
| Bone dynamics and bone physics as in Moho: spring, damping and torque per angle, position and scale; wind and gravity; physics radius, motor, lock tip, return to zero | Moho | Later |
| Smart warp: deform artwork with a warp mesh | Moho | Later |
| Rigid-body physics | Moho | Maybe |
| Copy, paste and paste-flipped poses using \_L and \_R bone names | Blender, community | v1 |
| Weight painting: paint how strongly each bone moves the points | Blender | Later |
| Pose library: save poses and expressions as thumbnails, apply with one click | Blender | Later |
| Setup toggle: jump to frame 0 and back, with clear on-screen signals and rigging tools limited to frame 0 (see Core concepts) | Moho, Blender | v1 |
| Bone constraints: copy rotation, limit rotation, track to | Blender | Later |

**Animation and timeline**

| Feature | From | Tier |
| --- | --- | --- |
| Animated channels with every Moho interpolation mode, holds and on-twos | Moho | v1 |
| Timeline and dope sheet: select, move, scale, copy and paste keys | Blender | v1 |
| Actions and the mainline; insert an action as a copy or reference | Moho | v1 |
| Blender-style graph editor: drag key handles; editing a Smooth key's handle turns it into Bezier | Blender | Later |
| Tween machine: breakdowns between keys with overshoot | Community | Later |
| Pose tool with on-canvas FK and IK handles | Community | Later |
| Lip-sync driving a switch layer from audio | Moho | Later |
| Drivers: any property controls any other | Blender | Later |
| Action strips layered on the timeline | Blender | Maybe |
| Frame-by-frame drawing: a vector layer whose drawing changes per frame, with onion skins | Both | v1 |
| Auto-keying record button, on by default | Blender | v1 |
| Motion paths: show the path a bone or layer travels over time | Both | Later |
| Audio waveform and scrubbing in the timeline | Both | Later |
| Playback controls: loop, preview range, real-time playback that skips frames if needed | Blender | v1 |

**Modifiers**

| Feature | From | Tier |
| --- | --- | --- |
| Modifier stack on vector layers, applied non-destructively | Blender | v1 |
| Mirror modifier for drawings and armatures | Blender | v1 |
| Noise, smooth, offset, thickness and simplify modifiers | Blender | Later |
| Array, build, dash, outline and envelope modifiers | Blender | Later |

**3D**

| Feature | From | Tier |
| --- | --- | --- |
| Layer depth on Z and an animated 3D camera | Moho | v1 |
| Import OBJ and glTF meshes and animate their transforms | Blender | Later |

**Interface and workflow**

| Feature | From | Tier |
| --- | --- | --- |
| Blender-style layout and default theme | Blender | v1 |
| Operator search and a redo panel to adjust the last action | Blender | v1 |
| Editable keymaps | Blender | v1 |
| Undo and redo for every operation | Both | v1 |
| Python console, Python tools and add-ons | Blender | v1 |
| Theme editor | Blender | Later |
| Properties editor with Layer, Drawing and Modifiers tabs (see below) | Blender | v1 |
| Built-in MCP server so AI assistants can inspect and edit the document (see below) | SkyAnimate | v1 |
| Top bar with File, Edit, Render, Window and Help menus (see below) | Blender | v1 |
| Workspace tabs: Layout, Drawing, Rigging, Animation, Render, Scripting; text editor and info log for Scripting | Blender | v1 |
| Autosave and crash recovery | Blender | v1 |
| Command-line mode: skyanimate.exe --background --python script.py renders or batch-edits without a window | Blender | v1 |
| Asset browser: drag characters, props and styles in from other .sky files | Blender | Later |
| Interface scale setting for 150–200% Windows display scaling | Blender | v1 |
| Save Incremental: scene\_002.sky, scene\_003.sky and so on with one shortcut | Blender | v1 |
| Auto-generated Python API docs, built from the property registry | Blender | v1 |

**Top bar.** Like Blender, the top of the window holds the main menus on the left and the workspace tabs beside them.

| Menu | Contents |
| --- | --- |
| File | New, Open, Open Recent, Save (Ctrl+S), Save As, Save Copy; Import (SVG, images, OBJ and glTF meshes, layers from another .sky file by reference); Export (PNG sequence, video, GIF); Save Startup File; Quit |
| Edit | Undo, Redo, Undo History, Repeat Last, Adjust Last Operation, Operator Search (F3), Preferences |
| Render | Render Frame (F12), Render Animation (Ctrl+F12), View Render (F11), Output Settings |
| Window | New Window, Toggle Fullscreen, Next and Previous Workspace, Toggle Python Console |
| Help | Manual, Python API Reference, Keyboard Shortcuts, Open Log Folder, About SkyAnimate |

**Workspaces.** A workspace is a saved screen layout for one kind of work, chosen from tabs in the top bar; + adds one, and each is saved in the .sky file. You asked for Layout, Render and Scripting; I added Drawing, Rigging and Animation because each needs a different set of editors.

| Workspace | Layout |
| --- | --- |
| Layout (default) | Viewport, Layers panel, Properties editor, timeline along the bottom |
| Drawing (added) | Large viewport in Camera view, tool settings and style swatches, a slim timeline |
| Rigging (added) | Viewport and local armature viewport side by side, Layers panel, Properties editor on the Bone tab, timeline |
| Animation (added) | Viewport, dope sheet and graph editor, Properties editor, a second viewport in Camera view |
| Render | Rendered image viewer, output and render settings, timeline |
| Scripting | Viewport, Python console, text editor for scripts, an info log of every operator run (for copying into scripts), Properties editor |

**Properties editor.** Like Blender's Properties editor: a column of tab icons down the side, each tab showing the active layer's settings as collapsible panels. Every animatable field has a key button beside it, as in Blender: click it to set a key at the current frame, and its colour shows whether the frame is keyed.

| Tab | What it shows | What you can do there |
| --- | --- | --- |
| Layer | Name, type, visibility and lock. Transform: position, scale, rotation X/Y/Z, shear, flip, origin. Opacity, blend mode, masking mode, parent bone, timing offset | Type exact values; set or clear keys per field; reset the transform; jump to the parent bone |
| Drawing | Every shape in the layer, top to bottom in stacking order, with its fill and outline swatches. Below the list: the selected shape's fill, outline, line width and style. Point and curve counts for the layer | Select a shape from the list or the canvas and they stay in sync; rename, hide, reorder by dragging, delete; change fill and outline; assign a shared style |
| Modifiers | The layer's modifier stack, top to bottom, each as a collapsible panel with its own settings | Add from a menu; reorder by dragging; show or hide in the viewport and in renders; apply, duplicate or remove |

The tabs shown depend on the layer type: a bone layer gets a Bone tab, an image layer an Image tab, and the Drawing tab appears only for vector layers. Panels are written in Python, so add-ons can add their own panels to any tab.

**Built-in MCP server.** SkyAnimate includes a Model Context Protocol server, so AI assistants such as Claude can inspect and edit the open document. Moho has no such thing, which is why MohoMCP has to poll for requests inside a tool's draw call; here it is a native part of the app.

- **Built on what already exists.** Every MCP tool calls the same operators and property registry as the UI and Python, so anything a person can do, the server can do, with the same undo steps.
- **Tools:** read the document, layer tree, bones, actions and keyframes; get and set any property by path; run any operator by name; set and delete keys; switch frames and actions; capture a screenshot of a viewport or render a frame; run several operations as one batch and one undo step; and optionally run Python.
- **Resources:** the `sky` Python API reference and the operator list, so an assistant can discover what is available.
- **Transport:** Streamable HTTP on 127.0.0.1 only, plus a small `skyanimate-mcp.exe` stdio bridge for clients that only launch local commands. Requests run on the app's main thread through its event queue, so no polling.
- **Safety:** off by default and switched on in Preferences; a token is required; running Python needs its own separate switch; a status-bar indicator shows when an assistant is connected, and every change it makes can be undone.
- **How it is built:** in Python, with the official MCP Python SDK running inside SkyAnimate's embedded Python.

**Files and output**

| Feature | From | Tier |
| --- | --- | --- |
| Save and open the native file format | Both | v1 |
| Export PNG image sequences | Both | v1 |
| Export video through FFmpeg | Both | Later |
| Import SVG artwork | Moho | Later |
| Export GIF and glTF | Both | Maybe |
| Render settings: resolution, frame range, transparent background, anti-aliasing quality | Both | v1 |

## Architecture sketch

The app is a small Blender: a C++20 core that holds the data and does the math, an operator and keymap layer that all editing goes through, and embedded Python for tools, panels and add-ons. We copy Blender's ideas, not its heavy infrastructure (depsgraph, multi-backend GPU module, raw-memory .blend files).

**Modules**

| Module | Responsibility | Blender equivalent |
| --- | --- | --- |
| `core/` | Math, containers, IDs, logging | `blenlib` |
| `data/` | Document, layer tree, vector drawings, skeletons, actions. Saved data kept apart from runtime caches | `makesdna`, `blenkernel` |
| `rna/` | Property reflection: name, type, range, getter/setter, update callback. Drives UI, Python and animation paths | `makesrna` |
| `anim/` | Animated channels, keyframes, interpolation, actions, smart bones, drivers | `animrig`, `fcurve.cc`, `constraint.cc` |
| `eval/` | Fixed evaluation pipeline that builds an evaluated copy of the scene each frame | `depsgraph`, simplified |
| `modifiers/` | Modifier type table: mirror, noise, smooth, offset, armature deform and more | `modifiers/` |
| `ops/` | Operator registry, macros, undo flag, redo panel | `windowmanager` operators |
| `wm/` | Windows, event loop, keymaps, modal handlers, split-area screen layout | `windowmanager`, `editors/screen` |
| `editors/` | One folder per editor: viewport, timeline and dope sheet, graph, layers, properties, outliner | `editors/space_*` |
| `gpu/` | OpenGL renderer, curve tessellation, overlays for bones, handles and onion skins | `gpu`, `draw` |
| `io/` | Native file format, image and video export, SVG import, mesh import | `blenloader`, `io` |
| `python/` | Embedded CPython and the `app` module | `python/` (bpy) |
| `scripts/` | Python startup scripts: UI panels, default keymap, theme, built-in Python tools | `scripts/` |
| `mcp/` | Built-in MCP server in Python on the official SDK; maps MCP tools to operators and properties; localhost HTTP plus a stdio bridge | None |

**Evaluation order, every frame**

1. Read each animated channel at the current frame from the mainline.
2. Apply smart actions: each driving bone's angle picks a frame in its action, and that action's values are layered on top.
3. Solve the bone pose: forward kinematics, IK, constraints.
4. Deform bound points and layers by their bones.
5. Run each layer's modifier stack.
6. Composite layers with masks and blend modes, then draw overlays.

**Key technical choices**

- **Window, input and tablet:** SDL3. **Graphics:** OpenGL 4.5 first; Vulkan only if needed later.
- **UI:** Dear ImGui with docking wherever it can do the job, themed with Blender's default colours, font and spacing. Custom widgets built on ImGui cover what it lacks or draws differently: Blender-style number fields, pie menus, tabs and icons. Panels are written in Python with a `layout.prop(...)` style API so the UI toolkit can be swapped later.
- **Python:** embedded CPython 3.12+ through pybind11, exposed as a `sky` module shaped like `bpy`.
- **Operators:** each has `poll`, `invoke`, `modal`, `execute` and an UNDO flag, as in Blender. Extrude (E) is a macro: the data operator followed by modal move (G).
- **Keymaps:** data files of (operator, key event, properties) per editor and mode, modelled on Blender's `blender_default.py`.
- **Undo:** an operator with the UNDO flag snapshots the layers it changed, using copy-on-write so unchanged data is shared.
- **File format:** a .sky file: a zip holding a JSON scene description plus binary blobs for point data, with a schema version and migration functions.
- **Build:** CMake with vcpkg for dependencies, on your installed MSVC 14.51 toolchain. Windows only for now; SDL3 and OpenGL keep a later macOS or Linux port possible.

**What a Python operator looks like**

```python
import sky

class BONE_OT_smart_create(sky.types.Operator):
    bl_idname = "bone.smart_create"
    bl_label = "Create Smart Bone"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, ctx):
        return ctx.active_bone is not None and ctx.action is None

    def execute(self, ctx):
        bone = ctx.active_bone
        action = ctx.active_layer.actions.new(name=bone.name)
        action.smart_bone = bone
        ctx.document.edit_action(action)
        return {'FINISHED'}

sky.utils.register_class(BONE_OT_smart_create)
```

## Roadmap

Version 1 arrives in five milestones, and each one ends with something you can actually use. No dates yet: the pace depends on how many hours a week go into it.

1. **Foundation:** CMake build, SDL3 window, OpenGL, ImGui docking, embedded Python; operator registry, keymaps, undo, split areas, reflection registry; command-line mode for scripts and tests; interface scale setting.
   *Gate: a Python operator runs from a key press and undoes cleanly.*
2. **Drawing:** vector layers (points, curves, shapes, styles, brushes) and the curve renderer; pen, freehand, select, G R S E, curvature, fill; save and open; snapping, autosave, crash recovery, Save Incremental, pen tablet support.
   *Gate: draw, fill and save a character that matches the Moho look.*
3. **Animation:** animated channels, interpolation modes, timeline and dope sheet; groups, switch, image and reference layers, masking, onion skins, PNG export; frame-by-frame drawing, auto-keying, playback controls, render settings.
   *Gate: animate a walk cycle by hand and export it as PNG frames.*
4. **Rigging:** bones, point and layer binding, IK, custom bone shapes, bone colours; actions, smart bones, local armature viewport, symmetrize with _L and _R; Setup toggle for frame 0; copy, paste and paste-flipped poses.
   *Gate: a smart-bone head turn works on a rig with IK arms.*
5. **Version 1:** modifier stack and mirror modifier, free 3D view, layer depth; operator search, redo panel, keymap editing, Python console, MCP server; top bar menus, workspaces, Properties editor tabs, auto-generated API docs.
   *Gate: rig and animate a short scene end to end, no Moho needed.*

**After version 1:** graph editor, bone dynamics and physics, bone constraints, smart warp; weight painting, motion paths, pose library, tracing images and video; audio waveform and scrubbing, asset browser, video export, mesh import.

Each gate is a hands-on test; a milestone is done only when its gate passes. Rendering quality is proven in milestone 2 and smart bones in milestone 4, the two riskiest pieces.

## Risks, open questions and references

The biggest risks are vector rendering quality and smart bones' interaction with IK; both get a prototype early, in milestone 2 and milestone 4.

**Risks**

| Risk | Why it matters | Mitigation |
| --- | --- | --- |
| Vector fills and strokes look worse than Moho's | Curves must tessellate into smooth, anti-aliased triangles every frame | Prototype the curve renderer first; compare against Moho screenshots; use multisampling |
| Smart bones fight IK and constraints | Moho samples actions after posing; order mistakes cause jitter | Follow the evaluation order in the architecture section; write regression tests from small rigs |
| UI work balloons | Blender's UI toolkit is huge | Start with Dear ImGui docking; keep panels in Python so the toolkit can change later |
| Python binding grows hard to maintain | Hand-written bindings for every class add up | Generate bindings from the reflection registry, as Blender's RNA does |
| Scope creep | 63 features is years of work | Ship each milestone; move features between tiers instead of adding them to v1 |
| Reference layers lose their link or gain stray keys | Advanced masking depends on references staying in sync; Moho users report both bugs | Store a per-channel override flag; test sync, override and break on nested groups in milestone 3 |

**Open questions**

- Your Moho pros list was cut off after "all the tools are o…". What was the rest?
- Should version 1 open Moho files? Moho's format is not documented, so the spec assumes no.

**Where to look in the reference material**

| Topic | Blender source (`blender-main/blender/source/blender/`) | Moho material (`Moho/`) |
| --- | --- | --- |
| Smart bones | `blenkernel/intern/constraint.cc` (`actcon_evaluate`, the Action constraint) | Smart-bone snippet in `MohoDocs/mohoscripting_text.txt`; `lm_select_bone.lua` |
| Operators, G and E | `windowmanager/WM_types.hh`; `editors/transform/transform_ops.cc`; `editors/grease_pencil/intern/grease_pencil_edit.cc` | Tool lifecycle on the script-structure page |
| Keymaps | `scripts/presets/keyconfig/keymap_data/blender_default.py` (outside `source/`) | Modifier keys listed in each tool's description |
| Split views | `editors/screen/screen_edit.cc`, `screen_geometry.cc`, `screen_ops.cc` | — |
| Drawings and mirror modifier | `makesdna/DNA_grease_pencil_types.h`; `modifiers/intern/MOD_grease_pencil_mirror.cc` | `M_Mesh`, `M_Point`, `M_Curve`, `M_Shape` pages; `sz_auto_mirror` |
| Bones and custom shapes | `makesdna/DNA_armature_types.h`; `draw/engines/overlay/overlay_armature.cc` | `M_Bone`, `M_Skeleton` pages; `sz_symmetrize_bones` |
| Bone deform | `blenkernel/intern/armature_deform.cc` | Flexi-binding methods on `MohoLayer` |
| Keyframes and interpolation | `blenkernel/intern/fcurve.cc` (key storage, handle types, Bezier evaluation, about lines 2000–2440); `editors/space_graph/`; `editors/space_action/` | `InterpSetting` page and `INTERP_*` constants (the modes, hold, interval, stagger); `AnimChannel` pages |
| Python API | `python/intern/bpy_rna.cc`; `scripts/modules/_bpy_types.py` | Lua tool scripts in `moho ai/` and `sz_*` |
| Theme colours | `release/datafiles/userdef/userdef_default_theme.c` (outside `source/`) | — |
