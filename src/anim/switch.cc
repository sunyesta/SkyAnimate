#include "anim/switch.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "anim/draw_order.hh"
#include "data/document.hh"

namespace oanim {

/* Values are layer ids, small whole numbers that floats hold exactly (as Draw Order's). */
static uint64_t value_id(float value)
{
  return value > 0.5f ? uint64_t(std::llround(value)) : 0;
}

static bool is_rig(float value)
{
  return value < -0.5f;
}

const Layer *switch_child(const Layer &sw, float value)
{
  const uint64_t id = value_id(value);
  if (!id) {
    return nullptr;
  }
  for (const Layer &child : sw.children) {
    if (child.id == id) {
      return &child;
    }
  }
  return nullptr;
}

float switch_default(const Layer &sw)
{
  const Channel<float> &ch = sw.switch_keys;
  if (ch.animated()) {
    return ch.evaluate(0);
  }
  /* Not animated: the plain value is the data, unless an edit not yet recorded changed it. */
  return ch.has_last ? ch.last : sw.switch_value;
}

std::string switch_value_label(const Layer &sw, float value)
{
  if (is_rig(value)) {
    return "Rig";
  }
  const uint64_t id = value_id(value);
  if (!id) {
    return "None";
  }
  if (const Layer *child = switch_child(sw, value)) {
    return child->name;
  }
  return std::to_string(id) + " (missing)";
}

bool switch_value_from_name(const Layer &sw, const std::string &name, float *r_value)
{
  if (name.empty() || name == "NONE") {
    *r_value = 0.0f;
    return true;
  }
  if (name == "RIG") {
    *r_value = SWITCH_RIG;
    return true;
  }
  for (const Layer &child : sw.children) {
    if (child.name == name) {
      *r_value = float(child.id);
      return true;
    }
  }
  return false;
}

/* The hand key holding at `frame` (after frame 0), unless it is a Rig key; false when there is none. */
static bool hand_value(const Layer &sw, int frame, float *r_value)
{
  const Key<float> *held = nullptr;
  for (const Key<float> &k : sw.switch_keys.keys) {
    if (k.frame > frame) {
      break;
    }
    if (k.frame > 0) {
      held = &k;
    }
  }
  if (!held || is_rig(held->value)) {
    return false;
  }
  *r_value = held->value;
  return true;
}

/* --- Switches linked by name ---------------------------------------------------------------------- */

SwitchLink *switch_link_of(Document &doc, const Layer &sw)
{
  if (!sw.switch_link) {
    return nullptr;
  }
  for (SwitchLink &l : doc.switch_links) {
    if (l.id == sw.switch_link) {
      return &l;
    }
  }
  return nullptr;
}

const SwitchLink *switch_link_of(const Document &doc, const Layer &sw)
{
  return switch_link_of(const_cast<Document &>(doc), sw);
}

std::vector<Layer *> switch_link_members(Document &doc, uint64_t link)
{
  std::vector<Layer *> members;
  const auto walk = [&](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &l : layers) {
      if (l.is_switch() && l.switch_link == link) {
        members.push_back(&l);
      }
      self(self, l.children);
    }
  };
  if (link) {
    walk(walk, doc.layers);
  }
  return members;
}

static bool answers(const Layer &child, const std::string &name)
{
  if (child.name == name) {
    return true;
  }
  return std::find(child.switch_aliases.begin(), child.switch_aliases.end(), name) != child.switch_aliases.end();
}

/* The first child of `sw` answering to `name`: by its name first, then its aliases. */
static const Layer *child_answering(const Layer &sw, const std::string &name)
{
  if (name.empty()) {
    return nullptr;
  }
  for (const Layer &c : sw.children) {
    if (c.name == name) {
      return &c;
    }
  }
  for (const Layer &c : sw.children) {
    if (answers(c, name)) {
      return &c;
    }
  }
  return nullptr;
}

static std::string entry_name(const SwitchLink &link, float value)
{
  const uint64_t id = value_id(value);
  for (const SwitchLinkName &n : link.names) {
    if (n.id == id) {
      return n.name;
    }
  }
  return "";
}

static uint64_t entry_for(SwitchLink &link, const std::string &name)
{
  for (const SwitchLinkName &n : link.names) {
    if (n.name == name) {
      return n.id;
    }
  }
  link.names.push_back({link.next_name++, name});
  return link.names.back().id;
}

