#include "media/media.hh"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace oanim {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

static std::wstring widen(const std::string &s)
{
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

static fs::path path_from_utf8(const std::string &path)
{
  return fs::path(std::u8string(path.begin(), path.end()));
}

static std::string path_to_utf8(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

static std::atomic<uint64_t> g_generation{1};
static std::atomic<uint64_t> g_frame_serial{1};

uint64_t media_generation()
{
  return g_generation.load();
}

/* COM on the calling thread, once. Worker threads take the multithreaded model; the main thread is
 * single-threaded (media_init), as OLE needs: SDL's OleInitialize fails on a multithreaded thread and
 * falls back to drops that carry no position. */
static thread_local bool g_com_done = false;

static void com_thread()
{
  if (!g_com_done) {
    g_com_done = true;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;
  }
}

static bool g_mf_started = false;

void media_init()
{
  if (!g_com_done) {
    g_com_done = true;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)hr;
  }
  g_mf_started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
}

/* ---------------------------------------------------------------------------------
 * Files
 */

static std::string lower_extension(const std::string &path)
{
  std::string ext = path_to_utf8(path_from_utf8(path).extension());
  for (char &c : ext) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext;
}

MediaKind media_kind_from_path(const std::string &path)
{
  static const char *const images[] = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff", ".webp", ".heic",
                                       ".heif", ".jxr", ".ico", ".dds", ".avif"};
  static const char *const videos[] = {".mp4", ".m4v", ".mov", ".avi", ".wmv", ".mkv", ".webm", ".mpg", ".mpeg",
                                       ".mts", ".m2ts", ".3gp", ".asf"};
  static const char *const sounds[] = {".wav", ".mp3", ".m4a", ".aac", ".wma", ".flac", ".aif", ".aiff"};
  const std::string ext = lower_extension(path);
  for (const char *e : images) {
    if (ext == e) {
      return MediaKind::Image;
    }
  }
  for (const char *e : videos) {
    if (ext == e) {
      return MediaKind::Video;
    }
  }
  for (const char *e : sounds) {
    if (ext == e) {
      return MediaKind::Audio;
    }
  }
  return MediaKind::None;
}

/* "walk_0012.png" -> ("walk_", "0012", ".png"); false when the name doesn't end in digits. */
static bool split_number(const fs::path &file, std::wstring *r_prefix, std::wstring *r_digits)
{
  const std::wstring stem = file.stem().wstring();
  size_t at = stem.size();
  while (at > 0 && std::iswdigit(stem[at - 1])) {
    at--;
  }
  if (at == stem.size()) {
    return false;
  }
  *r_prefix = stem.substr(0, at);
  *r_digits = stem.substr(at);
  return true;
}

std::vector<std::string> media_sequence_files(const std::string &path)
{
  const fs::path file = path_from_utf8(path);
  if (media_kind_from_path(path) != MediaKind::Image) {
    return {};
  }
  std::wstring prefix, digits;
  if (!split_number(file, &prefix, &digits)) {
    return {};
  }
  const std::wstring ext = file.extension().wstring();
  std::vector<std::pair<long long, fs::path>> found;
  std::error_code ec;
  for (const fs::directory_entry &e : fs::directory_iterator(file.parent_path(), ec)) {
    std::wstring p, d;
    if (!e.is_regular_file(ec) || _wcsicmp(e.path().extension().c_str(), ext.c_str()) != 0 ||
        !split_number(e.path(), &p, &d) || _wcsicmp(p.c_str(), prefix.c_str()) != 0)
    {
      continue;
    }
    found.push_back({std::stoll(d), e.path()});
  }
  if (found.size() < 2) {
    return {};
  }
  std::sort(found.begin(), found.end());
  /* From the file given on (it may not be the first: dropping frame 10 starts there). */
  const long long first = std::stoll(digits);
  std::vector<std::string> files;
  for (const auto &[n, p] : found) {
    if (n >= first) {
      files.push_back(path_to_utf8(p));
    }
  }
  return files.size() >= 2 ? files : std::vector<std::string>();
}

/* ---------------------------------------------------------------------------------
 * Pictures
 */

