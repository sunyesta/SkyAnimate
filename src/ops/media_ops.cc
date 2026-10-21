/* Media layer operators: adding a file (images, image sequences, video, sound), swapping a clip's
 * file, packing, finding missing files, and the clip edits the timeline makes (move, trim, slip,
 * split, delete). */

#include "ops/media_ops.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "anim/animation.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "data/media_layer.hh"
#include "media/media.hh"
#include "ops/operator.hh"
#include "wm/context.hh"
#include "wm/file_dialog.hh"

namespace oanim {

namespace fs = std::filesystem;

static fs::path path_from_utf8(const std::string &path)
{
  return fs::path(std::u8string(path.begin(), path.end()));
}

static std::string path_to_utf8(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

const char *media_file_pattern()
{
  return "png;jpg;jpeg;bmp;gif;tif;tiff;webp;heic;avif;mp4;m4v;mov;avi;wmv;mkv;webm;mpg;mpeg;mts;m2ts;3gp;"
         "wav;mp3;m4a;aac;wma;flac;aif;aiff";
}

std::string media_info_text(const MediaInfo &info)
{
  char text[160];
  switch (info.kind) {
    case MediaKind::Image:
      std::snprintf(text, sizeof(text), "Image, %d x %d", info.source_width, info.source_height);
      break;
    case MediaKind::Sequence:
      std::snprintf(text, sizeof(text), "Image sequence, %d x %d, %d images", info.source_width, info.source_height,
                    info.frame_count);
      break;
    case MediaKind::Video:
      std::snprintf(text, sizeof(text), "Video, %d x %d, %.3g fps, %.1f s%s", info.source_width, info.source_height,
                    info.fps, info.duration, info.has_audio ? ", with sound" : "");
      break;
    case MediaKind::Audio:
      std::snprintf(text, sizeof(text), "Sound, %.1f s", info.duration);
      break;
    default:
      return info.error.empty() ? std::string("Can't be read") : info.error;
  }
  return text;
}

Layer *media_target_layer(Context &C)
{
  if (!C.document) {
    return nullptr;
  }
  Document &doc = *C.document;
  if (op_has_arg(C, "layer")) {
    Layer *l = layer_find(doc, uint64_t(std::strtoull(op_arg(C, "layer").c_str(), nullptr, 10)));
    return l && l->is_media() ? l : nullptr;
  }
  Layer *active = document_active_layer(doc);
  return active && active->is_media() ? active : nullptr;
}

static bool media_poll(Context &C)
{
  return media_target_layer(C) != nullptr;
}

/* The clip an operator works on: `clip` (its id), else the one at the current frame, else the
 * first selected one. */
static MediaClip *target_clip(Context &C, Layer &layer)
{
  if (op_has_arg(C, "clip")) {
    const uint64_t id = uint64_t(std::strtoull(op_arg(C, "clip").c_str(), nullptr, 10));
    for (MediaClip &c : layer.media.clips) {
      if (c.id == id) {
        return &c;
      }
    }
    return nullptr;
  }
  if (MediaClip *c = media_clip_at(layer, C.document->frame_current)) {
    return c;
  }
  for (MediaClip &c : layer.media.clips) {
    if (c.selected) {
      return &c;
    }
  }
  return layer.media.clips.empty() ? nullptr : &layer.media.clips.front();
}

/* No filepath: in the window, ask for one and run again with it. */
static OpStatus ask_for_media(Context &C, const char *idname)
{
  if (C.wm->background) {
    report(ReportType::Error, std::string(idname) + " needs a filepath without a window");
    return OpStatus::Cancelled;
  }
  Context *ctx = &C;
  OpArgs args = C.op_args ? *C.op_args : OpArgs{};
  std::string folder;
  if (!C.wm->filepath.empty()) {
    folder = path_to_utf8(path_from_utf8(C.wm->filepath).parent_path());
  }
  file_dialog_open("Images, video and sound", media_file_pattern(),
                   [ctx, idname, args](const std::string &path) mutable {
                     args["filepath"] = path;
                     /* Run as from the viewport, so its options show in the ViewportActionPanel. */
                     const std::string area = ctx->area_type, drawing = ctx->drawing_area;
                     ctx->area_type = "Viewport";
                     ctx->drawing_area.clear();
                     operator_call(*ctx, idname, args);
                     ctx->area_type = area;
                     ctx->drawing_area = drawing;
                   },
                   folder);
  return OpStatus::Cancelled;
}

static bool readable(const std::string &path, bool sequence, MediaInfo *r_info)
{
  *r_info = media_probe(path, sequence);
  if (!r_info->ok()) {
    report(ReportType::Error, "Can't add " + path + ": " + r_info->error);
    return false;
  }
  return true;
}

static void add_operator(const char *idname, const char *name, const char *description,
                         std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec,
                         std::vector<OpProperty> props = {})
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot.poll = std::move(poll);
  ot.exec = std::move(exec);
  ot.props = std::move(props);
  operators().add(std::move(ot));
}

static double clip_rate(const Document &doc, const MediaClip &clip)
{
  const MediaInfo info = media_clip_info(clip);
  return clip.timing == MediaTiming::FrameByFrame && info.kind == MediaKind::Video && info.fps > 0.0 ? info.fps
                                                                                                     : double(doc.fps);
}

/* Splits `layer`'s clip covering `frame` there (a clip starting on it isn't split). Returns the
 * part from `frame` on, or null. */
static MediaClip *split_at(Document &doc, Layer &layer, int frame)
{
  for (size_t i = 0; i < layer.media.clips.size(); i++) {
    MediaClip c = layer.media.clips[i];
    if (frame > c.start && frame < c.start + c.length) {
      MediaClip right = c;
      right.id = layer.media.next_clip_id++;
      right.start = frame;
      right.length = c.start + c.length - frame;
      right.in += double(frame - c.start) / clip_rate(doc, c);
      right.selected = false;
      layer.media.clips[i].length = frame - c.start;
      layer.media.clips.insert(layer.media.clips.begin() + long(i) + 1, right);
      return &layer.media.clips[i + 1];
    }
  }
  return nullptr;
}

void media_operators_register()
{
  /* Add Media: a Media layer above the active one, its clip as long as the file (a still image: to
   * the end of the frame range). A Guide at half opacity, fitted to the camera, unless asked. */
  add_operator(
      "media.add", "Add Media",
      "Add an image, image sequence, video or sound as a Media layer: a guide to draw over, shown while you "
      "work and not rendered",
      [](Context &C) { return C.document != nullptr; },
      [](Context &C) {
        const std::string path = op_arg(C, "filepath");
        if (path.empty()) {
          return ask_for_media(C, "media.add");
        }
        Document &doc = *C.document;
        /* A numbered image ("walk_0001.png" with siblings) is a sequence unless asked otherwise. */
        const bool sequence = op_has_arg(C, "sequence") ? op_arg_bool(C, "sequence")
                                                        : !media_sequence_files(path).empty();
        MediaInfo info;
        if (!readable(path, sequence, &info)) {
          return OpStatus::Cancelled;
        }
        const int start = std::max(op_arg_int(C, "frame_start", 1), 1);
        /* Named after the file ("walk"), numbered only when that name is taken ("walk 1"). */
        std::string name = path_to_utf8(path_from_utf8(path).stem());
        const std::vector<const Layer *> flat = document_layers_flat(doc);
        if (std::any_of(flat.begin(), flat.end(), [&](const Layer *l) { return l->name == name; })) {
          name = document_layer_unique_name(doc, name);
        }
        Layer &layer = document_add_layer(doc, LayerType::Media, name);
        const bool picture = info.kind != MediaKind::Audio;
        layer.media.guide = op_has_arg(C, "guide") ? op_arg_bool(C, "guide") : true;
        const std::string fit = op_arg(C, "fit", "FIT");
        layer.media.fit = fit == "FILL" ? MediaFit::Fill : fit == "STRETCH" ? MediaFit::Stretch
                          : fit == "ORIGINAL"                               ? MediaFit::Original
                                                                            : MediaFit::Fit;
        layer.opacity = std::clamp(op_arg_float(C, "opacity", picture ? 0.5f : 1.0f), 0.0f, 1.0f);
        layer.media.clips.push_back(media_clip_new(doc, layer, path, sequence, start));
        /* Sound starts decoding now, to be ready when it plays. */
        if (info.has_audio) {
          media_sound(path);
        }
        const MediaClip &clip = layer.media.clips.back();
        char frames[96];
        std::snprintf(frames, sizeof(frames), ": frames %d to %d", clip.start, clip.start + clip.length - 1);
        report(ReportType::Info, "Added " + layer.name + " (" + media_info_text(info) + ")" + frames);
        /* Its settings, in view. */
        C.wm->properties_tab_request = 8;
        animation_sync(doc);
        return OpStatus::Finished;
      },
      {OpProperty{"frame_start", "Start Frame", "The frame it starts on", OpProperty::Kind::Int, {}, "1", 1.0f,
                  1048574.0f, 1.0f},
       OpProperty{"fit",
                  "Fit",
                  "How the picture sits in the camera's frame",
                  OpProperty::Kind::Enum,
                  {{"FIT", "Fit"}, {"FILL", "Fill"}, {"STRETCH", "Stretch"}, {"ORIGINAL", "Original Size"}},
                  "FIT"},
       OpProperty{"opacity", "Opacity", "How opaque the picture is (half, to draw over it)", OpProperty::Kind::Float,
                  {}, "0.5", 0.0f, 1.0f, 0.01f},
       OpProperty{"guide", "Guide", "Show it while you work, never in renders", OpProperty::Kind::Bool, {}, "1"}});

  /* Replace: the clip's file swapped for another, its timing kept (After Effects' Replace Footage). */
  add_operator(
      "media.replace", "Replace Media", "Show another file in the clip at the current frame, keeping its timing",
      media_poll, [](Context &C) {
        const std::string path = op_arg(C, "filepath");
        if (path.empty()) {
          return ask_for_media(C, "media.replace");
        }
        Layer &layer = *media_target_layer(C);
        MediaClip *clip = target_clip(C, layer);
        const bool sequence = op_has_arg(C, "sequence") ? op_arg_bool(C, "sequence") : !media_sequence_files(path).empty();
        MediaInfo info;
        if (!clip || !readable(path, sequence, &info)) {
          return OpStatus::Cancelled;
        }
        clip->filepath = path;
        clip->sequence = sequence;
        clip->packed.clear();
        if (sequence) {
          clip->timing = MediaTiming::FrameByFrame;
        }
        report(ReportType::Info, "Now showing " + path + " (" + media_info_text(info) + ")");
        return OpStatus::Finished;
      });

  /* Replace from Playhead: the source changes part way through: a new clip from the current frame
   * on, as long as its file, over what was there. */
  add_operator(
      "media.replace_from_frame", "Replace from Current Frame",
      "Show another file from the current frame on: the clip there ends and a new one starts", media_poll,
      [](Context &C) {
        const std::string path = op_arg(C, "filepath");
        if (path.empty()) {
          return ask_for_media(C, "media.replace_from_frame");
        }
        Document &doc = *C.document;
        Layer &layer = *media_target_layer(C);
        const bool sequence = op_has_arg(C, "sequence") ? op_arg_bool(C, "sequence") : !media_sequence_files(path).empty();
        MediaInfo info;
        if (!readable(path, sequence, &info)) {
          return OpStatus::Cancelled;
        }
        const int frame = std::max(op_arg_int(C, "frame", doc.frame_current), 1);
        split_at(doc, layer, frame);
        MediaClip clip = media_clip_new(doc, layer, path, sequence, frame);
        const uint64_t id = clip.id;
        layer.media.clips.push_back(clip);
        media_clips_tidy(doc, layer, id);
        if (info.has_audio) {
          media_sound(path);
        }
        return OpStatus::Finished;
      });

  add_operator(
      "media.reload", "Reload Media", "Read the layer's files again (after changing them in another program)",
      media_poll, [](Context &C) {
        for (const MediaClip &c : media_target_layer(C)->media.clips) {
          media_reload(media_clip_file(c));
        }
        return OpStatus::Finished;
      });
  /* Nothing in the document changes: no undo step for it. */
  if (OperatorType *ot = const_cast<OperatorType *>(operators().find("media.reload"))) {
    ot->flag = OPTYPE_REGISTER;
  }

  /* Pack: a copy of each file kept inside the .oanim, so it travels with it (Blender's Pack). */
  add_operator(
      "media.pack", "Pack Media", "Keep a copy of the layer's files inside the .oanim, so it opens anywhere",
      media_poll, [](Context &C) {
        int packed = 0;
        for (MediaClip &c : media_target_layer(C)->media.clips) {
          if (!c.packed.empty() || c.sequence) {
            continue;
          }
          std::string error;
          const std::string id = media_pack_file(c.filepath, &error);
          if (id.empty()) {
            report(ReportType::Error, error);
            continue;
          }
          c.packed = id;
          packed++;
        }
        if (packed == 0) {
          report(ReportType::Warning, "Nothing to pack (image sequences stay linked)");
          return OpStatus::Cancelled;
        }
        return OpStatus::Finished;
      });

  /* Unpack: back to the file on disk; one that is gone is written out beside the .oanim. */
  add_operator(
      "media.unpack", "Unpack Media", "Use the files on disk again instead of the copies inside the .oanim",
      media_poll, [](Context &C) {
        int unpacked = 0;
        for (MediaClip &c : media_target_layer(C)->media.clips) {
          if (c.packed.empty()) {
            continue;
          }
          std::error_code ec;
          if (!fs::is_regular_file(path_from_utf8(c.filepath), ec)) {
            const std::string *bytes = media_packed_bytes(c.packed);
            if (!bytes) {
              continue;
            }
            const fs::path folder = C.wm->filepath.empty() ? fs::temp_directory_path(ec) / "OpenAnimate"
                                                           : path_from_utf8(C.wm->filepath).parent_path() / "media";
            fs::create_directories(folder, ec);
            const fs::path file = folder / path_from_utf8(media_packed_name(c.packed));
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(bytes->data(), std::streamsize(bytes->size()));
            c.filepath = path_to_utf8(file);
            report(ReportType::Info, "Wrote " + c.filepath);
          }
          c.packed.clear();
          unpacked++;
        }
        return unpacked > 0 ? OpStatus::Finished : OpStatus::Cancelled;
      });

  /* Locate: a missing file found by hand; any other missing file in that folder is found with it
   * (Premiere's "relink others automatically"). */
  add_operator(
      "media.locate", "Locate Missing File",
      "Point the clip at where its file is now; other missing files in that folder are found too", media_poll,
      [](Context &C) {
        const std::string path = op_arg(C, "filepath");
        if (path.empty()) {
          return ask_for_media(C, "media.locate");
        }
        Document &doc = *C.document;
        Layer &layer = *media_target_layer(C);
        MediaClip *clip = target_clip(C, layer);
        MediaInfo info;
        if (!clip || !readable(path, clip->sequence, &info)) {
          return OpStatus::Cancelled;
        }
        clip->filepath = path;
        const int more = media_relink_folder(doc, path_to_utf8(path_from_utf8(path).parent_path()));
        const int left = media_missing_count(doc);
        std::string text = "Found " + path_to_utf8(path_from_utf8(path).filename());
        if (more > 0) {
          text += " and " + std::to_string(more) + " more missing file" + (more == 1 ? "" : "s") + " in that folder";
        }
        if (left > 0) {
          text += "; " + std::to_string(left) + " still missing";
        }
        report(ReportType::Info, text);
        return OpStatus::Finished;
      });

  /* A clip's timing, as the timeline's drags set it: Start (slide), Length and Start together (trim)
   * and In (slip); what it lands on is overwritten. */
  add_operator(
      "media.clip_set", "Change Clip", "Set a clip's start, length, In point, end or timing", media_poll,
      [](Context &C) {
        Document &doc = *C.document;
        Layer &layer = *media_target_layer(C);
        MediaClip *clip = target_clip(C, layer);
        if (!clip) {
          return OpStatus::Cancelled;
        }
        clip->start = std::max(op_arg_int(C, "start", clip->start), 1);
        clip->length = std::max(op_arg_int(C, "length", clip->length), 1);
        if (op_has_arg(C, "in_point")) {
          clip->in = std::max(std::strtod(op_arg(C, "in_point").c_str(), nullptr), 0.0);
        }
        const std::string end = op_arg(C, "end");
        if (!end.empty()) {
          clip->end = end == "LOOP" ? MediaEnd::Loop : end == "HIDE" ? MediaEnd::Hide : MediaEnd::Hold;
        }
        const std::string timing = op_arg(C, "timing");
        if (!timing.empty() && !clip->sequence) {
          /* Still covering the same part of the file: its length follows the new speed (unless given). */
          const double before = media_clip_file_frames(doc, *clip);
          clip->timing = timing == "FRAME_BY_FRAME" ? MediaTiming::FrameByFrame : MediaTiming::RealTime;
          const double after = media_clip_file_frames(doc, *clip);
          if (!op_has_arg(C, "length") && before > 0.0) {
            clip->length = std::max(1, int(std::lround(double(clip->length) * after / before)));
          }
        }
        media_clips_tidy(doc, layer, clip->id);
        return OpStatus::Finished;
      });

  add_operator(
      "media.clip_split", "Split Clip", "Cut the clip at the current frame into two", media_poll, [](Context &C) {
        Document &doc = *C.document;
        return split_at(doc, *media_target_layer(C), op_arg_int(C, "frame", doc.frame_current)) ? OpStatus::Finished
                                                                                                 : OpStatus::Cancelled;
      });

  add_operator(
      "media.clip_delete", "Delete Clip", "Remove the selected clips (or the clip given, or the one at the current frame)",
      media_poll, [](Context &C) {
        Layer &layer = *media_target_layer(C);
        std::vector<MediaClip> &clips = layer.media.clips;
        const size_t before = clips.size();
        if (!op_has_arg(C, "clip") && std::any_of(clips.begin(), clips.end(), [](const MediaClip &c) { return c.selected; })) {
          clips.erase(std::remove_if(clips.begin(), clips.end(), [](const MediaClip &c) { return c.selected; }), clips.end());
        }
        else if (MediaClip *c = target_clip(C, layer)) {
          const uint64_t id = c->id;
          clips.erase(std::remove_if(clips.begin(), clips.end(), [id](const MediaClip &k) { return k.id == id; }),
                      clips.end());
        }
        return clips.size() < before ? OpStatus::Finished : OpStatus::Cancelled;
      });
}

}  // namespace oanim