/* A child id of `member` (None, Rig as they are) as the group's entry for its name. */
static float to_entry(SwitchLink &link, const Layer &member, float value)
{
  if (is_rig(value) || !value_id(value)) {
    return value;
  }
  const Layer *child = switch_child(member, value);
  return child ? float(entry_for(link, child->name)) : 0.0f;
}

/* An entry as `member`'s child id, 0 when it has none answering to it (Rig as it is). */
static float to_child(const SwitchLink &link, const Layer &member, float value)
{
  if (is_rig(value) || !value_id(value)) {
    return value;
  }
  const Layer *child = child_answering(member, entry_name(link, value));
  return child ? float(child->id) : 0.0f;
}

/* What `member` shows for entry `entry` keyed by `lead` at `frame`: its child answering to it; else the
 * last one before it that it has (the latest key at or before `frame`, then the default); else its
 * own setup child (its first, for the lead). `r_missing`: the name it lacks, or "". */
static const Layer *linked_child(const SwitchLink &link, const Layer &member, const Layer &lead, float entry, int frame,
                                 std::string *r_missing)
{
  r_missing->clear();
  if (!value_id(entry)) {
    return nullptr;
  }
  const std::string name = entry_name(link, entry);
  if (const Layer *child = child_answering(member, name)) {
    return child;
  }
  *r_missing = name;
  const std::vector<Key<float>> &keys = lead.switch_keys.keys;
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
    if (it->frame > frame) {
      continue;
    }
    if (const Layer *child = child_answering(member, entry_name(link, it->value))) {
      return child;
    }
  }
  if (const Layer *child = child_answering(member, entry_name(link, switch_default(lead)))) {
    return child;
  }
  if (&member != &lead) {
    if (const Layer *child = switch_child(member, member.switch_value)) {
      return child;
    }
  }
  return member.children.empty() ? nullptr : &member.children.front();
}

static Layer *lead_of(Document &doc, const Layer &sw, SwitchLink **r_link = nullptr)
{
  SwitchLink *link = switch_link_of(doc, sw);
  Layer *lead = link ? layer_find(doc, link->lead) : nullptr;
  if (r_link) {
    *r_link = lead ? link : nullptr;
  }
  return lead && lead->is_switch() ? lead : nullptr;
}

std::string switch_key_label(const Document &doc, const Layer &sw, float value)
{
  SwitchLink *link = nullptr;
  if (!lead_of(const_cast<Document &>(doc), sw, &link)) {
    return switch_value_label(sw, value);
  }
  if (is_rig(value)) {
    return "Rig";
  }
  if (!value_id(value)) {
    return "None";
  }
  const std::string name = entry_name(*link, value);
  return name.empty() ? "(unknown)" : name;
}

bool switch_key_missing(const Document &doc, const Layer &sw, float value)
{
  SwitchLink *link = nullptr;
  if (!lead_of(const_cast<Document &>(doc), sw, &link)) {
    return value_id(value) && !switch_child(sw, value);
  }
  return value_id(value) && !child_answering(sw, entry_name(*link, value));
}

std::string switch_key_held(const Document &doc, const Layer &sw, float value, int frame)
{
  SwitchLink *link = nullptr;
  const Layer *lead = lead_of(const_cast<Document &>(doc), sw, &link);
  if (!lead) {
    return "";
  }
  std::string missing;
  const Layer *child = linked_child(*link, sw, *lead, value, frame, &missing);
  return missing.empty() || !child ? "" : child->name;
}

uint64_t switch_default_child(const Document &doc, const Layer &sw)
{
  SwitchLink *link = nullptr;
  const Layer *lead = lead_of(const_cast<Document &>(doc), sw, &link);
  if (!lead) {
    const Layer *child = switch_child(sw, switch_default(sw));
    return child ? child->id : 0;
  }
  std::string missing;
  const Layer *child = linked_child(*link, sw, *lead, switch_default(*lead), 0, &missing);
  return child ? child->id : 0;
}

uint64_t switch_child_at(const Document &doc, const Layer &sw, int frame)
{
  SwitchLink *link = nullptr;
  const Layer *lead = lead_of(const_cast<Document &>(doc), sw, &link);
  const Layer &keyed = lead ? *lead : sw;
  float value = 0.0f;
  if (!hand_value(keyed, frame, &value)) {
    value = switch_default(keyed);
  }
  if (!lead) {
    const Layer *child = switch_child(sw, value);
    return child ? child->id : 0;
  }
  std::string missing;
  const Layer *child = linked_child(*link, sw, *lead, value, frame, &missing);
  return child ? child->id : 0;
}