/* Halves the picture until its longer side fits MEDIA_MAX_SIZE (a 2x2 average each time). */
static void fit_max_size(MediaFrame &f)
{
  while (std::max(f.width, f.height) > MEDIA_MAX_SIZE && f.width > 1 && f.height > 1) {
    const int w = f.width / 2, h = f.height / 2;
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
      const uint8_t *r0 = &f.rgba[size_t(y * 2) * f.width * 4];
      const uint8_t *r1 = &f.rgba[size_t(y * 2 + 1) * f.width * 4];
      uint8_t *o = &out[size_t(y) * w * 4];
      for (int x = 0; x < w; x++) {
        for (int c = 0; c < 4; c++) {
          o[x * 4 + c] = uint8_t((r0[x * 8 + c] + r0[x * 8 + 4 + c] + r1[x * 8 + c] + r1[x * 8 + 4 + c] + 2) / 4);
        }
      }
    }
    f.width = w;
    f.height = h;
    f.rgba = std::move(out);
  }
}

/* Reads an image file with WIC (premultiplied RGBA, top row first). */
static bool image_read(const std::string &path, MediaFrame &r_frame, int *r_source_w, int *r_source_h,
                       std::string *r_error)
{
  com_thread();
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    *r_error = "Windows Imaging isn't available";
    return false;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder)))
  {
    *r_error = fs::exists(path_from_utf8(path)) ? "Windows can't read this image" : "The file is missing";
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) {
    *r_error = "The image has no picture in it";
    return false;
  }
  UINT w = 0, h = 0;
  frame->GetSize(&w, &h);
  ComPtr<IWICBitmapSource> converted;
  if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPRGBA, frame.Get(), &converted)) || w == 0 || h == 0) {
    *r_error = "Windows can't convert this image";
    return false;
  }
  *r_source_w = int(w);
  *r_source_h = int(h);
  r_frame.width = int(w);
  r_frame.height = int(h);
  r_frame.rgba.resize(size_t(w) * h * 4);
  if (FAILED(converted->CopyPixels(nullptr, w * 4, UINT(r_frame.rgba.size()), r_frame.rgba.data()))) {
    *r_error = "Windows can't read this image's pixels";
    return false;
  }
  fit_max_size(r_frame);
  r_frame.serial = g_frame_serial++;
  return true;
}

/* ---------------------------------------------------------------------------------
 * Media Foundation readers
 */

static int64_t to_hns(double seconds)
{
  return int64_t(std::llround(seconds * 1e7));
}

static double from_hns(int64_t hns)
{
  return double(hns) * 1e-7;
}

/* A reader for a file's first video stream, giving RGB32. */
static ComPtr<IMFSourceReader> video_reader(const std::string &path)
{
  com_thread();
  if (!g_mf_started) {
    return nullptr;
  }
  ComPtr<IMFAttributes> attrs;
  MFCreateAttributes(&attrs, 2);
  attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromURL(widen(path).c_str(), attrs.Get(), &reader))) {
    return nullptr;
  }
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  if (FAILED(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE))) {
    return nullptr;
  }
  ComPtr<IMFMediaType> type;
  MFCreateMediaType(&type);
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()))) {
    return nullptr;
  }
  return reader;
}

/* The decoded picture's layout: its size, the part shown (the display aperture: a 1080p video is
 * often decoded 1088 tall) and its row stride (negative: bottom-up). */
struct VideoLayout {
  int width = 0, height = 0;
  int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
  LONG stride = 0;
  double fps = 0.0;
};

static bool video_layout(IMFSourceReader *reader, VideoLayout &r)
{
  ComPtr<IMFMediaType> type;
  if (FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) {
    return false;
  }
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
  r.width = int(w);
  r.height = int(h);
  r.crop_w = r.width;
  r.crop_h = r.height;
  MFVideoArea area{};
  if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8 *>(&area), sizeof(area), nullptr))) {
    r.crop_x = area.OffsetX.value;
    r.crop_y = area.OffsetY.value;
    r.crop_w = std::clamp(int(area.Area.cx), 1, r.width - r.crop_x);
    r.crop_h = std::clamp(int(area.Area.cy), 1, r.height - r.crop_y);
  }
  UINT32 stride = 0;
  if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
    r.stride = LONG(int32_t(stride));
  }
  else {
    MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &r.stride);
  }
  UINT32 num = 0, den = 0;
  if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den)) && den != 0) {
    r.fps = double(num) / double(den);
  }
  return r.width > 0 && r.height > 0;
}

