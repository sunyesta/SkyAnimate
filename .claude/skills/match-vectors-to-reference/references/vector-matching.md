# Head turns, Bézier shapes and judging a match

## How a head turns

- Think of the head as a sphere with a centre line (brow, nose, mouth, chin) curving over its front. As it
  turns, the centre line slides toward the turning side and curves more; features ride that line.
- Estimate the angle from landmarks before reshaping anything: the nose's offset from the face's middle,
  how much of the far eye shows, where the near ear sits. Every part should agree on one angle.
- **Far side narrows.** The far eye, brow and cheek compress horizontally (foreshortening); the far eye
  turns into a sliver and disappears behind the face's edge at about 60-70 degrees. In Clay the head's
  mask does the disappearing: move the eye inward; don't shrink it to nothing.
- **Nose.** Past about 45 degrees the nose breaks the far cheek's silhouette: the head outline runs behind
  the nose (seed the nose and skin as one region for the head). The nose's own shape turns from a front
  view (symmetric) to a profile (tip and underside).
- **Near side widens**, the near cheek bulges toward the edge, and the **near ear slides inward** toward
  the face's middle and shows more of its front.
- **Mouth** wraps around the muzzle: its far end shortens and curves away, the near end lengthens.
- Jaw and chin follow the sphere and the neck stays put: the jaw line meets the neck where it did.

## Placing points

- Points belong at extrema (top, bottom, leftmost, rightmost of a curve), inflections (where the bend
  changes direction) and corners. Not in the middle of a smooth run, not two close together.
- **Lowest detail level first.** A shape with detail levels gets its sweep from its level-0 points; the
  knife's detail points ride that and only add bumps and notches. Fit level 0, then each finer level.
- **Never add points**; give each existing point a role in the new shape and slide it there. A point that
  was the chin stays the chin. If a shape needs a corner where there's a smooth point, break that one
  (`--corner`); if it needs a smooth bend where there's a corner, keep its handles in line.
- Smooth points' handles stay collinear (the in handle opposite the out handle). Lengths can differ:
  asymmetric handles are how one point serves a gentle side and a tight side.

## Handles

- Each handle about 1/3 of its segment's chord; a quarter circle is 0.552 x radius. Longer than 3/4 of the
  chord bulges or loops; crossing handles kink.
- Handles point along the tangent at their point: at an extremum they're horizontal or vertical.
- Fewer, well-placed points with good handles read cleaner than many: Clay's head is 10 points.

## Masks

- A layer masked inside another is cut by that one's outline: edit the mask layer for that edge. Leave the
  overlap under the mask; the cut makes the edge.
- A layer with `everything but X` shows outside X only: its edge inside X is invisible, don't fit it.
- Lines with "ending at the edge" stop where the mask cuts them.
- Judge the composite render, never one layer's wire alone.

## Judging a match

- Silhouette first: IoU of a part's render against the reference region (`vm.py score`), then the
  outline error in pixels. Landmarks second: eye centres, nose tip, mouth corners, ear tip, chin.
- Stop at about 1-2% of the head's height (Clay's head is ~400 px tall at 1920x1080: 4-8 px) or when a
  pass stops improving. Chasing the reference's brush wobble makes worse shapes.
- Look at the overlay at full size around each part; a number can hide a wrong role (a point on the
  wrong side of a notch).
