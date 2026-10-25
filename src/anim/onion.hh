#pragma once

/* Onion skins: the drawings being edited as they are on frames before and after the shown one, drawn
 * as wireframes over it (Document::onion_*). Before ghosts take one colour, after ghosts another, each
 * fainter the further it is (Fade). Keyframes mode steps by the keys of those layers and of the bone
 * layers and groups above them (a key on the frame shown doesn't count); Frames mode by frames,
 * every `onion_step`. Ghosts stay in the animation's range (frame 0 is the setup, never a ghost). */

#include <cstdint>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct Document;
struct Layer;

struct OnionGhost {
  int frame = 0;
  /* After the shown frame (else before). */
  bool after = false;
  /* 1 for the nearest on its side. */
  int distance = 1;
};

/* The layers whose ghosts show: the drawings among `editing` (the layers being edited) and those inside
 * a picked bone layer or group, or with All Layers every drawing shown. */
std::vector<uint64_t> onion_layers(const Document &doc, const std::vector<Layer *> &editing);
/* The frames ghosts show on for `layers`, nearest first on each side. Empty with onion skins off. */
std::vector<OnionGhost> onion_ghosts(const Document &doc, const std::vector<uint64_t> &layers);

/* One ghost's wires: the curves of `layers` at its frame, each a polyline in document space. */
struct OnionWires {
  OnionGhost ghost;
  std::vector<std::vector<float2>> lines;
};
/* Works the ghosts' frames out on a copy of the document (the document itself is left as it is). */
std::vector<OnionWires> onion_wires(const Document &doc, const std::vector<uint64_t> &layers,
                                    const std::vector<OnionGhost> &ghosts);
/* How strongly a ghost shows (0-1): fainter further away with Fade. */
float onion_ghost_alpha(const Document &doc, const OnionGhost &ghost);

}  // namespace oanim