static double reader_duration(IMFSourceReader *reader)
{
  PROPVARIANT var;
  PropVariantInit(&var);
  double seconds = 0.0;
  if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))) {
    seconds = from_hns(int64_t(var.uhVal.QuadPart));
  }
  PropVariantClear(&var);
  return seconds;
}

/* A decoded sample as a picture (BGRX rows to premultiplied RGBA, the display aperture only). */
static bool sample_to_frame(IMFSample *sample, const VideoLayout &l, MediaFrame &r)
{
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
    return false;
  }
  BYTE *data = nullptr;
  LONG pitch = l.stride;
  ComPtr<IMF2DBuffer> buffer2d;
  bool locked2d = false;
  DWORD length = 0;
  if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&data, &pitch))) {
    locked2d = true;
  }
  else if (FAILED(buffer->Lock(&data, nullptr, &length))) {
    return false;
  }
  else if (pitch < 0) {
    /* A bottom-up buffer from Lock(): its first row is at the end. */
    data += size_t(l.height - 1) * size_t(-pitch);
  }
  r.width = l.crop_w;
  r.height = l.crop_h;
  r.rgba.resize(size_t(r.width) * r.height * 4);
  for (int y = 0; y < r.height; y++) {
    const BYTE *row = data + ptrdiff_t(y + l.crop_y) * pitch + ptrdiff_t(l.crop_x) * 4;
    uint8_t *out = &r.rgba[size_t(y) * r.width * 4];
    for (int x = 0; x < r.width; x++) {
      out[x * 4 + 0] = row[x * 4 + 2];
      out[x * 4 + 1] = row[x * 4 + 1];
      out[x * 4 + 2] = row[x * 4 + 0];
      out[x * 4 + 3] = 255;
    }
  }
  if (locked2d) {
    buffer2d->Unlock2D();
  }
  else {
    buffer->Unlock();
  }
  fit_max_size(r);
  return true;
}

/* ---------------------------------------------------------------------------------
 * Frame decoding (video and sequences), on a worker thread per source
 */

/* How much decoded picture a source keeps, and how far past an asked-for frame it decodes. */
static constexpr size_t CACHE_BYTES = size_t(768) << 20;
static constexpr int READ_AHEAD = 12;

