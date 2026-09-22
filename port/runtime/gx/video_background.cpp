// Looping H.264/MP4 backgrounds for CSS and stage select, decoded off the render thread with the
// Windows Media Foundation Source Reader. The ISO and guest simulation are never modified.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_background.h"
#include "video_background_learning.h"
#include "video_background_style.h"
#include "../host/host.h"
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace gx::video_bg {
namespace {

std::filesystem::path exe_directory() {
  wchar_t path[32768]{};
  DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
  if (!n || n >= std::size(path)) return std::filesystem::current_path();
  return std::filesystem::path(path).parent_path();
}

struct Candidate {
  uint32_t width = 0, height = 0;
  unsigned observations = 0;
  unsigned distinct_frames = 0;
  unsigned last_learning_frame = ~0u;
  bool persistence_logged = false;
};

class Decoder {
 public:
  ~Decoder() { stop(); }

  void start(const std::filesystem::path& path, bool sss_matte) {
    if (path_ == path && worker_.joinable()) return;
    stop();
    path_ = path;
    sss_matte_ = sss_matte;
    stopping_ = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      error_.clear();
    }
    worker_ = std::thread([this] { run(); });
  }

  void stop() {
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    frame_.reset();
    path_.clear();
  }

  void set_active(bool active) {
    active_ = active;
    cv_.notify_all();
  }

  std::shared_ptr<const Frame> frame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_;
  }

  std::string error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

 private:
  static std::string hr_text(const char* what, HRESULT hr) {
    char out[160];
    std::snprintf(out, sizeof out, "%s (HRESULT %08lX)", what, (unsigned long)hr);
    return out;
  }

  bool wait_active() {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    cv_.wait(lock, [&] { return stopping_.load() || active_.load(); });
    return !stopping_;
  }

  bool wait_until(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    cv_.wait_until(lock, deadline, [&] { return stopping_.load() || !active_.load(); });
    return !stopping_ && active_;
  }

  void set_error(std::string text) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::move(text);
  }

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 3);
    if (SUCCEEDED(hr)) hr = attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path_.c_str(), attrs.Get(), &reader);
    if (FAILED(hr)) {
      set_error(hr_text("cannot open MP4", hr));
      CoUninitialize();
      return;
    }

    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> type;
    hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                        nullptr, type.Get());
    if (FAILED(hr)) {
      set_error(hr_text("unsupported video format", hr));
      CoUninitialize();
      return;
    }

    ComPtr<IMFMediaType> actual;
    UINT32 width = 0, height = 0;
    hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
    if (SUCCEEDED(hr)) hr = MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr) || !width || !height || width > 3840 || height > 2160) {
      set_error("video dimensions are missing or exceed 3840x2160");
      CoUninitialize();
      return;
    }
    LONG stride = (LONG)width * 4;
    actual->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride));

    std::vector<uint8_t> sss_mask;
    if (sss_matte_) {
      sss_mask.resize((size_t)width * height);
      for (UINT32 y = 0; y < height; ++y)
        for (UINT32 x = 0; x < width; ++x)
          sss_mask[(size_t)y * width + x] = style::sss_matte_alpha(x, y, width, height);
    }

    uint64_t serial = 0;
    while (!stopping_) {
      if (!wait_active()) break;
      PROPVARIANT pos;
      PropVariantInit(&pos);
      pos.vt = VT_I8;
      pos.hVal.QuadPart = 0;
      reader->SetCurrentPosition(GUID_NULL, pos);
      PropVariantClear(&pos);
      const auto wall_start = std::chrono::steady_clock::now();
      LONGLONG first_time = -1;

      while (!stopping_ && active_) {
        DWORD flags = 0;
        LONGLONG sample_time = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags,
                                &sample_time, &sample);
        if (FAILED(hr)) { set_error(hr_text("video decode failed", hr)); break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        if (first_time < 0) first_time = sample_time;
        const auto deadline = wall_start + std::chrono::nanoseconds((sample_time - first_time) * 100);
        if (!wait_until(deadline)) break;

        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        BYTE* bytes = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(hr)) hr = buffer->Lock(&bytes, nullptr, &length);
        if (FAILED(hr)) { set_error(hr_text("cannot read decoded frame", hr)); break; }

        auto next = std::make_shared<Frame>();
        next->width = width;
        next->height = height;
        next->serial = ++serial;
        next->bgra.resize((size_t)width * height * 4);
        const size_t row_bytes = (size_t)width * 4;
        const size_t source_stride = (size_t)(stride < 0 ? -stride : stride);
        if (source_stride >= row_bytes && source_stride * height <= length) {
          for (UINT32 y = 0; y < height; ++y) {
            const UINT32 source_y = stride < 0 ? y : height - 1 - y;
            std::memcpy(next->bgra.data() + (size_t)y * row_bytes,
                        bytes + (size_t)source_y * source_stride, row_bytes);
          }
          // RGB32's unused high byte is not guaranteed to be opaque. The guest background may
          // sample texture alpha in its TEV stages, so normalize it instead of inheriting zeros.
          for (size_t p = 3; p < next->bgra.size(); p += 4) next->bgra[p] = 255;
          if (sss_matte_) {
            // Match the supplied Sky SSS reference: retain motion at the perimeter while a dark
            // slate panel protects the stage icons and labels. This runs on the decoder thread;
            // the render thread still only uploads the latest completed frame.
            constexpr uint8_t matte_bgr[3] = {76, 55, 45};
            for (size_t pixel = 0; pixel < sss_mask.size(); ++pixel) {
              const unsigned a = sss_mask[pixel];
              if (!a) continue;
              uint8_t* bgra = next->bgra.data() + pixel * 4;
              for (int channel = 0; channel < 3; ++channel)
                bgra[channel] = (uint8_t)((bgra[channel] * (255 - a) +
                                           matte_bgr[channel] * a + 127) / 255);
            }
          }
        } else {
          next.reset();
          set_error("decoded frame has an invalid stride");
        }
        buffer->Unlock();
        if (next) {
          std::lock_guard<std::mutex> lock(mutex_);
          frame_ = std::move(next);
          error_.clear();
        }
      }
      // EOS loops immediately while the slot is active. A decode error pauses briefly so a bad
      // file cannot spin a CPU core and flood the log.
      if (FAILED(hr)) {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return stopping_.load(); });
      }
    }
    CoUninitialize();
  }

  std::filesystem::path path_;
  bool sss_matte_ = false;
  mutable std::mutex mutex_;
  std::mutex wait_mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  std::atomic<bool> stopping_{false}, active_{false};
  std::shared_ptr<const Frame> frame_;
  std::string error_;
};