/* Every value a switch's channel holds, `f` given each to change. */
template<typename F> static void channel_values(Layer &sw, F &&f)
{
  f(sw.switch_value);
  f(sw.switch_keys.last);
  for (Key<float> &k : sw.switch_keys.keys) {
    f(k.value);
  }
  for (auto &[name, keys] : sw.switch_keys.actions) {
    for (Key<float> &k : keys) {
      f(k.value);
    }
  }
}

/* Its timeline's keys left out (the lead holds the group's); its actions' keys stay. */
static int drop_mainline(Layer &sw)
{
  const int n = int(sw.switch_keys.keys.size());
  sw.switch_keys.keys.clear();
  return n;
}

/* Joins `sw` to `link`: its values become entries. Its own timeline's keys go to the lead when the lead
 * has none (`r_dropped`: how many went instead). */
static void link_join(Document &doc, SwitchLink &link, Layer &sw, int *r_dropped)
{
  Layer *lead = layer_find(doc, link.lead);
  const float floor = sw.switch_value;
  channel_values(sw, [&](float &v) { v = to_entry(link, sw, v); });
  sw.switch_link = link.id;
  sw.switch_link_off = false;
  if (lead && lead != &sw) {
    if (!lead->switch_keys.animated() && sw.switch_keys.animated()) {
      lead->switch_keys.keys = sw.switch_keys.keys;
      lead->switch_value = sw.switch_value;
      lead->switch_keys.last = sw.switch_keys.last;
      lead->switch_keys.has_last = true;
      sw.switch_keys.keys.clear();
    }
    else if (r_dropped) {
      *r_dropped += drop_mainline(sw);
    }
    /* Not the lead: its plain value is its own setup child, the floor when no name matches. */
    sw.switch_value = floor;
    sw.switch_keys.last = floor;
  }
}

uint64_t switch_link_create(Document &doc, const std::vector<Layer *> &members, Layer *keep_from, int *r_dropped)
{
  if (members.size() < 2) {
    return 0;
  }
  SwitchLink link;
  link.id = doc.next_switch_link++;
  Layer *lead = keep_from ? keep_from : members.front();
  for (Layer *m : members) {
    if (!keep_from && m->switch_keys.animated()) {
      lead = m;
      break;
    }
  }
  link.lead = lead->id;
  doc.switch_links.push_back(link);
  SwitchLink &placed = doc.switch_links.back();
  int dropped = 0;
  link_join(doc, placed, *lead, nullptr);
  for (Layer *m : members) {
    if (m != lead) {
      link_join(doc, placed, *m, &dropped);
    }
  }
  if (r_dropped) {
    *r_dropped = dropped;
  }
  return placed.id;
}

/* `sw` on its own again, showing what it showed: its keys the group's, as its own children. */
static void link_leave(Document &doc, SwitchLink &link, Layer &sw, bool by_hand)
{
  Layer *lead = layer_find(doc, link.lead);
  if (!lead) {
    sw.switch_link = 0;
    return;
  }
  if (lead == &sw) {
    /* The keys go to another member first. */
    for (Layer *m : switch_link_members(doc, link.id)) {
      if (m != &sw) {
        m->switch_keys.keys = sw.switch_keys.keys;
        const float floor = m->switch_value;
        m->switch_value = sw.switch_value;
        m->switch_keys.last = sw.switch_value;
        link.lead = m->id;
        (void)floor;
        break;
      }
    }
  }
  const Layer &keys_from = lead == &sw ? sw : *lead;
  /* Its own keys: what it showed at each of the group's keys, holds included. */
  std::vector<Key<float>> own;
  for (const Key<float> &k : keys_from.switch_keys.keys) {
    Key<float> mine = k;
    if (!is_rig(k.value) && value_id(k.value)) {
      std::string missing;
      const Layer *child = linked_child(link, sw, keys_from, k.value, k.frame, &missing);
      mine.value = child ? float(child->id) : 0.0f;
    }
    own.push_back(mine);
  }
  std::string missing;
  const Layer *def = linked_child(link, sw, keys_from, switch_default(keys_from), 0, &missing);
  for (auto &[name, keys] : sw.switch_keys.actions) {
    for (Key<float> &k : keys) {
      k.value = to_child(link, sw, k.value);
    }
  }
  sw.switch_keys.keys = own;
  sw.switch_value = def ? float(def->id) : 0.0f;
  if (sw.switch_keys.animated()) {
    sw.switch_value = sw.switch_keys.evaluate(doc.frame_current);
  }
  sw.switch_keys.last = sw.switch_value;
  sw.switch_keys.has_last = true;
  sw.switch_link = 0;
  sw.switch_link_off = by_hand;
}