class FrameDecoder {
 public:
  FrameDecoder(std::string path, std::vector<std::string> files, MediaInfo info)
      : path_(std::move(path)), files_(std::move(files)), info_(std::move(info))
  {
    thread_ = std::thread([this] { run(); });
  }
  ~FrameDecoder()
  {
    {
      std::lock_guard lock(m_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  MediaFramePtr get(double t, bool wait, bool *r_exact)
  {
    std::unique_lock lock(m_);
    bool exact = false;
    MediaFramePtr f = find(t, &exact);
    if (!exact) {
      /* Asked again for what is already on its way (every redraw until it comes), or for what was
       * tried and couldn't be had: left alone. */
      if (t != want_) {
        want_ = t;
        want_serial_++;
      }
      cv_.notify_all();
      if (wait) {
        const uint64_t mine = want_serial_;
        done_cv_.wait(lock, [&] {
          f = find(t, &exact);
          return exact || failed_ || (done_serial_ >= mine && !busy_);
        });
      }
    }
    if (r_exact) {
      *r_exact = exact;
    }
    return f;
  }

 private:
  /* The frame showing at `t`: exact when t falls inside it (or past the end, on the last frame). */
  MediaFramePtr find(double t, bool *r_exact)
  {
    *r_exact = false;
    if (cache_.empty()) {
      return nullptr;
    }
    const int64_t at = to_hns(t);
    auto it = cache_.upper_bound(at);
    if (it == cache_.begin()) {
      return it->second;
    }
    --it;
    const MediaFrame &f = *it->second;
    const int64_t end = it->first + std::max<int64_t>(to_hns(f.duration), 1);
    if (at < end || (end_known_ && it->first == last_ts_)) {
      *r_exact = true;
    }
    else if (auto next = std::next(it); next != cache_.end() && next->first - at < at - end) {
      return next->second;
    }
    return it->second;
  }

  void store(MediaFrame &&frame)
  {
    const int64_t ts = to_hns(frame.time);
    frame.serial = g_frame_serial++;
    auto shared = std::make_shared<const MediaFrame>(std::move(frame));
    std::lock_guard lock(m_);
    if (auto old = cache_.find(ts); old != cache_.end()) {
      bytes_ -= old->second->rgba.size();
    }
    bytes_ += shared->rgba.size();
    cache_[ts] = std::move(shared);
    /* Over the budget: the frames furthest from what is wanted go. */
    const int64_t focus = to_hns(std::max(want_, 0.0));
    while (bytes_ > CACHE_BYTES && cache_.size() > 2) {
      auto first = cache_.begin(), last = std::prev(cache_.end());
      auto drop = std::llabs(first->first - focus) > std::llabs(last->first - focus) ? first : last;
      bytes_ -= drop->second->rgba.size();
      cache_.erase(drop);
    }
    g_generation++;
    done_cv_.notify_all();
  }

  /* The next thing to decode toward, or false to stop. */
  bool next_want(double *r_t, uint64_t *r_serial)
  {
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return stop_ || want_serial_ != handled_serial_; });
    if (stop_) {
      return false;
    }
    *r_t = want_;
    *r_serial = want_serial_;
    handled_serial_ = want_serial_;
    busy_ = true;
    return true;
  }

  void finish(uint64_t serial)
  {
    std::lock_guard lock(m_);
    busy_ = false;
    done_serial_ = std::max(done_serial_, serial);
    done_cv_.notify_all();
  }

  bool superseded(uint64_t serial)
  {
    std::lock_guard lock(m_);
    return stop_ || want_serial_ != serial;
  }

  bool cached_exact(double t)
  {
    std::lock_guard lock(m_);
    bool exact = false;
    find(t, &exact);
    return exact;
  }

  void run()
  {
    com_thread();
    if (!files_.empty()) {
      run_sequence();
    }
    else {
      run_video();
    }
  }

  void run_sequence()
  {
    double t = 0.0;
    uint64_t serial = 0;
    while (next_want(&t, &serial)) {
      const int first = std::clamp(int(std::floor(t * MEDIA_SEQUENCE_RATE + 1e-6)), 0, int(files_.size()) - 1);
      for (int i = first; i < std::min(first + READ_AHEAD, int(files_.size())); i++) {
        if (i > first && superseded(serial)) {
          break;
        }
        const double at = double(i) / MEDIA_SEQUENCE_RATE;
        if (cached_exact(at)) {
          continue;
        }
        MediaFrame f;
        int sw = 0, sh = 0;
        std::string error;
        if (!image_read(files_[size_t(i)], f, &sw, &sh, &error)) {
          /* A missing or broken file in the run: the frame before it stays. */
          continue;
        }
        f.time = at;
        f.duration = 1.0 / MEDIA_SEQUENCE_RATE;
        store(std::move(f));
      }
      {
        std::lock_guard lock(m_);
        end_known_ = true;
        last_ts_ = to_hns(double(files_.size() - 1) / MEDIA_SEQUENCE_RATE);
      }
      finish(serial);
    }
  }

  void run_video()
  {
    ComPtr<IMFSourceReader> reader = video_reader(path_);
    VideoLayout layout;
    if (!reader || !video_layout(reader.Get(), layout)) {
      std::lock_guard lock(m_);
      failed_ = true;
      done_cv_.notify_all();
      return;
    }
    const double frame_time = layout.fps > 0.0 ? 1.0 / layout.fps : 1.0 / 30.0;
    /* Where the reader is: the time of the last frame it gave (-1: just seeked or opened). */
    double position = -1.0;
    bool at_end = false;
    double t = 0.0;
    uint64_t serial = 0;
    while (next_want(&t, &serial)) {
      /* Read on from where the reader is when the frame is a little ahead; otherwise seek (Media
       * Foundation goes to the keyframe before it) and decode forward. */
      const bool forward = position >= 0.0 && t >= position && t - position < 1.0 && !at_end;
      if (!forward) {
        PROPVARIANT var;
        InitPropVariantFromInt64(to_hns(std::max(t - frame_time * 0.5, 0.0)), &var);
        reader->SetCurrentPosition(GUID_NULL, var);
        PropVariantClear(&var);
        position = -1.0;
        at_end = false;
      }
      int ahead = 0;
      bool reached = false;
      while (!at_end && ahead < READ_AHEAD) {
        if (superseded(serial) && reached) {
          break;
        }
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &sample))) {
          at_end = true;
          break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
          at_end = true;
          std::lock_guard lock(m_);
          end_known_ = true;
          last_ts_ = cache_.empty() ? 0 : std::prev(cache_.end())->first;
          break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
          video_layout(reader.Get(), layout);
        }
        if (!sample) {
          continue;
        }
        LONGLONG dur = 0;
        sample->GetSampleDuration(&dur);
        MediaFrame f;
        f.time = from_hns(ts);
        f.duration = dur > 0 ? from_hns(dur) : frame_time;
        position = f.time;
        /* Frames well before the one wanted (from the keyframe on) are only passed through. */
        if (f.time + f.duration < t - frame_time * 3.0) {
          continue;
        }
        if (sample_to_frame(sample.Get(), layout, f)) {
          store(std::move(f));
        }
        if (position + f.duration > t) {
          reached = true;
          ahead++;
        }
      }
      finish(serial);
    }
  }

  std::string path_;
  std::vector<std::string> files_;
  MediaInfo info_;
  std::mutex m_;
  std::condition_variable cv_, done_cv_;
  std::map<int64_t, MediaFramePtr> cache_;
  size_t bytes_ = 0;
  double want_ = -1.0;
  uint64_t want_serial_ = 0, handled_serial_ = 0, done_serial_ = 0;
  bool busy_ = false;
  bool stop_ = false;
  bool failed_ = false;
  /* The last frame is known (the stream ended there): past it, it holds. */
  bool end_known_ = false;
  int64_t last_ts_ = 0;
  std::thread thread_;
};