struct Slot {
  const wchar_t* stem = nullptr;
  std::filesystem::path video_path, target_path;
  std::string target;
  std::map<std::string, Candidate> candidates;
  unsigned learning_frames = 0;
  unsigned saved_target_frames = 0;
  unsigned candidate_logs = 0;
  bool target_confirmed = false;
  bool learning_failed = false;
  bool replacement_logged = false;
  std::string backend_error;
  Decoder decoder;
};

class Manager {
 public:
  Manager() {
    root_ = exe_directory() / "VideoBackgrounds";
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    slots_[0].stem = L"css";
    slots_[1].stem = L"sss";
    for (Slot& s : slots_) {
      s.video_path = root_ / (std::wstring(s.stem) + L".mp4");
      s.target_path = root_ / (std::wstring(s.stem) + L".target");
      std::ifstream input(s.target_path);
      std::getline(input, s.target);
      if (!s.target.empty() && s.target.rfind("tex1_", 0) != 0) s.target.clear();
    }
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    media_foundation_ = SUCCEEDED(hr);
    if (!media_foundation_) host::log("video backgrounds: Media Foundation startup failed (%08lX)", (unsigned long)hr);
  }

  ~Manager() {
    for (Slot& s : slots_) s.decoder.stop();
    if (media_foundation_) MFShutdown();
  }

  void begin(uint8_t major, uint8_t minor) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int next = learning::scene_slot(major, minor);
    const uint16_t scene = (uint16_t)((uint16_t)major << 8 | minor);
    if (scene != last_scene_) {
      last_scene_ = scene;
      if (scene_logs_ < 64) {
        const char* detected = next == 0 ? "CSS" : next == 1 ? "SSS" : "none";
        host::log("video backgrounds: scene major=%02X minor=%02X detected=%s",
                  major, minor, detected);
        if (++scene_logs_ == 64)
          host::log("video backgrounds: further scene diagnostics suppressed");
      }
    }
    active_slot_ = next;