bool switch_unlink(Document &doc, Layer &sw)
{
  SwitchLink *link = switch_link_of(doc, sw);
  if (!link) {
    return false;
  }
  const uint64_t id = link->id;
  link_leave(doc, *link, sw, true);
  std::vector<Layer *> rest = switch_link_members(doc, id);
  if (rest.size() < 2) {
    for (Layer *m : rest) {
      link_leave(doc, *switch_link_of(doc, *m), *m, false);
    }
    std::erase_if(doc.switch_links, [&](const SwitchLink &l) { return l.id == id; });
  }
  switches_resolve(doc, doc.frame_current);
  return true;
}

void switch_links_before_remove(Document &doc, uint64_t layer)
{
  const Layer *removed = layer_find(doc, layer);
  if (!removed || doc.switch_links.empty()) {
    return;
  }
  for (SwitchLink &link : doc.switch_links) {
    Layer *lead = layer_find(doc, link.lead);
    if (!lead || !layer_inside(doc, lead->id, layer)) {
      continue;
    }
    /* A lead going: another member takes the group's keys. */
    for (Layer *m : switch_link_members(doc, link.id)) {
      if (!layer_inside(doc, m->id, layer)) {
        m->switch_keys.keys = lead->switch_keys.keys;
        m->switch_value = lead->switch_value;
        m->switch_keys.last = lead->switch_value;
        link.lead = m->id;
        break;
      }
    }
  }
}

std::vector<std::string> switch_link_lacks(const Document &doc, const Layer &sw)
{
  std::vector<std::string> lacks;
  SwitchLink *link = nullptr;
  const Layer *lead = lead_of(const_cast<Document &>(doc), sw, &link);
  if (!lead) {
    return lacks;
  }
  const auto check = [&](float v) {
    const std::string name = value_id(v) ? entry_name(*link, v) : "";
    if (!name.empty() && !child_answering(sw, name) && std::find(lacks.begin(), lacks.end(), name) == lacks.end()) {
      lacks.push_back(name);
    }
  };
  check(switch_default(*lead));
  for (const Key<float> &k : lead->switch_keys.keys) {
    check(k.value);
  }
  return lacks;
}

std::string switch_name_stem(const std::string &name)
{
  /* "Happy.001", "Happy copy", "Happy_copy 2", "happy.png": the same name to a person. */
  std::string s = name;
  for (char &c : s) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  const auto strip = [&](const std::string &end) {
    if (s.size() > end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0) {
      s.erase(s.size() - end.size());
      return true;
    }
    return false;
  };
  for (bool again = true; again;) {
    again = false;
    while (!s.empty() && (std::isdigit(static_cast<unsigned char>(s.back())) || s.back() == ' ')) {
      const size_t before = s.size();
      while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
      }
      if (!s.empty() && (s.back() == '.' || s.back() == ' ' || s.back() == '_')) {
        s.pop_back();
      }
      again = again || s.size() != before;
      if (s.size() == before) {
        break;
      }
    }
    again = strip(" copy") || strip("_copy") || strip(".png") || strip(".svg") || again;
  }
  return s;
}

static bool g_rename_here = false;

void switch_rename_here_next()
{
  g_rename_here = true;
}

/* The place of `sw` inside `child` (names down from it, "Face/Mouth"), for linking same-named switches. */
static std::string place_inside(const Document &doc, const Layer &child, const Layer &sw)
{
  std::string place;
  for (const Layer *l = &sw; l && l->id != child.id; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    place = place.empty() ? l->name : l->name + "/" + place;
  }
  return place;
}