/* ---------------------------------------------------------------------------------
 * Sound
 */

/* Decodes a file's first audio stream whole, to MEDIA_AUDIO_RATE stereo. */
static MediaSoundPtr sound_decode(const std::string &path)
{
  com_thread();
  if (!g_mf_started) {
    return nullptr;
  }
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromURL(widen(path).c_str(), nullptr, &reader))) {
    return nullptr;
  }
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  if (FAILED(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
    return nullptr;
  }
  ComPtr<IMFMediaType> want;
  MFCreateMediaType(&want);
  want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
  if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want.Get()))) {
    return nullptr;
  }
  ComPtr<IMFMediaType> type;
  reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &type);
  const int channels = int(MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2));
  const int rate = int(MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, MEDIA_AUDIO_RATE));
  if (channels < 1 || rate < 1) {
    return nullptr;
  }
  std::vector<float> native;
  while (true) {
    DWORD stream = 0, flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &stream, &flags, &ts, &sample)) ||
        (flags & MF_SOURCE_READERF_ENDOFSTREAM))
    {
      break;
    }
    if (!sample) {
      continue;
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
      continue;
    }
    BYTE *data = nullptr;
    DWORD length = 0;
    if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
      const float *f = reinterpret_cast<const float *>(data);
      native.insert(native.end(), f, f + length / sizeof(float));
      buffer->Unlock();
    }
  }
  const size_t native_frames = native.size() / size_t(channels);
  auto sound = std::make_shared<MediaSound>();
  /* Stereo at the mixing rate: mono goes to both sides, more than two channels keep the first two;
   * the rate changes by straight lines between samples (plenty for a reference track). */
  const double step = double(rate) / double(MEDIA_AUDIO_RATE);
  const size_t frames = native_frames == 0 ? 0 : size_t(double(native_frames) / step);
  sound->samples.resize(frames * 2);
  for (size_t i = 0; i < frames; i++) {
    const double at = double(i) * step;
    const size_t a = std::min(size_t(at), native_frames - 1), b = std::min(a + 1, native_frames - 1);
    const float u = float(at - double(a));
    for (int c = 0; c < 2; c++) {
      const int src = std::min(c, channels - 1);
      const float va = native[a * channels + src], vb = native[b * channels + src];
      sound->samples[i * 2 + c] = va + (vb - va) * u;
    }
  }
  const size_t per_peak = size_t(MEDIA_AUDIO_RATE * MEDIA_PEAK_SECONDS);
  sound->peaks.resize((frames + per_peak - 1) / per_peak);
  for (size_t p = 0; p < sound->peaks.size(); p++) {
    float peak = 0.0f;
    for (size_t i = p * per_peak; i < std::min(frames, (p + 1) * per_peak); i++) {
      peak = std::max({peak, std::fabs(sound->samples[i * 2]), std::fabs(sound->samples[i * 2 + 1])});
    }
    sound->peaks[p] = std::min(peak, 1.0f);
  }
  return sound;
}

