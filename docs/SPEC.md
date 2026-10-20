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
| Masking | Grease Pencil-style layer masks |
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
- **Clear signals on frame 0:** a red border on the viewport and the timeline (and a red playhead), a "Setup" label in the header, and frame 0 drawn apart at the start of the timeline.
- **Rigging tools only work on frame 0**, as in Moho: adding bones, binding points and layers, bone strength. Elsewhere they are greyed out with the tooltip "Switch to Setup (frame 0)". The toolbar has two sections: tools for every frame (Select, Cursor, Transform Bones), then frame-0 tools (Draw, Shape, Split Curve, Weight Select, Weight Paint), greyed out elsewhere with the tooltip "Only available on frame 0"; a frame-0 tool hands over to Select away from frame 0 and comes back on it.
- **Auto-keying never applies on frame 0**, because changes there edit the setup pose and carry into every frame.
- **Keys are changes from the setup pose**, as Blender's pose bones and shape keys, Spine's deform keys and Harmony's deformers keep them (Moho keeps own values): a bone's keys hold its Transform (along the bone's rest axes) and Angle; a point's its Transform, applied before bones bend the drawing, so a keyed shape follows the bones; a handle's its Handle Angle and Length (the turn, and the length added), so a handle swings along an arc between keys. Away from frame 0 the Properties editor shows these, 0 at rest: the Bone panel's Transform and Angle, the Point and Shape panels' Transform, and a handle picked on its own's Angle and Length (its aligned partner turns with it). Reshaping frame 0 after animating keeps every pose's motion. A point added to an animated curve (Split Curve) takes its neighbours' motion, blended by distance along the curve (past an end, the end's), so adding points doesn't break the animation. Older files and Moho imports open converted.

**Layers.** Every layer has a name, a stable ID, visibility, lock, label colour, an origin, animated transform channels (position, scale, X/Y/Z rotation, shear, flip), opacity, blend mode, masks, an optional parent bone, a timing offset and a modifier stack. Types for our app:

| Layer type | Holds | Version |
| --- | --- | --- |
| Vector | Points, curves, shapes and styles | v1 |
| Group | Child layers; its masks apply to everything in it | v1 |
| Bone | A group that owns a skeleton and controls its children | v1 |
| Switch | A group that shows one child per frame, chosen by an animated name | v1 |
| Image | A picture, image sequence or video | v1 |
| Mesh 3D | An imported OBJ or glTF mesh placed in the scene | Later |
| Audio | A sound clip with per-frame loudness for lip-sync | Later |
| Text | Editable text turned into vector shapes | Later |
| Particle | Child layers emitted as particles | Maybe |
| Note | A sticky note in the viewport | Maybe |

**Vector drawings.** A drawing is a pool of points, curves through those points, and shapes. A point can sit on several curves, which is how Moho welds lines together. Each point has an animated position, an animated width and an optional bone binding. Every curve point has two Bezier handles, and every handle is free, like Blender's Free handles: stored, and moved only by hand (or with its point). A new point gets its handles once, from Moho's smooth spline (curvature, weight and offset) or, at corners and straight sides, a third of the way toward its neighbours. Align (right-click a selected point or handle) lines a point's handles up and keeps them in line: moving one turns the other. Right-clicking selected points edited from another layer (a bone layer, or several layers at once) also offers Select Layers of Selected Points: the layer holding them becomes active (others with selected points are picked), the points staying selected. Alt-dragging a handle moves it alone and unaligns the point; Unalign does the same without moving anything. Peak and Smooth set a point's handles once (straight, or smooth and aligned). Viewport Overlays > Handles shows None, Selected, Shape (the default: every handle of the shapes being worked on, any of whose points is selected) or All; Edges selection mode shows none, as it picks segments rather than points. A shape is a list of curve segments with a fill and outline style. Styles cascade: a shape has its own style plus up to two shared styles from the library.

**Bones.** A skeleton is a tree of bones. Each bone has a rest position, angle and length plus animated position, angle and scale channels. Scale is X (across the bone: its thickness) and Y (along it: its length), as Blender's bone axes, and is for posing only: on frame 0, S resizes bones instead. Bones support IK with locks and angle limits, independent angle, maintain volume (squash and stretch), controlling bones (one bone's angle drives another's), bone dynamics (bones hang like weighted strands that swing, jiggle and stretch as the body moves), colour tags and a display shape. Points follow the bones by weights, as Blender's vertex groups: new points get automatic weights (nearer bones pull harder, a bone's strength widens its reach), and weights can be assigned or painted. A whole layer can be parented to one bone instead.

**Local armature viewport.** Every armature has two places its bones can live. **Viewport bones** sit in the scene: they move the character, deform artwork and show in the normal viewport. **Local bones** live in that armature's own local armature viewport, a separate canvas that belongs only to it, and never appear in the main viewport. This is where control bones go, such as smart-bone dials, sliders and switches, so they stop floating around the workspace. Both kinds belong to the same armature and key onto the same timeline. Bones are added and edited the same way in either place.

- **Control-only.** Local bones drive smart actions and can be controlling bones for viewport bones, but they cannot bind or deform artwork.
- **Separate trees.** A local bone can only be parented to another local bone, never to a viewport bone, and vice versa.
- **Helper artwork.** The local viewport can hold drawings that never render: labels, slider tracks, a face image to click on. Together with the local bones this makes a rig control board.
- **An editor type.** "Armature" is an editor you can put in any split area. It follows the selected armature and can also open as a floating window. Turning a dial there updates the character in the main viewport live.

**Animated channels.** One typed channel class covers numbers, 2D and 3D vectors, colours, booleans and strings. Channels can split into separate X/Y channels, be muted, and have keys selected for the timeline.

**Interpolation: Moho modes on the surface, Blender F-curves underneath.** Each key is stored like a Blender keyframe: a value, an in-handle and an out-handle, and a per-key interpolation mode. The modes are Moho's full set plus Blender's Back easing. Most modes compute their handles automatically, so they behave exactly as in Moho; in the graph editor, dragging a handle turns the key into a Bezier key, the way Blender's auto handles become free. Moho's per-key extras sit on top: a hold length, an interval for animating on 1s, 2s or 3s, a stagger flag and a colour tag. New keys get the new-key mode (saved with the preferences; Smooth to begin with), or, with Copy Previous Key on, the mode and settings of the key before them.

| Mode | What happens between this key and the next | Settings | Handles |
| --- | --- | --- | --- |
| Linear | Constant speed | — | None |
| Smooth | Eases through the key using its neighbours, clamped as Blender's auto-clamped handles: equal neighbouring keys hold still and the motion never overshoots a key | — | Automatic |
| Ease In/Out | Starts slowly and slows into the next key | Strength | Automatic |
| Ease In | Slows into the next key | Strength | Automatic |
| Ease Out | Starts slowly from this key | Strength | Automatic |
| Step | Holds the value, then jumps at the next key | — | None |
| Bezier | Custom curve | In and out angle and length per component | Free, aligned or vector, as in Blender |
| Back | Overshoots slightly, then settles (from Blender) | Overshoot amount | Formula |
| Bounce | Bounces into the next key | — | Formula |
| Elastic | Springs past the next key and settles | Amplitude (how far the first swing goes), period (how long each wobble is) | Formula |
| Noisy | Random jitter around the interpolated value | Amplitude, frequency | Formula |
| Cycle | Loops back to an earlier key | Frames back, or an absolute start frame | Copied from the cycled keys |
| Pose | Holds a value taken from an action | Which action pose | From the action |

All modes also take the hold length (frames to wait before moving) and the interval (1s, 2s, 3s). The Ease modes' Strength is one of Robert Penner's easing families, from Gentle (Sine) through Quad (the default), Cubic, Quart and Quint to Strong (Expo), plus Circ. Because keys share Blender's storage, drivers can later use the same curves.

**Picking a mode.** The timeline's header has an Interpolation dropdown, as Moho's: with keys selected it sets their mode (it reads Mixed when they differ); with none selected it sets the mode new keys get. T over the timeline opens the same menu at the mouse, and right-clicking a key has it under Interpolation (the right-clicked keys join the selection first). Every item shows a small drawing of its curve, in groups: Smooth, Linear and Step; Ease (Ease In, Ease Out, Ease In/Out, and a Strength submenu); Bouncy (Back, Bounce, Elastic); Special (Noisy, Cycle). Bezier isn't offered: dragging a handle in the graph editor makes it. The menu also has Copy Previous Key, Copy Interpolation and Paste Interpolation (one key's mode and settings onto the selected keys, their values untouched), Use for New Keys, and Settings. The settings button beside the dropdown (or Settings in the menu) opens a panel with the selected segment's curve, drawn as progress from this key (0) to the next (1), and the mode's own settings: Strength, Overshoot, Amplitude and Period, Amount and Wiggles, Loop Back; then Hold and Animate On (1s, 2s, 3s). A change goes to every selected key (a drag is one undo step); with no keys selected it changes what new keys get. A yes/no property's keys (visibility) stay Step whatever is chosen.

**Keys show their mode.** In the timeline a key's shape tells its family: a diamond for Smooth, a thin diamond for Linear, a square for Step, a circle for the Ease modes, a star for Back, Bounce and Elastic, and a hexagon for Noisy and Cycle; a frame where the keys of a row differ is a diamond. (A key whose handles were dragged gets a mark once the graph editor can drag them.)

**Stepped Preview.** A stairs button after the playback buttons (also Playback > Stepped Preview) plays everything as Step, holding each key until the next, to judge the timing of the poses while blocking. No key changes; turning it off brings the curves back.

**Actions.** An action is a named, alternative timeline that lives inside every channel of a layer; the regular timeline is the mainline. Actions belong to a bone layer and cover it and everything inside it. Editing an action switches the whole document into that action: its keys are swapped into the channels, layers outside the bone layer keep the mainline, and frame 0 stays the shared setup pose (editing it in an action changes the setup, and every bone pose with it). Actions can be inserted into the mainline as a copy or by reference, and blended.

**Smart bones.** A smart bone is an action with the same name as a bone. Inside the action, the bone's own angle is keyed over time, for example 0° at frame 0 and 90° at frame 24. On the mainline, the bone's current angle is looked up in that keyed range to find a matching action frame. Every other channel in the action is sampled at that frame, and its change from frame 0 is added on top. Influences from several smart bones always add together. A second action named after the bone plus " 2" handles rotation in the other direction. It plays live while the smart bone is dragged, moving its bones and their points as it turns (bones the action moves without keys of their own included). On the setup frame (0) a smart bone plays nothing: turning it there sets its rest angle, as in Moho.

**Masking.** A layer's masks name other layers, as After Effects' track mattes, Blender Grease Pencil's layer masks and Live2D's clipping do: each mask shows the layer only **Inside** (or only **Outside**) any of the layers it names, and the layer is shown where every mask that's on holds (inside the head and outside the mouth hole: two masks). A group's masks apply to everything in it; naming a group means all of it. Masks don't depend on where layers are in the Layers editor, on Draw Order, or on visibility: a hidden layer still masks (a mask shape is usually kept hidden), and one mask layer can mask many layers. A mask layer's own masks count (its shape masks only where they hold). Each mask has its own Expand by a Pixel (its layers' shapes grow a pixel, hiding thin seams) and Mask With: As Drawn (fills and strokes as they're drawn), Fills Only (Moho's option: their strokes don't mask), Filled Shape (the inside of every shape, filled or not, an outline-only one too, an open one closed straight across, as Rive clips with a shape's path; plus its stroke) or Filled Shape, No Stroke. So one shape can mask one layer with its strokes and another without, and an outline drawing can cut a hole without a filled copy of it. Extend (None, Up, Down, Left, Right) stretches the layers' shapes to the edge of everything that way, in the mask layer's own directions (turning with it and its bone); a filled shape (or any shape with Filled Shape) stretches all of it, an outline-only one just the edges it draws (a reference's hidden edges don't stretch, so a reference showing only the upper lip extends up from the upper lip): Outside the mouth line extended Up hides the mouth and everything above it, so teeth can show over the lower lip without a hand-drawn shape; and its own viewport and render switches, as modifiers have. Copied together (Duplicate with both picked), a layer and its mask layer stay a pair: the copy is masked by the copy; a reference of a group masks itself with its own copies. Where they are set: the Layers editor shows a masked layer with just a mask symbol after its name, no text (orange, the colour masks are shown in everywhere; grey when all its masks are off): hovering it lists the masks and outlines their layers in the viewport, and clicking it opens the Masking tab on that layer. Mask layers aren't marked or outlined there. Alt-dragging a layer onto another masks that one with it (Shift: outside it). The Masking tab in Properties (next to Layer) shows the masks as the Modifiers tab shows modifiers: Add Mask... (click the mask layer, in the viewport or the Layers editor) and With Layer Below, then a box per mask with a header (open arrow, the inside/outside icon, what it does, "Inside head", viewport and render switches, remove, a grip to drag it up or down) and inside it Mode (Inside / Outside), Layers (a list of checkboxes, an eyedropper to add one; hovering the field, or a layer in its list, highlights those layers in the Layers editor, their folded group if they're inside one, and outlines them in the viewport), Expand by a Pixel, Mask With and Extend. On a mask layer, a Used as a Mask section below lists Masking: the layers it masks (click one to select it). **Ctrl Alt G** (Mask With Layer Below) shows the layer only inside the one just below it, as Photoshop's clipping mask. The Mask menu (the Layers editor's right-click menu, and the viewport's right-click menu with nothing selected) lists the masks and has Mask With Layer Below, Inside Layer..., Outside Layer... and Remove Masks. In the viewport the active layer's mask layers are outlined dashed in orange, labelled "inside head" or "outside mouth hole". Undo steps say what happened ("Mask head: Outside mouth hole"). Files saved before this, with Moho's masking groups (Hide all / Reveal all, each layer Masked, Add, Subtract or Clear), open with those turned into masks (a mask layer's old Expand and Fills Only going to the masks naming it): a masked layer gets Inside the layers added before it (since the last Clear) and Outside the ones cut, and the masking-only layers become hidden layers. The Moho converter leaves masking out. Python: layer.masks, layer.masking, sky.ops.mask.add / set / remove / move / with_layer_below.

**Draw order.** Every layer has a keyable Draw Order: Normal (where the Layers editor puts it), In Front Of a layer, or Behind one. Its keys hold until the next key, as visibility's do. A rule can name any layer in the document, across groups and bone layers; a rule on a group moves the whole group; rules are settled outer groups first, then in the Layers editor's order. Masks name their layers, so a masked layer drawn in front of another layer is still clipped to its own mask. A group faded below full opacity is drawn as one image, so nothing inside it can leave it: a rule inside it that names a layer outside moves the whole faded group (the Draw Order row and the Layers editor say so). A rule naming a layer that is gone draws as Normal and shows "(missing)". On frame 0 the commands set the setup order; on any other frame they key it. **Ctrl ]** / **Ctrl [** (Bring Forward / Send Backward) move a layer past the next drawn layer that overlaps it, so every press shows (past one that doesn't only when none does); with **Shift**, Bring to Front / Send to Back. A step that lands where the Layers editor has it anyway goes back to Normal. **In Front Of... / Behind...** wait for a click on the other layer, in the viewport or the Layers editor: the layer pointed at is outlined and named, and the result shows before the click; Esc cancels. The Draw Order menu (right-click in the viewport with nothing selected, the points' right-click menu, and the Layers editor's right-click menu) has these and Normal, under "Now: In Front Of Torso". The Layer tab's Draw Order row has the mode, the layer (a list or the eyedropper) and the key button. The Layers editor marks a layer drawn out of place on this frame with an arrow (up in front, down behind; red when its layer is gone): hovering says what it's drawn in front of or behind, clicking selects that layer. Its Order button lists the drawing layers in this frame's draw order instead of the tree, each with its group; dragging one onto another (upper half in front, lower half behind) sets its rule. In the timeline a Draw Order row (under the layer's Props, beside Opacity and Visibility; with Group by Kind, in its own Draw Order section) has square hold keys and, between them, a bar naming the rule in effect ("In Front Of Torso", or an arrow and the layer's name where the bar is short); hovering a bar outlines both layers in the viewport. Keyed Here lists it as "Draw Order: In Front Of Torso". Undo steps say what happened ("Hand In Front Of Torso"). Viewport clicks that pick a layer take the topmost as drawn. Python: layer.draw_order, sky.data.draw_order (names as drawn, bottom first), sky.ops.layer.draw_order_set(mode=, target=).

**Reference layers.** A reference layer is a live copy of another layer or group, in the same file or in another .sky file. It shows the original's drawing, bones, smart bones and animation, and updates as the original changes. SkyAnimate copies Moho's rules:

- **Layer channels follow the original** (location, rotation, scale, opacity, animation and all) until you edit or key that channel on the reference. The channel is then changed on the copy: teal in the Transform panel and the timeline, where right-clicking it offers Reset to Original (its keys go too) and Push to Original.
- **Settings that are not keyable stay local** to each reference: masks, blend mode, label colour, lock and similar. This is what makes advanced masking work. A hidden reference of an animated body can be the mask of shading, textures or outlines anywhere, so they are clipped to the body's exact animated shape with no extra animation.
- **Referencing a group** references its whole tree; every child is itself a reference.
- **Making one:** **Alt D** in the Layers editor (Blender's Duplicate Linked; Shift D is an ordinary copy), the Layers editor's Ref button or right-click › Reference Layer: Duplicate as Reference, a reference of the active layer just above it.
- **A reference of a reference links to the original** (references never chain): Duplicate as Reference on a reference, or on a referenced group, makes a plain reference of the original layer (inside a group, of each original layer), without the copy's own changes. Files with a chain, made before this, open with it linked to the original.
- **Its own changes, each kept apart** (overrides, as Blender's library overrides, Unity's prefab overrides and Figma's instances keep theirs), found by the original's permanent point and shape ids so they stay on the right ones as the original changes. Everything not changed follows the original.
  - **Looks:** per shape and per setting (fill colour, line colour, line width, brush, fill and outline on), edited in the Drawing tab's Shape panel as on any layer; a colour, line width or brush changed on a copy shows a revert button beside it in the Shape panel, and with its points locked the shape's Position is greyed out (point editing is locked); a shape of the original can be hidden on the reference only (from Python, layer.reference_hide_shape; listed under Changes, to reset); an edge can be hidden or shown on the reference only, with the Edge panel as on any layer, whether its points are locked or not (kept by the ids of the points at either end; Reset Looks brings back the original's); the Reference tab's **Recolor** has just Reset All Shape Styles and Push All Shape Styles (every colour, line width, brush and fill or outline changed on the copy, at once; hidden shapes stay hidden). Colour swaps (one colour of the original recoloured on every shape using it, the ones added later too) can still be made from Python (layer.reference_swap_color) and are listed under Changes.
  - **Points: Locked / Editable** (the Reference tab, or the lock in the viewport header beside Points / Edges / Strokes; locked by default, so points don't move by accident): moving a reference's points keeps offsets from the original's points, keyed like any edit; the offsets' keys add to the original's motion. **Follow Original's Animation** (under More, on by default); off, only the original's setup shape shows, with the reference's own keys. Handles turned or dragged are kept as offsets too (found by their point and the neighbour each points toward), added to the original's handles the same way. Widths and curve types stay the original's, and points can't be added or deleted (a message says so; Break Reference to change the drawing).
  - **Pose: Same as Original / Own Bones** (Same as Original by default): the reference shows the original's final shape, posed by its bones, smart bones and dynamics, wherever it sits (even outside the bone layer, as exact masks need); the top reference's own transform takes the place of the original's (and follows it until taken over), and everything inside a referenced group follows the original. With Own Bones, it binds to its own bone layer, starting with the original's weights by bone name (or automatic ones); its weights are its own, and Weight Paint changes them without touching the original's. Its Parent Bone is greyed out until it has Own Bones (posed Same as Original, it moves with the original's bones, so a parent bone set earlier waits).
  - **The Reference tab** (on references) holds everything about the link: "Copy of <original>" with Go to Original and how many changes it has, then a dropdown each: Pose, Points and Recolor (open), **Changes on This Copy** (folded to start with: grouped as Colors, Shapes, Points, Layer Animation and Bone Weights, each opening to its items, clicking one picks it in the viewport, with Reset and Push on each item and group) and More (Follow Original's Animation, Remove Unused Changes); Break Reference apart, with a warning. One vocabulary everywhere: **Reset** (back to the original) and **Push** (written into the original, so it and every copy get it; hiding and own weights stay the copy's).
  - **Changes show where they are:** a value the copy changed shows teal where it's edited (the Shape panel's settings, Transform, Opacity, Mean Position, the timeline's rows), as Blender shows overridden properties, and right-clicking it offers Reset to Original and Push to Original. The Layers editor marks a copy with changes with a teal dot, and while a copy is selected its original's row is outlined (or the folded group holding it). Something not allowed on a copy (deleting or adding its points, moving them while locked, changing widths or curve types) is undone, and a yellow warning shows beside the cursor, as in Blender (also when a copy's selected points stay put while other layers' points move, as when editing from a bone layer); the same goes for the viewport's and rigging's other refusals (drawing away from the setup frame, changing the rig away from it, a bone hanging from its own child).
- **References to another file** show a warning when the original has changed. Updating offers Moho's options: add new layers, remove missing layers, replace mismatched bones, replace mismatched drawings.
- **Break reference** turns it into an ordinary layer that keeps what it shows, its offsets added to its points' keys so its animation stays as it was. A reference whose original is deleted does the same.
- **Better than Moho:** each channel stores its override flag explicitly, so a reference never silently gains keys or loses its link. Moho users report both problems ([forum thread](https://lostmarble.net/forum/viewtopic.php?t=37872)).

Sources on Moho's behaviour: the scripting API (`DuplicateLayer` by reference, `UpdateReferencedLayer`, `MohoLayerRefSyncOptions`) and [this forum thread](https://lostmarble.net/forum/viewtopic.php?t=34787).

**Tools.** A tool is an object with mouse, key and tablet callbacks, an overlay draw callback, an options bar, saved preferences, and checks that hide or grey it out by context. That maps directly onto our Python tool class.

## Feature spec

Version 1 covers 67 of the 98 features below: enough to draw, rig with smart bones, animate on a timeline and export frames. The rest are tiered Later or Maybe. Change any tier from its dropdown. "Community" means a feature that Moho users built as add-on scripts, so it becomes native here. Speed is a requirement throughout, not a feature: the viewport should play back at the document frame rate.

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

- **1, 2, 3** switch the selection mode: Points, Edges (clicking a line selects the segment between two points) or Strokes (clicking a line selects the whole curve). In Points mode only points are picked: clicking a line between them selects nothing. With edges selected, the Drawing tab's Edge panel hides or shows their outline, as Moho's Hide Edge does. Clicks pick lines where they show (posed by bones, and a reference matching its original where that is). Curve handles show only in Points mode: Edges and Strokes modes pick lines, so their handles are hidden.
- **L** selects the whole shape under the mouse; **Shift+L** deselects it.
- The **Points, Edges and Strokes** buttons in the header show only when there are points to pick.
- **Shift+Z** over the viewport shows or hides its overlays (bones, wires, points, handles), as the header's Overlays button does.

**Two cursors.** Camera view has a 2D cursor on the canvas and World view has a 3D cursor in the scene. They are separate, and each keeps its own position. New layers, bones and imported meshes are placed at the active view's cursor, and the cursor can be the pivot point.

**Move, rotate, scale.** G moves, R rotates and S scales from any tool, as in Blender. Angles are plain numbers, as in Moho: R counts whole turns while the mouse circles the pivot, and layer rotation and bone angles key any amount, so one key at 720° spins twice. For bones, S on frame 0 resizes them (their joints spread about the pivot, so bones get longer; scale stays 1). Posing, S scales each bone along its own axes: S X its thickness, S Y its length, and S S squashes and stretches (longer and thinner, keeping its area). Bone transforms take X / Y and typed numbers too.

- Then press **X, Y or Z** to lock to that axis; press it again to switch to the other orientation. **Shift+X, Y or Z** locks to the other two axes.
- Type a number for an exact value, hold Ctrl to snap, Enter or click to confirm, Esc or right-click to cancel.
- Points and bones lie flat on their layer, so Z applies to things with depth: layers, the camera, the 3D cursor and meshes.

**Header dropdowns.** The viewport header carries Blender's three transform controls.

- **Transform orientation:** Global, Local (the layer's own axes), Parent (the parent bone), View or Cursor. Comma opens it as a pie menu.
- **Pivot point:** Bounding box centre, Median point, Individual origins, Active element or Cursor. Period opens it as a pie menu.
- **Proportional editing:** points only; the toggle and its dropdown show only while the edited layers have points. O toggles it. The dropdown, as Blender's panel, picks the falloff (Smooth, Sphere, Root, Inverse square, Sharp, Linear, Constant, Random; Shift+O cycles), Connected only (Alt+O: measured along the curves, so only points on a curve with selected points move) and the Proportional Size (0.2 to start). Scroll the mouse wheel (or Page Up / Page Down) during G, R or S to resize the influence circle drawn about what moves. Blender's Projected from View is left out: a drawing is already flat. This replaces Moho's Magnet tool.

**Drawing and vector editing**

| Feature | From | Tier |
| --- | --- | --- |
| Pen, in the Draw tool (right-click it to pick Draw or Pen): click to add points, auto-weld to existing points | Moho | v1 |
| Freehand and brush with tablet pressure setting line width | Moho | v1 |
| Shape tool: box and circle, picked from a toolbar flyout (right-click the tool); Shift keeps sides even, Alt draws from the centre | Both | v1 |
| Select points with box and lasso; transform points | Both | v1 |
| Selection modes on 1, 2, 3 (points, edges, strokes) and L to select a whole shape | Blender | v1 |
| Split Curve tool: click a line to add a point without changing its shape | Community | v1 |
| Knife tool (Ctrl+K): cut across shapes to add internal segments and split fills | Blender | v1 |
| G, R, S modal move, rotate, scale with X/Y/Z axis lock and typed values (see Viewport toolbar) | Blender | v1 |
| E to extrude a new point from an end point | Blender | v1 |
| X on selected points opens the Delete menu, as Blender's: Points, Edges (the segments between selected points go and the points stay, curves splitting there) and Dissolve Points (the curve joins across each gap, so lines and shapes stay whole). The Delete key deletes the selected points at once | Blender | v1 |
| Mirror editing (X and Y in the viewport header): moving a point moves its mirror image across the layer's axis too. Points are paired by where they are in the setup pose (frame 0), so the pairs hold on every frame, and the image snaps to the reflected spot (handles too), making the sides symmetrical on any frame | Blender | v1 |
| Curvature, smooth and peak points | Moho | v1 |
| Create shape, fill and outline, shared styles | Moho | v1 |
| Line width per point, corner type per point (round or sharp joins and caps), hide edge, delete edge | Moho | v1 |
| Stroke sculpting brushes: grab, push, smooth, thickness, strength, with falloff and tablet pressure | Blender | v1 |
| More sculpting brushes: twist, pinch, randomize, clone | Blender | Later |
| Slide point along curve, point reduction | Community | Later |
| Gradients and texture fills | Moho | Later |
| Eraser and scatter brush | Moho | Later |
| Liquid shapes: combine shapes by add, subtract, intersect | Moho | Maybe |
| Pen tablet support through Windows Ink: pressure, tilt and the eraser end | Both | v1 |
| Moho stamp brushes: brush picker, all of Moho's brush settings including line boil, .mohobrush import (see below) | Moho | v1 |

**Brushes.** SkyAnimate copies Moho's stamp brushes: a line can be drawn as a row of image stamps instead of a solid stroke. A brush is part of a line's style, so shared styles share their brush. The stamps stick to the artwork: they are laid out along the line as drawn on the setup frame, each at a place on one of its segments, and kept at that place as points move and bones bend the line, so they stretch and squash with it instead of sliding along it (Moho's slide as a curve stretches; Toon Boom's textured lines are fixed to the line like this). A segment stretched past 1.5 times its length fades in stamps halfway between its own (and again at 3 times, ...), and one squashed below three quarters fades out every other one, so a line never breaks into dots or clots, after Bénard et al.'s self-similar line textures. The layout doesn't depend on the zoom, so the viewport and renders agree.

- **Brush picker dialog**, laid out like Moho's: a grid of brush thumbnails (with None first), multi-stamp brushes marked with three dots, the settings below, a live preview stroke, Save Brush, OK and Cancel. Alt-click a brush to carry the current settings over to it.
- **Brush files:** Moho's `.mohobrush` file is a zip holding a `brush.json` with the settings below plus one PNG stamp, or a folder of PNG stamps for a multi-stamp brush. SkyAnimate reads these files directly and saves its own brushes in the same format.
- **Older brushes:** a lone PNG (or folder of PNGs) with no `.mohobrush` is a brush too. Settings can follow its name as `Name_<align 0|1>_<jitter degrees>_<spacing percent>.png`, as in Moho's `Brush522_1_0_200.png`. Stamps are dark ink on white (darkness is coverage), or colour with alpha for colour brushes.
- **Asset library:** all of the app's brushes live in one asset library folder, set in Preferences (`<library>/library.json` marks it, `<library>/Brushes/` holds the brushes). If none is set, or its folder is gone, SkyAnimate asks for a folder at startup and initializes the library there. It then offers to import Moho's brushes: it copies them from the Moho install (`Resources/Support/Common`, `Debut` and `Pro` brush folders) and from your Moho custom content into the library. Preferences can also import from any folder later. Moho's brushes are Smith Micro's artwork, which is fine for private use, so they are never bundled with SkyAnimate; they only reach the library from your own Moho install.

| Setting | Stored as | What it does |
| --- | --- | --- |
| Align brush with curve | `align` | Stamps rotate to follow the direction of the line |
| Random stamp order | `randomOrder` | Multi-stamp brushes pick stamps at random instead of in sequence |
| Merge alpha | `mergedAlpha` | Overlapping stamps merge into one transparency instead of building up darker |
| Tint brush using stroke color | the style's tint flag | Stamps take the line colour |
| Line boil | `randomize` | Stamps re-randomise over time so the line shimmers like hand-drawn animation |
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
| Masks that name layers: Inside / Outside, several per layer, groups, hidden mask layers, chips in the Layers editor, Ctrl Alt G | After Effects, Blender, Live2D | v1 |
| Blend modes and layer opacity | Both | v1 |
| Multi-layer editing: point tools work on every selected layer at once (see below) | Community | v1 |
| Layer comps | Moho | Later |
| Reference layers: live copies with per-channel overrides and local masks (see Core concepts) | Moho | v1 |
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
| Bone collections, as Blender's (Armature tab > Bone Collections): a list with, per collection, a dot (filled while something of it is selected, hollow while it has members), an eye to show or hide it and a star to solo it; + and -, a menu (Move Up/Down, Show All, Unsolo All, Select All in Collection, Remove Unused Collections), a grip that sizes the list and a name filter; Assign, Remove, Select and Deselect under it; double-click renames. All comes first in every bone layer: it holds every bone and every drawing Editable in Bone Group, can't be renamed or removed, and nothing is assigned to it or removed from it (Assign and Remove refuse while it is active). Bones and editable drawings can each be in several other collections; new bones join the active one. As in Blender, a bone (or editable drawing) shows while any collection holding it shows, so while All shows everything does, and hiding All leaves the other collections' eyes to decide; while any collection is soloed only soloed ones show (soloing All shows everything). Hidden bones can't be picked or moved (hiding deselects them); a hidden drawing loses its points and wires and isn't edited with the bones, its artwork still rendering. A drawing joins the bone layer with Editable in Bone Group (its Layer tab), then Assign or Remove (picked in the Layers editor, or its points selected in the viewport) and M / Shift M move it between collections. M opens Move to Collection (leave the others, join one) and Shift M Assign to Collection (join one, keep the others), each ending in New Collection, over the viewport or the Layers editor: with the bone layer active they take its selected bones and the editable drawings picked or with points selected; with an editable drawing inside it active, that drawing and the other picked ones (not the bones). Subdivide, Duplicate and Symmetrize keep membership; Moho's bone tags convert to collections (Tag 1, Tag 2...). Nested collections: Later | Blender | v1 |
| Add, select, transform and reparent bones as in Blender's armature edit mode: head and tail joints selectable on their own, an active bone, Shift A adds a bone at the origin, E extrudes from the selected tips (connected), Shift D duplicates, Ctrl P parents to the active bone (Connected or Keep Offset), Alt P clears or disconnects. On frame 0, as in Blender's edit mode, only the picked joints move: G on a ball moves that end (the bone stretches), G on the body moves both ends, R and S turn and size picked bones; a child that isn't connected stays where it is, and a connected child's head (its parent's tail) comes along while its tail stays. Away from frame 0, as in pose mode, G on the top ball turns the bone toward it, G on the bottom ball or body moves the whole bone, R turns it about its bottom ball, and children come along. Away from frame 0, G on a connected bone turns it about its base instead of moving it. Away from frame 0, Alt G, Alt R and Alt S put the selected bones' position, angle or scale back to the setup pose. X deletes a bone picked by any part. A dotted line joins a child to its parent. Viewport Overlays > Armature shows bones Off, Selected (only selected bone layers') or On (every bone layer's, the default); bones of a layer you're not working in show as faint outlines. Unless it's Off, the weight tools show their bone layer's bones, with the selected bone solid. Right-click a selected bone for Subdivide (a chain of connected pieces) and the other bone commands. With X mirror editing on, the other side (_L / _R) follows moves, turns, extrudes, subdivides and deletes | Blender | v1 |
| Box-select bones with the Select tool (the ends inside the box; Shift adds, Ctrl removes). A Transform Bones tool in the toolbar, after Moho's sz_transform_bones_gizmo: a box around the selected bones to move (inside), turn (the band around it) and scale (eight handles; Alt from the pivot, Shift keeps proportions), a draggable pivot (Ctrl snaps to bone ends), Position/Scale/Angle fields, Flip H/V and Symmetrize; unselected children stay where they are | Moho, community | v1 |
| Weights instead of Moho's binding: new points get automatic weights (from the nearest weighted points along their curve, else by distance to the bones); a Weight Select tool (select points, Ctrl-click a bone; Automatic Weights, Assign, Remove, Normalize) and the Weight Paint tool (with a Selection Mask); Parent to Bone for whole layers (giving a layer's points weights sets it back to None; a group's Parent Bone moves everything inside it as a whole, overriding their own Parent Bone and weights, which show the group's bone greyed out); a layer's Editable in Bone Group option lets its points be picked, moved and deleted while the bone layer is active, along with the bones (while its bone collections show) (its handles too: pressing on one drags it, as on the layer itself); bone strength sets the automatic reach. Older files' point and flexi binding become the same weights | Blender, Moho | v1 |
| IK with bone locks and angle limits: Auto IK (header toggle) makes posing a bone by its tip turn its parents, up to a bone Ignored by IK, inside Limit Angle; Arc Solver bends a chain evenly; an IK constraint reaches for a target bone (chain length, max stretch) | Moho | v1 |
| Bone display (Armature tab > Viewport Display, any frame), as Blender's: a Shape, Bone, Pin (with a Size, kept on screen whatever the zoom) or Custom (a vector layer, picked from a list, dropped from the Layers editor, or with the eyedropper from the viewport or the Layers editor): the layer's drawing as on the setup frame, where that layer sits at rest, moving with the bone (its hidden edges, the layer's own or a reference's, left out); clicking anywhere on it picks the bone, and a missing layer draws a bone. A Bone Color: Default Colors, Blender's 15 theme colour sets, or a Custom Color Set of three swatches (Regular, Select, Active; choosing it starts from the set the bone had); bones, pins and custom shapes are drawn in their set's regular shade, and its selected or active shade when picked, see-through as bones are. Copy Bone Color to Selected gives the other selected bones the active bone's colours. Custom Bone Shapes in Viewport Overlays (on by default) draws the shapes; off, every bone is drawn as a bone. While bones are moved on the setup frame (G, R, S, a drag or the gizmo), the moving ones are drawn as bones until the move ends, so their ends show. Moho's pin bones (no length) convert as pins. Offsets for custom shapes, and drawing one at another bone: Later | Blender, Moho | v1 |
| Custom bone shapes: draw a bone as any vector shape | Blender | v1 |
| Bone colours and colour-group visibility | Both | v1 |
| Smart bones (smart actions): with one bone selected, the top of the timeline's action menu lists that bone's smart actions and, while it has fewer than two, New Smart Action (an action named after the bone; the second, "<bone> 2", turns the other way); smart actions are listed apart from ordinary ones. Turned past the action's last frame, the bone holds that frame (one drag keeps turning the same way past half a turn). What they add is kept apart from the data, so saving, undo and keying never bake it in | Moho | v1 |
| Local armature viewport: a per-armature canvas for control bones and smart-bone dials (see Core concepts) | SkyAnimate | v1 |
| Symmetrize a rig using \_L and \_R names, including smart bone actions | Blender, community | v1 |
| Controlling bones (Copy Angle / Position / Scale with factor and delay), Independent Angle, Maintain Volume (Moho's Squash & Stretch), as bone constraints | Moho | v1 |
| Bone dynamics (after Spine's physics constraints, Live2D and Cartoon Animator's spring bones rather than Moho): a Dynamics constraint (always last) makes a bone hang like a weighted strand. Angle: when what it hangs from moves or turns, the strand's tip stays behind and swings back (a run of dynamic bones is one strand, thrown off as a piece over its whole length, each bone then aiming from where the one above has swung to, so a wave runs down it). Position: the head lags and jiggles. Squash & Stretch: moving along the bone stretches or squashes its length, and, as an elastic pendulum, its own swing pulls it longer the faster it goes (levelling off, so a whip stretches it most at its fastest point) and gravity and the wind along it stretch or squash it (with Maintain Volume it thins as it stretches). Each channel has Settle Speed (how quickly the swinging dies down, about half a second at 1) and Bounce (how far past it swings, 0 to 90%, 30% by default: forward, back, still); Angle and Position add Inertia (how much movement throws it off; 0% moves rigidly); Squash & Stretch adds Stretch (how stretchy: movement and swing); and each has an optional soft limit, ticked on beside its value (Max Swing, Max Distance, Max Stretch). The bone as a whole has a Weight (how heavy it is: how hard gravity pulls it, down, and along the bone for Squash & Stretch) and a Wind Catch (how much the wind catches it, 100% by default); as in Unreal's AnimDynamics the wind doesn't follow Weight, since it pushes on a surface, not a mass. Moving the bone layer counts as movement. Moho's spring, damping and weight, the earlier Torque, and the earlier Weight of each channel (Angle's becomes the bone's), convert. Simulated from the Simulation start (bones start already bent by gravity and the wind, and already moving with them when keys are changing them) and cached; gravity and wind in the Document tab, or a bone's own with Own Gravity and Own Wind, each ticked on its own (starting as the document's the first time it is ticked, and kept while unticked). The wind isn't steady, as Unity's wind zones: Gusts make it stronger and weaker and waver its direction, sweeping across the drawing like a wave through grass (timed by where each bone sits in the setup pose, so a moving bone doesn't run through them), Gust Speed sets how often they come, and Turbulence adds a small, fast flutter along and across it (from the frame, so repeatable). The wind drags on how fast the air goes past a bone, as Houdini's, Disney's and Unreal's wind do: moving into it a bone is pushed harder, moving with it hardly at all, and strong wind calms swings across it; a still bone bends as before, and without wind there is no air drag (settling is Settle Speed's). Gravity, the wind settings and the Dynamics numbers are animatable, each with its key button; as settings they are keyed by hand (I and auto-keying leave them alone until one is keyed, then edits key it), and the document's show as a Simulation row in the timeline; the simulation feels keyed gravity and wind eased (averaged over about a tenth of a second, never reaching back before the simulation start to the setup frame, and smooth between frames), so a key where a ramp stops doesn't jolt the bones; Bake to keys. With Physics Ghosts on (Viewport Overlays, the default), bones are drawn, picked and edited where their keys put them, so a dynamic bone lands exactly where it is put, and pale blue ghosts show where the dynamics have them (the artwork follows the ghosts); off, bones are drawn where the dynamics have them. With Live Physics on (Viewport Overlays, the default), dynamics run live: while a pose changes on a frame (a bone dragged or turned), the dynamic bones simulate in real time, trailing, swinging and settling as you work, and stay as the live simulation has them until the frame changes (scrubbing or playing shows the frames' own results); it is only shown, so keys, the cache, playback and baking stay repeatable. Collisions and a keyable reset: Later | Spine, Live2D | v1 |
| Smart warp: deform artwork with a warp mesh | Moho | Later |
| Rigid-body physics | Moho | Maybe |
| Copy, paste and paste-flipped poses using \_L and \_R bone names | Blender, community | v1 |
| Weight painting: a Weight Paint tool in the toolbar for layers inside a bone layer, working like Blender's (active bone, weight, radius, strength, Draw/Add/Subtract, normalize; Ctrl subtracts, Ctrl-click picks the bone; curve handles are hidden while painting or selecting weights) | Blender | v1 |
| Pose library: save poses and expressions as thumbnails, apply with one click | Blender | Later |
| Setup toggle: jump to frame 0 and back, with clear on-screen signals and rigging tools limited to frame 0 (see Core concepts) | Moho, Blender | v1 |
| Bone constraints in a Bone Constraints tab beside Armature (the active bone's, while it is selected; with none selected it asks for one), as Blender's stack (add, reorder, on/off, remove, animatable Influence with its key button): Limit Angle, Independent Angle, Maintain Volume (the width follows the length however it got there, keeping the area), Copy Angle / Position / Scale, Track To, Stretch To, IK, Dynamics. They act on posed frames, never on the setup frame. Limit Angle shows, as Moho does, a red line from a selected bone's base at its smallest angle and one at its largest. Dynamics, last in the stack, swings from where the constraints above it put the bone: a Limit Angle limits its aim, not its swing | Blender, Moho | v1 |

**Animation and timeline**

| Feature | From | Tier |
| --- | --- | --- |
| Animated channels with every Moho interpolation mode, holds and on-twos | Moho | v1 |
| Timeline and dope sheet: select, move, scale, copy and paste keys (pasted on frame 0, the first copied key becomes the setup pose). Channel rows fold, as Blender's do, down to one row per property, in four groups: Pinned at the top (red: properties pinned with the pin button beside their key button in Properties, listed whatever is selected, even with no keys left, each with its pin to unpin it; View > Clear Pins unpins all); Props (the settings: a layer's own transform, opacity and visibility, the bones' constraint settings, the document's Simulation settings); All (the motion: the bones' angle, position and scale, and the points); and Selected (red: the motion of the selected bones and points). Selected starts open with its Bones folded (as All's are). Inside them, View > Group by Kind (the default) lists by what they are (Layers, Constraints, Simulation; Bone Angle and Bone Transform with a row per bone), and Group by Owner, as Blender's Dope Sheet, lists each layer, its bones and each bone's rows. The names column's edge drags to widen it; long names end in an ellipsis and show whole on hover. Rows without keys are hidden (pinned ones aside); a folding row shows the keys of all it holds. Tab goes to the setup frame (0) and back to the frame you came from; with the mouse over the viewport it always does, never hopping between text fields (one being edited lets go). Click a layer's name to make it active; click or box-select keys in any rows, each row picking only its own kind of key. Ctrl C / Ctrl V copy and paste keys at the current frame, onto the same layers' channels, even into another action. Keyed Here: Ctrl-click (or right-click) a key for every property keyed on that frame under its row, by owner, with its value and interpolation (points grouped by kind, each group listing its points; held still over a key, the same list shows as a tooltip); hovering an item outlines its bone or points in the viewport, and choosing one jumps to that frame (Alt stays) and selects its bone, or its points or handles (staying in the layer worked in when it can edit them, else going to theirs; Shift adds), or opens it in the Properties editor, its panel open and its row flashing blue for a second; Select These Keys picks them | Blender | v1 |
| Animated draw order: per-layer In Front Of / Behind rules, keyed, across groups and bone layers, masks kept; Ctrl ] / Ctrl [, In Front Of... with a live preview, the Layers editor's Order view and markers, labelled timeline bars | Rive, Cartoon Animator, Spine, Harmony | v1 |
| Actions and the mainline; insert an action as a copy or reference | Moho | v1 |
| Action dropdown in the timeline header: Mainline by default, or any action of the bone layer you're in; a + beside it names a new action and starts editing it; right-clicking an action deletes it; the timeline turns orange while editing an action | Moho | v1 |
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
| Mirror modifier for drawings; bone layers get Blender's Symmetrize instead (Armature menu and the Bones panel), not a mirror modifier | Blender | v1 |
| Noise, smooth, offset, thickness and simplify modifiers | Blender | Later |
| Array, build, dash, outline and envelope modifiers | Blender | Later |

**Armature modifier.** The bones moving a drawing are a modifier too: every layer inside a bone layer has one Armature modifier, added and removed with the bone layer and never deleted by hand. Its place in the stack decides what the bones move: a Mirror modifier above it mirrors the setup drawing, so each half follows its own side's bones (a point bound or weighted to `arm_L` mirrors to `arm_R`); below it, the posed drawing is mirrored.

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
| Menus that open at the mouse (right-click menus, X's Delete menu, Ctrl P, Alt P) remember the item last chosen and open with it under the mouse (the first item before any choice), as in Blender | Blender | v1 |
| Workspace tabs: Layout, Drawing, Rigging, Animation, Render, Scripting; text editor and info log for Scripting | Blender | v1 |
| Autosave and crash recovery; starting without a file reopens the file that was open at quitting (as last saved; File > Recover > Last Session has any unsaved changes). A crash writes a report to %LOCALAPPDATA%\SkyAnimate\crashes: a minidump (crash_<time>.dmp, for Visual Studio or WinDbg with the build's .pdb) and a readable crash_<time>.txt (the reason, the bad address, the call stack with function names and source lines); the next start says where it is in the Info editor. sky.wm.debug_crash() crashes on purpose to try it | Blender | v1 |
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
| Render | Render Frame (Ctrl+R or F12, shown in a Render Result window with Save As PNG), Render Animation (Ctrl+F12), View Render (F11), Output Settings |
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

**Properties editor.** Like Blender's Properties editor: a column of tab icons down the side, each tab showing the active layer's settings as collapsible panels. Every animatable field has a key button beside it, as in Blender: a dot when not animated, a hollow diamond when animated, a filled one when keyed on this frame; clicking it keys the property here, or takes the key away. Once a property is animated, a pin button beside it pins it to the timeline (red when pinned, and its row tinted red). The field is tinted green when animated and yellow when keyed on this frame. So far: the layer's Visible, Location, Rotation, Scale and Opacity, and the bone's Angle and Scale. Visibility is an animated channel whose keys can only be constant (yes/no properties only hold) that auto-key leaves alone until it is keyed by hand. As in Blender, a property whose last real key is deleted isn't animated any more: its frame-0 key goes too, and it keeps frame 0's value.

| Tab | What it shows | What you can do there |
| --- | --- | --- |
| Layer | Name, type, visibility and lock. Transform: position, scale, rotation X/Y/Z, shear, flip, origin. Opacity, blend mode, masks, parent bone, timing offset | Type exact values; set or clear keys per field; reset the transform; jump to the parent bone |
| Drawing | Every shape in the layer, top to bottom in stacking order, with its fill and outline swatches. Below the list: the selected shape's fill, outline, line width and style. Point and curve counts for the layer | Select a shape from the list or the canvas and they stay in sync; rename, hide, reorder by dragging, delete; change fill and outline; assign a shared style |
| Modifiers | The layer's modifier stack, top to bottom, each as a collapsible panel with its own settings | Add from a menu; reorder by dragging; show or hide in the viewport and in renders; apply, duplicate or remove |
| Armature (bone layers) | The bones as a tree, and the active bone: name, parent, IK settings, strength, shape. On the setup frame (0), as Blender's edit mode: its Head, Tail and Length. On other frames, as Blender's pose mode: its Transform, Angle and Scale, each with its key button; Transform and Angle are the change from the setup pose, (0, 0) and 0° at rest, the Transform measured along the bone's rest axes (X along it as it rests, Y across it, as Blender's pose location) (their keys hold these changes, as Blender's pose bones do, unlike Moho's: moving a bone on frame 0 moves every pose and action with it; the timeline names the row Transform too; older files and Moho imports open converted) | Select a bone from the tree; type exact ends on frame 0 (a connected bone's head moves its parent's tail too) or exact pose values elsewhere; set or clear keys per field |

The tabs shown depend on the layer type: a bone layer gets a Bone tab, an image layer an Image tab, and the Drawing tab appears only for vector layers. Panels are written in Python, so add-ons can add their own panels to any tab.

**Built-in MCP server.** SkyAnimate can be driven by AI assistants such as Claude through the Model Context Protocol, so they can look at and work in the running app. Moho has no such thing, which is why MohoMCP has to poll for requests inside a tool's draw call; here it is part of the app. The first version is deliberately thin while the API is still changing.

- **Built on what already exists.** Everything goes through the `sky` Python API and its operators, so anything a person can do, the server can do, with the same undo steps.
- **Tools now:** run Python with the `sky` module (as a console: names stay between calls, the last line's value, printed output and tracebacks come back); a screenshot of the window or one editor; the app's state (file, frame, playback, layers, selection, a running transform, recent Info reports); and pressing keys as if over an editor (typed numbers go into a running transform). Later, once the API settles: typed tools to read the document, layer tree, bones, actions and keyframes, get and set properties by path, run operators by name, set and delete keys, switch frames and actions, render a frame, and run several operations as one undo step.
- **Resources (later):** the `sky` Python API reference and the operator list, so an assistant can discover what is available.
- **Transport:** JSON lines on 127.0.0.1 only (`src/wm/remote.cc`), run on the app's main loop between frames (screenshots from the frame just drawn), so no polling; a stdio bridge in plain Python using only the standard library (`tools/mcp/skyanimate_mcp.py`) speaks MCP, registered for this project by its `.mcp.json`.
- **Safety:** off by default and switched on in Preferences (Remote Control); every request needs a token made afresh each time it starts (written with the port to remote.json in the user data folder, where the bridge reads it, and removed when it stops); a connection that doesn't open with a request, as a browser's doesn't, is closed unread. Later: a separate switch for running Python and a status-bar indicator while an assistant is connected.
- **How it is built:** the listener in C++, the bridge in plain Python outside the app (no MCP SDK needed: MCP's stdio protocol is small), tested end to end headless (`tests/test_remote_control.py`).

**Files and output**

| Feature | From | Tier |
| --- | --- | --- |
| Save and open the native file format | Both | v1 |
| Export PNG image sequences | Both | v1 |
| Export video through FFmpeg | Both | Later |
| Import SVG artwork | Moho | Later |
| Export GIF and glTF | Both | Maybe |
| Render settings: resolution, frame range, transparent background; viewport and render quality (stroke resolution: pieces drawn per curve segment) and viewport and render samples (multisampled anti-aliasing, 1 to 16 per pixel; default 4 and 16), set per document. Masks are per-sample stencil masks, so masked edges leave no seams | Both | v1 |

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
- **UI:** Dear ImGui with docking wherever it can do the job, themed with Blender's default colours, font and spacing. Custom widgets built on ImGui cover what it lacks or draws differently: Blender-style number fields (a click without dragging starts typing, a drag changes the value, Shift while dragging is precise), pie menus, tabs and icons. Panels are written in Python with a `layout.prop(...)` style API so the UI toolkit can be swapped later.
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
2. **Drawing:** vector layers (points, curves, shapes, styles, brushes) and the curve renderer; pen, freehand, select, G R S E, curvature, fill; save and open; Layers editor, group and reference layers, masks; snapping, autosave, crash recovery, Save Incremental, pen tablet support.
   *Gate: draw, fill and save a character that matches the Moho look.*
3. **Animation:** animated channels, interpolation modes, timeline and dope sheet; switch and image layers, onion skins, PNG export; frame-by-frame drawing, auto-keying, playback controls, render settings.
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
| Reference layers lose their link or gain stray keys | Advanced masking depends on references staying in sync; Moho users report both bugs | Store a per-channel override flag; test sync, override and break on nested groups in milestone 2 |

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