std::vector<std::string> switch_links_update(Document &doc)
{
  std::vector<std::string> said;
  /* Members whose group is gone (pasted from another file): on their own, at their first child. */
  std::vector<Layer *> switches;
  const auto gather = [&](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &l : layers) {
      if (l.is_switch()) {
        switches.push_back(&l);
      }
      self(self, l.children);
    }
  };
  gather(gather, doc.layers);
  for (Layer *sw : switches) {
    if (sw->switch_link && !switch_link_of(doc, *sw)) {
      sw->switch_link = 0;
      sw->switch_keys.keys.clear();
      sw->switch_keys.actions.clear();
      sw->switch_value = sw->children.empty() ? 0.0f : float(sw->children.front().id);
      sw->switch_keys.last = sw->switch_value;
    }
  }
  /* Same-named switches at the same place inside a switch's children. */
  for (Layer *outer : switches) {
    if (!outer->link_same_named || outer->children.size() < 2) {
      continue;
    }
    std::map<std::string, std::vector<Layer *>> by_place;
    for (Layer &child : outer->children) {
      std::set<std::string> seen;
      const auto walk = [&](auto &&self, Layer &l) -> void {
        for (Layer &inner : l.children) {
          if (inner.is_switch()) {
            const std::string place = place_inside(doc, child, inner);
            if (seen.insert(place).second) {
              by_place[place].push_back(&inner);
            }
          }
          self(self, inner);
        }
      };
      walk(walk, child);
    }
    for (auto &[place, found] : by_place) {
      std::vector<Layer *> members;
      for (Layer *m : found) {
        if (!m->switch_link_off) {
          members.push_back(m);
        }
      }
      if (members.size() < 2) {
        continue;
      }
      uint64_t group = 0;
      for (Layer *m : members) {
        if (m->switch_link) {
          group = m->switch_link;
          break;
        }
      }
      int dropped = 0;
      int joined = 0;
      if (!group) {
        group = switch_link_create(doc, members, nullptr, &dropped);
        joined = int(members.size());
      }
      else {
        SwitchLink *link = nullptr;
        for (SwitchLink &l : doc.switch_links) {
          link = l.id == group ? &l : link;
        }
        for (Layer *m : members) {
          if (!m->switch_link && link) {
            link_join(doc, *link, *m, &dropped);
            joined++;
          }
        }
      }
      if (joined > 0) {
        std::string text = "Linked by name: " + place + " in " + std::to_string(joined) + " views of \"" + outer->name +
                           "\": picking a drawing in one picks the same-named drawing in all";
        if (dropped > 0) {
          text += " (" + std::to_string(dropped) + (dropped == 1 ? " key" : " keys") + " of the others went: the lead's are kept)";
        }
        said.push_back(text);
      }
    }
  }
  /* Groups: a lead that is still a member, keys only on the lead, at least two members. */
  for (size_t i = 0; i < doc.switch_links.size();) {
    SwitchLink &link = doc.switch_links[i];
    std::vector<Layer *> members = switch_link_members(doc, link.id);
    if (members.size() < 2) {
      const uint64_t id = link.id;
      for (Layer *m : members) {
        link_leave(doc, link, *m, false);
      }
      std::erase_if(doc.switch_links, [&](const SwitchLink &l) { return l.id == id; });
      continue;
    }
    if (std::find_if(members.begin(), members.end(), [&](const Layer *m) { return m->id == link.lead; }) == members.end()) {
      link.lead = members.front()->id;
    }
    for (Layer *m : members) {
      if (m->id != link.lead) {
        drop_mainline(*m);
      }
    }
    i++;
  }
  /* Renames: a drawing renamed in a linked switch is renamed in its partners, and the group's name with it
   * (unless renamed here only). */
  for (Layer *sw : switches) {
    SwitchLink *link = switch_link_of(doc, *sw);
    for (Layer &child : sw->children) {
      if (child.link_name_seen.empty() || !link) {
        child.link_name_seen = child.name;
        continue;
      }
      if (child.link_name_seen == child.name) {
        continue;
      }
      const std::string was = child.link_name_seen, now = child.name;
      child.link_name_seen = now;
      if (g_rename_here) {
        continue;
      }
      for (SwitchLinkName &n : link->names) {
        if (n.name == was) {
          n.name = now;
        }
      }
      int partners = 0;
      for (Layer *m : switch_link_members(doc, link->id)) {
        for (Layer &c : m->children) {
          if (m != sw && c.name == was) {
            c.name = now;
            c.link_name_seen = now;
            partners++;
          }
        }
      }
      if (partners > 0) {
        said.push_back("Renamed \"" + was + "\" to \"" + now + "\" in " + std::to_string(partners) +
                       (partners == 1 ? " linked switch" : " linked switches") + " too");
      }
    }
  }
  g_rename_here = false;
  return said;
}