/* ---------------------------------------------------------------------------------
 * Sources
 */

struct Source {
  std::string path;
  bool sequence = false;
  MediaInfo info;
  std::vector<std::string> files;
  /* A still image, decoded on first use. */
  bool image_read = false;
  MediaFramePtr image;
  std::unique_ptr<FrameDecoder> decoder;
  /* Sound: decoded once, on a thread of its own (dropping the source waits for it). */
  std::shared_future<MediaSoundPtr> sound;
  /* The timeline's filmstrip, likewise. */
  std::shared_future<MediaThumbsPtr> thumbs;
};

static std::mutex g_sources_mutex;
static std::map<std::string, std::shared_ptr<Source>> &sources()
{
  static std::map<std::string, std::shared_ptr<Source>> map;
  return map;
}

static std::string source_key(const std::string &path, bool sequence)
{
  return (sequence ? "seq:" : "one:") + path;
}

static MediaInfo probe_file(const std::string &path, bool sequence, std::vector<std::string> *r_files)
{
  MediaInfo info;
  const MediaKind kind = media_kind_from_path(path);
  std::error_code ec;
  if (!fs::is_regular_file(path_from_utf8(path), ec)) {
    info.error = "The file is missing";
    return info;
  }
  if (kind == MediaKind::Image) {
    MediaFrame f;
    if (!image_read(path, f, &info.source_width, &info.source_height, &info.error)) {
      return info;
    }
    info.width = f.width;
    info.height = f.height;
    info.kind = MediaKind::Image;
    if (sequence) {
      *r_files = media_sequence_files(path);
      if (r_files->empty()) {
        r_files->push_back(path);
      }
      info.kind = MediaKind::Sequence;
      info.frame_count = int(r_files->size());
      info.duration = double(info.frame_count) / MEDIA_SEQUENCE_RATE;
    }
    return info;
  }
  if (kind == MediaKind::None) {
    info.error = "Not an image, video or sound file Open Animate reads";
    return info;
  }
  if (!g_mf_started) {
    info.error = "Windows Media Foundation isn't available";
    return info;
  }
  com_thread();
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromURL(widen(path).c_str(), nullptr, &reader))) {
    info.error = "Windows can't play this file (its format or codec isn't installed)";
    return info;
  }
  info.duration = reader_duration(reader.Get());
  ComPtr<IMFMediaType> audio;
  info.has_audio = SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &audio));
  ComPtr<IMFMediaType> video;
  if (SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &video))) {
    reader.Reset();
    ComPtr<IMFSourceReader> decoding = video_reader(path);
    VideoLayout layout;
    if (!decoding || !video_layout(decoding.Get(), layout)) {
      info.error = "Windows can't decode this video (its codec isn't installed)";
      return info;
    }
    info.kind = MediaKind::Video;
    info.source_width = layout.crop_w;
    info.source_height = layout.crop_h;
    MediaFrame probe;
    probe.width = layout.crop_w;
    probe.height = layout.crop_h;
    /* The size it will be decoded at. */
    while (std::max(probe.width, probe.height) > MEDIA_MAX_SIZE && probe.width > 1 && probe.height > 1) {
      probe.width /= 2;
      probe.height /= 2;
    }
    info.width = probe.width;
    info.height = probe.height;
    info.fps = layout.fps > 0.0 ? layout.fps : 30.0;
    info.frame_count = std::max(1, int(std::lround(info.duration * info.fps)));
    return info;
  }
  if (info.has_audio) {
    info.kind = MediaKind::Audio;
    return info;
  }
  info.error = "The file has no picture or sound Windows can read";
  return info;
}

