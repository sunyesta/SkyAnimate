#include "anim/controller_preview.hh"

#include <memory>

#include "anim/animation.hh"
#include "anim/controller.hh"
#include "data/bones.hh"

namespace oanim {

namespace {
uint64_t g_seen_revision = 0, g_generation = 0;
}

uint64_t controller_document_generation()
{
  if (revision_latest() != g_seen_revision) {
    g_seen_revision = revision_latest();
    g_generation++;
  }
  return g_generation;
}

Document controller_posed_copy(const Document &doc, uint64_t armature, uint64_t bone, float2 at)
{
  controller_document_generation();
  Document copy = doc;
  if (copy.frame_current == 0) {
    copy.frame_current = 1;
    animation_evaluate(copy, 1);
  }
  Layer *owner = layer_find(copy, armature);
  Bone *knob = owner ? bone_find(*owner, bone) : nullptr;
  if (knob && bone_is_controller(*knob)) {
    controller_knob_put(*knob, at);
    animation_refresh_action_bones(copy);
  }
  /* What making it gave its layers isn't the document changing. */
  g_seen_revision = revision_latest();
  return copy;
}

namespace {

struct Preview {
  bool active = false;
  uint64_t armature = 0, bone = 0;
  float2 at;
  /* What it was made from: the document's generation then, and the frame. */
  uint64_t made_generation = 0;
  int made_frame = -1;
  uint64_t stamp = 0;
  std::unique_ptr<Document> copy;
};

Preview g_preview;
uint64_t g_stamps = 0;

}  // namespace

void controller_preview_set(uint64_t armature, uint64_t bone, float2 at)
{
  if (g_preview.active && g_preview.armature == armature && g_preview.bone == bone && g_preview.at.x == at.x &&
      g_preview.at.y == at.y)
  {
    return;
  }
  g_preview.active = true;
  g_preview.armature = armature;
  g_preview.bone = bone;
  g_preview.at = at;
  g_preview.copy.reset();
  g_preview.stamp = ++g_stamps;
}

void controller_preview_clear()
{
  if (g_preview.active) {
    g_preview = Preview();
    g_stamps++;
  }
}

bool controller_preview_active(uint64_t *r_armature, uint64_t *r_bone, float2 *r_at)
{
  if (r_armature) {
    *r_armature = g_preview.armature;
  }
  if (r_bone) {
    *r_bone = g_preview.bone;
  }
  if (r_at) {
    *r_at = g_preview.at;
  }
  return g_preview.active;
}

const Document *controller_preview_document(const Document &doc)
{
  if (!g_preview.active) {
    return nullptr;
  }
  /* Made again once the document changes, or the frame. */
  const uint64_t generation = controller_document_generation();
  if (!g_preview.copy || doc.frame_current != g_preview.made_frame || generation != g_preview.made_generation) {
    g_preview.copy = std::make_unique<Document>(controller_posed_copy(doc, g_preview.armature, g_preview.bone, g_preview.at));
    g_preview.made_frame = doc.frame_current;
    g_preview.made_generation = controller_document_generation();
    g_preview.stamp = ++g_stamps;
  }
  return g_preview.copy.get();
}

uint64_t controller_preview_stamp()
{
  return g_preview.active ? g_preview.stamp : 0;
}

}  // namespace oanim