static uint64_t g_preview_switch = 0, g_preview_child = 0;
static bool g_preview = false;

void switch_preview_set(uint64_t sw, uint64_t child)
{
  g_preview = true;
  g_preview_switch = sw;
  g_preview_child = child;
}

void switch_preview_clear()
{
  g_preview = false;
}

bool switch_preview_active(uint64_t *r_sw, uint64_t *r_child)
{
  if (r_sw) {
    *r_sw = g_preview_switch;
  }
  if (r_child) {
    *r_child = g_preview_child;
  }
  return g_preview;
}

int switch_keys_showing(const Document &doc, const Layer &sw, uint64_t child)
{
  SwitchLink *link = nullptr;
  if (const Layer *lead = lead_of(const_cast<Document &>(doc), sw, &link)) {
    /* Linked: the group's keys naming it (the lead's), and its own actions'. */
    const Layer *c = switch_child(sw, float(child));
    if (!c) {
      return 0;
    }
    int count = 0;
    const auto names = [&](float v) { return value_id(v) && answers(*c, entry_name(*link, v)); };
    for (const Key<float> &k : lead->switch_keys.keys) {
      count += names(k.value) ? 1 : 0;
    }
    for (const auto &[name, keys] : sw.switch_keys.actions) {
      for (const Key<float> &k : keys) {
        count += names(k.value) ? 1 : 0;
      }
    }
    return count;
  }
  int count = 0;
  const auto shows = [&](float v) { return value_id(v) == child; };
  for (const Key<float> &k : sw.switch_keys.keys) {
    count += shows(k.value) ? 1 : 0;
  }
  for (const auto &[name, keys] : sw.switch_keys.actions) {
    for (const Key<float> &k : keys) {
      count += shows(k.value) ? 1 : 0;
    }
  }
  if (!sw.switch_keys.animated() && shows(sw.switch_value)) {
    count++;
  }
  return count;
}

/* `out`: switched out as renders show it; `edit_out`: as the viewport does, where the child holding
 * the layer being edited (`editing`: it and the layers holding it) shows in place of the pick. */
static void resolve(Document &doc, std::vector<Layer> &layers, int frame, bool out, bool edit_out,
                    const std::set<uint64_t> &editing)
{
  for (Layer &layer : layers) {
    layer.switched_out = out;
    layer.edit_out = edit_out;
    if (!layer.is_switch()) {
      resolve(doc, layer.children, frame, out, edit_out, editing);
      continue;
    }
    /* Linked by name: the group's keys are the lead's, and hold entries. */
    SwitchLink *link = nullptr;
    const Layer *lead = layer.switch_link ? lead_of(doc, layer, &link) : nullptr;
    const Layer &keyed = lead ? *lead : layer;
    float value;
    const Channel<float> &keys = keyed.switch_keys;
    if (frame > 0 && keys.animated() && !is_rig(keyed.switch_value) &&
        std::fabs(keyed.switch_value - keys.evaluate(frame)) > 0.5f)
    {
      /* A pick not keyed (Auto Keying off): shown, as a hand key would be, until the frame changes. */
      value = keyed.switch_value;
    }
    else if (!hand_value(keyed, frame, &value)) {
      const bool own_rig = layer.switch_rig_set && !is_rig(layer.switch_rig);
      const bool lead_rig = lead && keyed.switch_rig_set && !is_rig(keyed.switch_rig);
      value = own_rig ? layer.switch_rig : lead_rig ? keyed.switch_rig : switch_default(keyed);
    }
    const Layer *shown = nullptr;
    layer.switch_missing.clear();
    if (lead) {
      /* A name it lacks: the last one it had. */
      shown = linked_child(*link, layer, *lead, value, frame, &layer.switch_missing);
    }
    else {
      /* A child that is gone shows the default, and the default gone shows none. */
      shown = switch_child(layer, value);
      if (!shown && value_id(value)) {
        shown = switch_child(layer, switch_default(layer));
      }
    }
    layer.switch_shown = shown ? shown->id : 0;
    uint64_t edit_shown = layer.switch_shown;
    for (const Layer &child : layer.children) {
      if (editing.count(child.id)) {
        edit_shown = child.id;
      }
    }
    if (g_preview && g_preview_switch == layer.id) {
      edit_shown = g_preview_child;
    }
    for (Layer &child : layer.children) {
      const bool was_out = child.switched_out, was_edit_out = child.edit_out;
      child.switched_out = out || child.id != layer.switch_shown;
      child.edit_out = edit_out || child.id != edit_shown;
      if (child.switched_out != was_out || child.edit_out != was_edit_out) {
        /* Shown or hidden now: views draw it anew. */
        layer_touch(child);
        layer_touch(layer);
      }
      resolve(doc, child.children, frame, child.switched_out, child.edit_out, editing);
    }
  }
}