    for (int i = 0; i < 2; ++i) {
      Slot& s = slots_[i];
      const bool exists = enabled_ && media_foundation_ && std::filesystem::is_regular_file(s.video_path);
      has_video_[i] = exists;
      if (exists && !started_[i]) {
        s.decoder.start(s.video_path, i == 1);
        started_[i] = true;
        host::log("video backgrounds: found %ls.mp4", s.stem);
      }
      s.decoder.set_active(exists && i == active_slot_);
    }

    if (active_slot_ >= 0) {
      Slot& s = slots_[active_slot_];
      if (!s.target.empty() && !s.target_confirmed && has_video_[active_slot_]) {
        if (++s.saved_target_frames >= learning::kSavedTargetTimeoutFrames) {
          host::log("video backgrounds: saved %ls target %s was not observed and is stale; relearning",
                    s.stem, s.target.c_str());
          s.target.clear();
          s.saved_target_frames = 0;
          s.learning_frames = 0;
          std::error_code ec;
          std::filesystem::remove(s.target_path, ec);
        }
      }
      if (s.target.empty() && has_video_[active_slot_] && !s.learning_failed) {
        ++s.learning_frames;
        if (s.learning_frames == learning::kDecisionFrames)
          try_learning(s, false);
        if (s.learning_frames >= learning::kTimeoutFrames && s.target.empty())
          try_learning(s, true);
      }
    }
  }

  bool wants_names() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return wants_names_locked();
  }

  std::shared_ptr<const Frame> lookup_frame(const std::string& base, uint32_t width,
                                            uint32_t height, int* out_slot) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!wants_names_locked()) return {};
    Slot& s = slots_[active_slot_];
    if (s.target.empty() || !s.target_confirmed) record_candidate(s, base, width, height);
    if (!s.target.empty() && base == s.target && !s.target_confirmed) {
      s.target_confirmed = true;
      s.saved_target_frames = 0;
      host::log("video backgrounds: confirmed saved %ls target %s (%ux%u)",
                s.stem, s.target.c_str(), width, height);
    }
    if (s.target.empty() || !s.backend_error.empty()) {
      return {};
    }
    if (base != s.target) return {};
    // CSS's largest persistent texture is the four player-card atlas, not its background. The
    // actual backdrop is a long run of untextured 3D draws, so substituting this atlas corrupts
    // portraits and panels. CSS is composited by the backend as a full-screen layer instead.
    if (!learning::uses_texture_target(active_slot_)) return {};
    if (out_slot) *out_slot = active_slot_;
    std::shared_ptr<const Frame> decoded = s.decoder.frame();
    if (decoded && !s.replacement_logged) {
      s.replacement_logged = true;
      host::log("video backgrounds: applying %ls video to %s (%ux%u video frame)",
                s.stem, s.target.c_str(), decoded->width, decoded->height);
    }
    return decoded;
  }

  std::shared_ptr<const Frame> fullscreen(int* out_slot) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!wants_names_locked() || active_slot_ != 0) return {};
    Slot& s = slots_[active_slot_];
    if (!s.backend_error.empty()) return {};
    std::shared_ptr<const Frame> decoded = s.decoder.frame();
    if (decoded && out_slot) *out_slot = active_slot_;
    if (decoded && !s.replacement_logged) {
      s.replacement_logged = true;
      host::log("video backgrounds: decoded %ls full-screen frame (%ux%u)",
                s.stem, decoded->width, decoded->height);
    }
    return decoded;
  }

  void set_enabled(bool value) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    enabled_ = value;
    if (!enabled_) for (Slot& s : slots_) s.decoder.set_active(false);
  }

  void relearn_slot(int i) {
    if (i < 0 || i > 1) return;
    std::lock_guard<std::mutex> lock(state_mutex_);
    Slot& s = slots_[i];
    s.target.clear();
    s.candidates.clear();
    s.learning_frames = 0;
    s.saved_target_frames = 0;
    s.candidate_logs = 0;
    s.target_confirmed = false;
    s.learning_failed = false;
    s.replacement_logged = false;
    s.backend_error.clear();
    std::error_code ec;
    std::filesystem::remove(s.target_path, ec);
    host::log("video backgrounds: %ls target cleared; visit the screen for half a second to relearn", s.stem);
  }

  std::string slot_status(int i) const {
    if (i < 0 || i > 1) return "invalid";
    std::lock_guard<std::mutex> lock(state_mutex_);
    const Slot& s = slots_[i];
    if (!std::filesystem::is_regular_file(s.video_path)) return "No MP4";
    if (!enabled_) return "Disabled — vanilla background active";
    const std::string error = s.decoder.error();
    if (!error.empty()) return error;
    if (!s.backend_error.empty()) return s.backend_error + " — using vanilla";
    if (i == 0 && s.decoder.frame()) return "Ready: full-screen video layer";
    if (!s.target.empty() && !s.target_confirmed)
      return active_slot_ == i ? "Checking saved target..." : "Visit this screen to verify saved target";
    if (s.learning_failed) {
      return s.candidates.empty() ?
          "Automatic learning timed out — no textures observed" :
          "Automatic learning timed out — choose an observed texture below";
    }
    if (s.target.empty()) {
      if (active_slot_ != i) return "Visit this screen to learn";
      return "Learning background... " + std::to_string(s.learning_frames) + "/" +
             std::to_string(learning::kTimeoutFrames) + " frames (" +
             std::to_string(s.candidates.size()) + " textures)";
    }
    return "Ready: " + s.target;
  }

  std::string slot_target(int i) const {
    if (i < 0 || i > 1) return {};
    std::lock_guard<std::mutex> lock(state_mutex_);
    return slots_[i].target;
  }

  std::vector<ObservedTexture> observations(int i) const {
    std::vector<ObservedTexture> result;
    if (i < 0 || i > 1) return result;
    std::lock_guard<std::mutex> lock(state_mutex_);
    const Slot& s = slots_[i];
    for (const auto& item : s.candidates) {
      const Candidate& c = item.second;
      result.push_back({item.first, c.width, c.height, c.observations, c.distinct_frames,
                        learning::candidate_score(c.width, c.height, c.distinct_frames,
                                                  c.observations)});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
      if (a.score != b.score) return a.score > b.score;
      if (a.distinct_frames != b.distinct_frames) return a.distinct_frames > b.distinct_frames;
      return a.name < b.name;
    });
    return result;
  }

  bool select_target(int i, const std::string& name) {
    if (i < 0 || i > 1) return false;
    std::lock_guard<std::mutex> lock(state_mutex_);
    Slot& s = slots_[i];
    auto found = s.candidates.find(name);
    if (found == s.candidates.end()) return false;
    s.target = name;
    s.target_confirmed = true;
    s.learning_failed = false;
    s.replacement_logged = false;
    s.backend_error.clear();
    s.saved_target_frames = 0;
    write_target(s);
    host::log("video backgrounds: manually selected %ls target %s (%ux%u, observations=%u, frames=%u)",
              s.stem, name.c_str(), found->second.width, found->second.height,
              found->second.observations, found->second.distinct_frames);
    return true;
  }

  void backend_failure(int i, const std::string& message) {
    if (i < 0 || i > 1) return;
    std::lock_guard<std::mutex> lock(state_mutex_);
    Slot& s = slots_[i];
    if (s.backend_error.empty())
      host::log("video backgrounds: %ls backend failure: %s; using vanilla",
                s.stem, message.c_str());
    s.backend_error = message;
  }

  void open() {
    std::filesystem::create_directories(root_);
    ShellExecuteW(nullptr, L"open", root_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }

  bool is_enabled() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return enabled_;
  }

 private:
  bool wants_names_locked() const {
    return learning::should_route_video(
        enabled_, active_slot_, active_slot_ >= 0 && has_video_[active_slot_]);
  }

  static uint64_t score(const Candidate& c) {
    return learning::candidate_score(c.width, c.height, c.distinct_frames, c.observations);
  }

  void record_candidate(Slot& s, const std::string& base, uint32_t width, uint32_t height) {
    if (base.empty() || !width || !height || width > 4096 || height > 4096) return;
    auto found = s.candidates.find(base);
    if (found == s.candidates.end()) {
      if (s.candidates.size() >= learning::kMaximumCandidates) {
        auto weakest = std::min_element(s.candidates.begin(), s.candidates.end(),
            [](const auto& a, const auto& b) { return score(a.second) < score(b.second); });
        Candidate incoming{}; incoming.width = width; incoming.height = height;
        incoming.observations = incoming.distinct_frames = 1;
        if (weakest != s.candidates.end() && score(incoming) <= score(weakest->second)) return;
        if (weakest != s.candidates.end()) s.candidates.erase(weakest);
      }
      Candidate candidate{};
      candidate.width = width; candidate.height = height;
      found = s.candidates.emplace(base, candidate).first;
      if (s.candidate_logs < 24) {
        host::log("video backgrounds: observed %ls texture %s %ux%u observations=1 frames=1",
                  s.stem, base.c_str(), width, height);
        ++s.candidate_logs;
      }
    }
    Candidate& c = found->second;
    c.width = width; c.height = height;
    ++c.observations;
    if (c.last_learning_frame != s.learning_frames) {
      c.last_learning_frame = s.learning_frames;
      ++c.distinct_frames;
    }
    if (!c.persistence_logged && c.distinct_frames == learning::kMinimumPersistentFrames &&
        s.candidate_logs < 32) {
      c.persistence_logged = true;
      host::log("video backgrounds: persistent %ls candidate %s %ux%u observations=%u frames=%u score=%llu",
                s.stem, base.c_str(), width, height, c.observations, c.distinct_frames,
                (unsigned long long)score(c));
      ++s.candidate_logs;
    }
  }

  void log_best(const Slot& s, const char* reason) const {
    std::vector<std::pair<std::string, Candidate>> ranked(s.candidates.begin(), s.candidates.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
      if (score(a.second) != score(b.second)) return score(a.second) > score(b.second);
      return a.first < b.first;
    });
    host::log("video backgrounds: %ls %s after %u frames; observed=%zu; best candidates follow",
              s.stem, reason, s.learning_frames, s.candidates.size());
    for (size_t i = 0; i < std::min<size_t>(8, ranked.size()); ++i) {
      const Candidate& c = ranked[i].second;
      host::log("video backgrounds: %ls candidate #%zu %s %ux%u observations=%u frames=%u score=%llu",
                s.stem, i + 1, ranked[i].first.c_str(), c.width, c.height, c.observations,
                c.distinct_frames, (unsigned long long)score(c));
    }
  }

  void write_target(const Slot& s) const {
    std::ofstream output(s.target_path, std::ios::trunc);
    if (output) output << s.target << "\n";
  }

  void try_learning(Slot& s, bool final_attempt) {
    log_best(s, final_attempt ? "learning timeout" : "learning checkpoint");
    auto best = std::max_element(s.candidates.begin(), s.candidates.end(),
        [](const auto& a, const auto& b) { return score(a.second) < score(b.second); });
    if (best != s.candidates.end() && learning::confident(
            best->second.width, best->second.height, best->second.distinct_frames,
            s.learning_frames, best->second.observations)) {
      s.target = best->first;
      s.target_confirmed = true;
      s.learning_failed = false;
      write_target(s);
      host::log("video backgrounds: learned %ls target %s %ux%u observations=%u frames=%u score=%llu",
                s.stem, s.target.c_str(), best->second.width, best->second.height,
                best->second.observations, best->second.distinct_frames,
                (unsigned long long)score(best->second));
      return;
    }
    if (final_attempt) {
      s.learning_failed = true;
      host::log("video backgrounds: %ls automatic learning timed out; choose an observed texture in PC settings",
                s.stem);
    }
  }

  std::filesystem::path root_;
  Slot slots_[2];
  mutable std::mutex state_mutex_;
  bool media_foundation_ = false;
  bool enabled_ = true;
  bool started_[2]{};
  bool has_video_[2]{};
  int active_slot_ = -1;
  uint16_t last_scene_ = 0xffff;
  unsigned scene_logs_ = 0;
};

Manager& manager() {
  static Manager value;
  return value;
}

}  // namespace

void begin_frame(uint8_t major, uint8_t minor) { manager().begin(major, minor); }
bool wants_texture_names() { return manager().wants_names(); }
std::shared_ptr<const Frame> lookup(const std::string& base, uint32_t width, uint32_t height,
                                    int* slot) {
  return manager().lookup_frame(base, width, height, slot);
}
std::shared_ptr<const Frame> fullscreen_frame(int* slot) {
  return manager().fullscreen(slot);
}
void set_enabled(bool value) { manager().set_enabled(value); }
bool enabled() { return manager().is_enabled(); }
void open_folder() { manager().open(); }
void relearn(int slot) { manager().relearn_slot(slot); }
std::string status(int slot) { return manager().slot_status(slot); }
std::string target(int slot) { return manager().slot_target(slot); }
std::vector<ObservedTexture> observed_textures(int slot) { return manager().observations(slot); }
bool choose_target(int slot, const std::string& name) {
  return manager().select_target(slot, name);
}
void report_backend_failure(int slot, const std::string& message) {
  manager().backend_failure(slot, message);
}

}  // namespace gx::video_bg