static std::shared_ptr<Source> source_get(const std::string &path, bool sequence)
{
  {
    std::lock_guard lock(g_sources_mutex);
    if (auto it = sources().find(source_key(path, sequence)); it != sources().end()) {
      return it->second;
    }
  }
  auto s = std::make_shared<Source>();
  s->path = path;
  s->sequence = sequence;
  s->info = probe_file(path, sequence, &s->files);
  std::lock_guard lock(g_sources_mutex);
  auto [it, added] = sources().emplace(source_key(path, sequence), s);
  return it->second;
}

MediaInfo media_probe(const std::string &path, bool sequence)
{
  return source_get(path, sequence)->info;
}

MediaFramePtr media_frame(const std::string &path, bool sequence, double seconds, bool wait, bool *r_exact)
{
  if (r_exact) {
    *r_exact = false;
  }
  std::shared_ptr<Source> s = source_get(path, sequence);
  switch (s->info.kind) {
    case MediaKind::Image: {
      std::lock_guard lock(g_sources_mutex);
      if (!s->image_read) {
        s->image_read = true;
        MediaFrame f;
        int w = 0, h = 0;
        std::string error;
        if (image_read(path, f, &w, &h, &error)) {
          s->image = std::make_shared<const MediaFrame>(std::move(f));
        }
      }
      if (r_exact) {
        *r_exact = s->image != nullptr;
      }
      return s->image;
    }
    case MediaKind::Sequence:
    case MediaKind::Video: {
      FrameDecoder *decoder = nullptr;
      {
        std::lock_guard lock(g_sources_mutex);
        if (!s->decoder) {
          s->decoder = std::make_unique<FrameDecoder>(path, s->info.kind == MediaKind::Sequence ? s->files
                                                                                               : std::vector<std::string>(),
                                                      s->info);
        }
        decoder = s->decoder.get();
      }
      return decoder->get(std::max(seconds, 0.0), wait, r_exact);
    }
    default:
      return nullptr;
  }
}

MediaSoundPtr media_sound(const std::string &path, bool wait)
{
  std::shared_ptr<Source> s = source_get(path, false);
  if (!s->info.has_audio) {
    return nullptr;
  }
  std::shared_future<MediaSoundPtr> sound;
  {
    std::lock_guard lock(g_sources_mutex);
    if (!s->sound.valid()) {
      s->sound = std::async(std::launch::async, [path] {
                   MediaSoundPtr decoded = sound_decode(path);
                   g_generation++;
                   return decoded;
                 }).share();
    }
    sound = s->sound;
  }
  if (!wait && sound.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return nullptr;
  }
  return sound.get();
}

/* Shrinks a picture to MEDIA_THUMB_HEIGHT tall (nearest pixel: they are small). */
static MediaFramePtr thumb_of(const MediaFrame &f, double time)
{
  auto t = std::make_shared<MediaFrame>();
  t->height = std::min(MEDIA_THUMB_HEIGHT, std::max(f.height, 1));
  t->width = std::max(1, int(std::lround(double(f.width) * t->height / std::max(f.height, 1))));
  t->rgba.resize(size_t(t->width) * t->height * 4);
  for (int y = 0; y < t->height; y++) {
    const int sy = std::min(f.height - 1, int((y + 0.5) * f.height / t->height));
    for (int x = 0; x < t->width; x++) {
      const int sx = std::min(f.width - 1, int((x + 0.5) * f.width / t->width));
      std::copy_n(&f.rgba[(size_t(sy) * f.width + sx) * 4], 4, &t->rgba[(size_t(y) * t->width + x) * 4]);
    }
  }
  t->time = time;
  t->serial = g_frame_serial++;
  return t;
}

