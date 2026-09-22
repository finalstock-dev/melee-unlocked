// Looping H.264/MP4 backgrounds for CSS and stage select, decoded off the render thread with the
// Windows Media Foundation Source Reader. The ISO and guest simulation are never modified.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_background.h"
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
  uint64_t area = 0;
  unsigned observations = 0;
};

class Decoder {
 public:
  ~Decoder() { stop(); }

  void start(const std::filesystem::path& path) {
    if (path_ == path && worker_.joinable()) return;
    stop();
    path_ = path;
    stopping_ = false;
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
    int next = -1;
    const bool match_mode = major == 0x02 || major == 0x03 || major == 0x04 || major == 0x05 ||
                            major == 0x0F || (major >= 0x10 && major <= 0x13) ||
                            major == 0x1B || major == 0x1C;
    if ((match_mode || major == 0x08) && minor == 0) next = 0;
    else if (match_mode && minor == 1) next = 1;
    active_slot_ = next;

    for (int i = 0; i < 2; ++i) {
      Slot& s = slots_[i];
      const bool exists = enabled_ && media_foundation_ && std::filesystem::is_regular_file(s.video_path);
      has_video_[i] = exists;
      if (exists && !started_[i]) {
        s.decoder.start(s.video_path);
        started_[i] = true;
        host::log("video backgrounds: found %ls.mp4", s.stem);
      }
      s.decoder.set_active(exists && i == active_slot_);
    }

    if (active_slot_ >= 0) {
      Slot& s = slots_[active_slot_];
      if (s.target.empty() && std::filesystem::is_regular_file(s.video_path)) {
        ++s.learning_frames;
        if (s.learning_frames >= 30 && !s.candidates.empty()) {
          auto best = std::max_element(s.candidates.begin(), s.candidates.end(), [](const auto& a, const auto& b) {
            if (a.second.area != b.second.area) return a.second.area < b.second.area;
            return a.second.observations < b.second.observations;
          });
          s.target = best->first;
          std::ofstream output(s.target_path, std::ios::trunc);
          output << s.target << "\n";
          host::log("video backgrounds: learned %ls target %s", s.stem, s.target.c_str());
          s.candidates.clear();
        }
      }
    }
  }

  bool wants_names() const {
    return enabled_ && active_slot_ >= 0 && has_video_[active_slot_];
  }

  std::shared_ptr<const Frame> lookup_frame(const std::string& base, uint32_t width,
                                            uint32_t height, int* out_slot) {
    if (!wants_names()) return {};
    Slot& s = slots_[active_slot_];
    if (s.target.empty()) {
      // Portraits are 136x188. Requiring at least 192 in both dimensions keeps them, icons and
      // labels out of automatic selection; the menu backdrop is larger and persists every frame.
      if (width >= 192 && height >= 192) {
        Candidate& c = s.candidates[base];
        c.area = std::max(c.area, (uint64_t)width * height);
        ++c.observations;
      }
      return {};
    }
    if (base != s.target) return {};
    if (out_slot) *out_slot = active_slot_;
    return s.decoder.frame();
  }

  void set_enabled(bool value) {
    enabled_ = value;
    if (!enabled_) for (Slot& s : slots_) s.decoder.set_active(false);
  }

  void relearn_slot(int i) {
    if (i < 0 || i > 1) return;
    Slot& s = slots_[i];
    s.target.clear();
    s.candidates.clear();
    s.learning_frames = 0;
    std::error_code ec;
    std::filesystem::remove(s.target_path, ec);
    host::log("video backgrounds: %ls target cleared; visit the screen for half a second to relearn", s.stem);
  }

  std::string slot_status(int i) const {
    if (i < 0 || i > 1) return "invalid";
    const Slot& s = slots_[i];
    if (!std::filesystem::is_regular_file(s.video_path)) return "No MP4";
    const std::string error = s.decoder.error();
    if (!error.empty()) return error;
    if (s.target.empty()) return active_slot_ == i ? "Learning background..." : "Visit this screen to learn";
    return "Ready: " + s.target;
  }

  void open() {
    std::filesystem::create_directories(root_);
    ShellExecuteW(nullptr, L"open", root_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }

  bool is_enabled() const { return enabled_; }

 private:
  std::filesystem::path root_;
  Slot slots_[2];
  bool media_foundation_ = false;
  bool enabled_ = true;
  bool started_[2]{};
  bool has_video_[2]{};
  int active_slot_ = -1;
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
void set_enabled(bool value) { manager().set_enabled(value); }
bool enabled() { return manager().is_enabled(); }
void open_folder() { manager().open(); }
void relearn(int slot) { manager().relearn_slot(slot); }
std::string status(int slot) { return manager().slot_status(slot); }

}  // namespace gx::video_bg