/* The layer that became active, and on which frame: a hidden child it is in shows for editing only on
 * that frame. Once the frame changes (scrubbing, playing) the switches show their picks again, though it
 * stays active. */
static uint64_t g_editing_layer = 0;
static int g_editing_frame = 0;
static bool g_editing = false;

void switches_resolve(Document &doc, int frame)
{
  if (doc.active_layer != g_editing_layer) {
    g_editing_layer = doc.active_layer;
    g_editing_frame = frame;
    g_editing = true;
  }
  else if (frame != g_editing_frame) {
    g_editing = false;
  }
  std::set<uint64_t> editing;
  for (const Layer *l = g_editing ? layer_find(doc, doc.active_layer) : nullptr; l; l = layer_parent(doc, l->id)) {
    editing.insert(l->id);
  }
  resolve(doc, doc.layers, frame, false, false, editing);
}

static void set_raw(Document &doc, Layer &sw, float value);

void switch_set(Document &doc, Layer &sw, float value)
{
  SwitchLink *link = nullptr;
  if (Layer *lead = lead_of(doc, sw, &link)) {
    /* Linked: the group's keys, by name. */
    set_raw(doc, *lead, to_entry(*link, sw, value));
    return;
  }
  set_raw(doc, sw, value);
}

static void set_raw(Document &doc, Layer &sw, float value)
{
  Channel<float> &ch = sw.switch_keys;
  const int frame = doc.frame_current;
  if (frame != 0 && !doc.auto_key) {
    /* Auto Keying off: shown, not keyed, as any property edited then. Keyed, it shows until the frame
     * changes (I keys it); not keyed yet, it is the switch's value everywhere. */
    sw.switch_value = value;
    if (!ch.animated()) {
      ch.last = value;
      ch.has_last = true;
    }
    switches_resolve(doc, frame);
    return;
  }
  if (frame != 0 && !ch.animated() && doc.action.empty()) {
    /* The default stays what it was (an action's keys start after frame 0, the shared setup). */
    ch.set_key(0, switch_default(sw)).interp = Interp::Step;
  }
  if (frame != 0 || ch.animated()) {
    ch.set_key(frame, value).interp = Interp::Step;
  }
  if (frame == 0 || !ch.animated()) {
    sw.switch_value = value;
  }
  else {
    sw.switch_value = ch.evaluate(frame);
  }
  ch.last = sw.switch_value;
  ch.has_last = true;
  switches_resolve(doc, frame);
}

static void set_hold_raw(Document &doc, Layer &sw, float value);

void switch_set_hold(Document &doc, Layer &sw, float value)
{
  SwitchLink *link = nullptr;
  if (Layer *lead = lead_of(doc, sw, &link)) {
    set_hold_raw(doc, *lead, to_entry(*link, sw, value));
    return;
  }
  set_hold_raw(doc, sw, value);
}

static void set_hold_raw(Document &doc, Layer &sw, float value)
{
  Channel<float> &ch = sw.switch_keys;
  const Key<float> *start = nullptr;
  for (const Key<float> &k : ch.keys) {
    if (k.frame > doc.frame_current) {
      break;
    }
    start = &k;
  }
  if (!start || (start->frame == 0 && doc.action.empty())) {
    /* The hold from frame 0: the default. */
    const int now = doc.frame_current;
    doc.frame_current = 0;
    set_raw(doc, sw, value);
    doc.frame_current = now;
    sw.switch_value = ch.animated() ? ch.evaluate(now) : value;
    ch.last = sw.switch_value;
    switches_resolve(doc, now);
    return;
  }
  ch.set_key(start->frame, value).interp = Interp::Step;
  sw.switch_value = ch.evaluate(doc.frame_current);
  ch.last = sw.switch_value;
  ch.has_last = true;
  switches_resolve(doc, doc.frame_current);
}

static int replace_raw(Document &doc, Layer &sw, float from, float to);