static MediaThumbsPtr thumbs_make(const std::string &path, const MediaInfo &info, const std::vector<std::string> &files)
{
  com_thread();
  auto strip = std::make_shared<MediaThumbs>();
  /* At most 120 pictures, at most one every half second. */
  if (info.kind == MediaKind::Image) {
    MediaFrame f;
    int w = 0, h = 0;
    std::string error;
    if (image_read(path, f, &w, &h, &error)) {
      strip->frames.push_back(thumb_of(f, 0.0));
    }
    return strip;
  }
  if (info.kind == MediaKind::Sequence) {
    const int step = std::max(1, int(files.size()) / 120);
    strip->interval = double(step) / MEDIA_SEQUENCE_RATE;
    for (size_t i = 0; i < files.size(); i += size_t(step)) {
      MediaFrame f;
      int w = 0, h = 0;
      std::string error;
      if (image_read(files[i], f, &w, &h, &error)) {
        strip->frames.push_back(thumb_of(f, double(i) / MEDIA_SEQUENCE_RATE));
      }
    }
    return strip;
  }
  ComPtr<IMFSourceReader> reader = video_reader(path);
  VideoLayout layout;
  if (!reader || !video_layout(reader.Get(), layout) || info.duration <= 0.0) {
    return strip;
  }
  strip->interval = std::max(0.5, info.duration / 120.0);
  for (double t = 0.0; t < info.duration; t += strip->interval) {
    PROPVARIANT var;
    InitPropVariantFromInt64(to_hns(t), &var);
    reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    /* Decode forward to the frame at t (from the keyframe the seek went to). */
    for (int tries = 0; tries < 600; tries++) {
      DWORD stream = 0, flags = 0;
      LONGLONG ts = 0;
      ComPtr<IMFSample> sample;
      if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &sample)) ||
          (flags & MF_SOURCE_READERF_ENDOFSTREAM))
      {
        break;
      }
      LONGLONG dur = 0;
      if (!sample || (sample->GetSampleDuration(&dur), from_hns(ts + dur) <= t)) {
        continue;
      }
      MediaFrame f;
      if (sample_to_frame(sample.Get(), layout, f)) {
        strip->frames.push_back(thumb_of(f, t));
      }
      break;
    }
  }
  return strip;
}

MediaThumbsPtr media_thumbnails(const std::string &path, bool sequence)
{
  std::shared_ptr<Source> s = source_get(path, sequence);
  if (s->info.kind == MediaKind::None || s->info.kind == MediaKind::Audio) {
    return nullptr;
  }
  std::shared_future<MediaThumbsPtr> thumbs;
  {
    std::lock_guard lock(g_sources_mutex);
    if (!s->thumbs.valid()) {
      s->thumbs = std::async(std::launch::async, [path, info = s->info, files = s->files] {
                    MediaThumbsPtr made = thumbs_make(path, info, files);
                    g_generation++;
                    return made;
                  }).share();
    }
    thumbs = s->thumbs;
  }
  return thumbs.wait_for(std::chrono::seconds(0)) == std::future_status::ready ? thumbs.get() : nullptr;
}

void media_reload(const std::string &path)
{
  std::vector<std::shared_ptr<Source>> dropped;
  {
    std::lock_guard lock(g_sources_mutex);
    for (const bool sequence : {false, true}) {
      if (auto it = sources().find(source_key(path, sequence)); it != sources().end()) {
        dropped.push_back(std::move(it->second));
        sources().erase(it);
      }
    }
  }
  /* Their threads finish here, outside the lock (a sound thread takes it to finish). */
  dropped.clear();
  g_generation++;
}

void media_shutdown()
{
  std::map<std::string, std::shared_ptr<Source>> all;
  {
    std::lock_guard lock(g_sources_mutex);
    all.swap(sources());
  }
  all.clear();
  if (g_mf_started) {
    MFShutdown();
    g_mf_started = false;
  }
}

}  // namespace oanim