int switch_replace_keys(Document &doc, Layer &sw, float from, float to, bool from_entry_given)
{
  SwitchLink *link = nullptr;
  if (Layer *lead = lead_of(doc, sw, &link)) {
    /* Linked: `from` a child (its name), or an entry of the group's names, `to` a child. */
    const float from_entry = from_entry_given ? from : to_entry(*link, sw, from);
    const float to_entry_value = to_entry(*link, sw, to);
    int count = replace_raw(doc, *lead, from_entry, to_entry_value);
    for (Layer *m : switch_link_members(doc, link->id)) {
      if (m != lead) {
        for (auto &[name, keys] : m->switch_keys.actions) {
          for (Key<float> &k : keys) {
            if (std::fabs(k.value - from_entry) < 0.5f) {
              k.value = to_entry_value;
              count++;
            }
          }
        }
      }
    }
    return count;
  }
  return replace_raw(doc, sw, from, to);
}

static int replace_raw(Document &doc, Layer &sw, float from, float to)
{
  const auto same = [&](float v) { return std::fabs(v - from) < 0.5f; };
  int count = 0;
  const auto replace = [&](std::vector<Key<float>> &keys) {
    for (Key<float> &k : keys) {
      if (same(k.value)) {
        k.value = to;
        count++;
      }
    }
  };
  Channel<float> &ch = sw.switch_keys;
  replace(ch.keys);
  for (auto &[name, keys] : ch.actions) {
    replace(keys);
  }
  if (!ch.animated() && same(sw.switch_value)) {
    sw.switch_value = to;
    count++;
  }
  sw.switch_value = ch.animated() ? ch.evaluate(doc.frame_current) : sw.switch_value;
  ch.last = sw.switch_value;
  ch.has_last = true;
  switches_resolve(doc, doc.frame_current);
  return count;
}

SwitchSource switch_source(const Document &doc, const Layer &sw, int frame)
{
  float value;
  const Layer *lead = lead_of(const_cast<Document &>(doc), sw);
  if (hand_value(lead ? *lead : sw, frame, &value)) {
    return SwitchSource::Hand;
  }
  return sw.switch_rig_set && !is_rig(sw.switch_rig) ? SwitchSource::Rig : SwitchSource::Default;
}

bool switch_keyed_by_actions(const Layer &sw)
{
  for (const auto &[name, keys] : sw.switch_keys.actions) {
    if (!name.empty() && !keys.empty()) {
      return true;
    }
  }
  return false;
}

Layer *switch_of(Document &doc, uint64_t layer)
{
  for (Layer *l = layer_find(doc, layer); l; l = layer_parent(doc, l->id)) {
    if (l->is_switch()) {
      return l;
    }
  }
  return nullptr;
}

const Layer *switch_child_holding(const Document &doc, const Layer &sw, uint64_t layer)
{
  for (const Layer *l = layer_find(doc, layer); l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    for (const Layer &child : sw.children) {
      if (child.id == l->id) {
        return &child;
      }
    }
  }
  return nullptr;
}

void switch_child_added(Layer &sw, uint64_t child)
{
  if (!sw.is_switch() || sw.children.size() != 1 || sw.switch_keys.animated() || value_id(sw.switch_value)) {
    return;
  }
  sw.switch_value = float(child);
  sw.switch_keys.last = sw.switch_value;
  sw.switch_keys.has_last = true;
}

void switch_remap(Layer &layer, const std::map<uint64_t, uint64_t> &remap)
{
  if (!layer.is_switch()) {
    return;
  }
  if (layer.switch_link) {
    /* Linked: its values are the group's names, not layers; only its own setup child is one. */
    if (const auto it = remap.find(value_id(layer.switch_value)); it != remap.end()) {
      layer.switch_value = float(it->second);
      layer.switch_keys.last = layer.switch_value;
    }
    layer.switch_shown = 0;
    return;
  }
  const auto map = [&](float &value) {
    if (const auto it = remap.find(value_id(value)); it != remap.end()) {
      value = float(it->second);
    }
  };
  map(layer.switch_value);
  map(layer.switch_keys.last);
  for (Key<float> &k : layer.switch_keys.keys) {
    map(k.value);
  }
  for (auto &[name, keys] : layer.switch_keys.actions) {
    for (Key<float> &k : keys) {
      map(k.value);
    }
  }
  layer.switch_shown = 0;
}

}  // namespace oanim
