#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// Audio sink using the ALSA API directly with a dedicated playback thread.
///
/// Replaces the previous aplay-pipe approach to enable:
///  - snd_pcm_delay() queries for hardware-accurate timing feedback
///  - Explicit ALSA hwparams (period / buffer size) for granular sync
///  - Decoupled playback thread so audio output never blocks network I/O
class AlsaPipeSink {
  public:
    /// Pipeline delay declared to the sync task.  With direct ALSA and
    /// snd_pcm_delay()-based timestamps the reported playback time already
    /// accounts for the hardware buffer — no extra compensation is needed.
    static constexpr int32_t PIPELINE_DELAY_US = 0;

    AlsaPipeSink() = default;
    ~AlsaPipeSink();

    AlsaPipeSink(const AlsaPipeSink&) = delete;
    AlsaPipeSink& operator=(const AlsaPipeSink&) = delete;

    /// Write PCM data in the given format into the ring buffer.  Returns bytes
    /// accepted.  Opens the ALSA device on the first write and reopens it when
    /// the format differs from the previous write's.  If the device cannot be
    /// opened, writes in that format are discarded until clear() or stop().
    /// Called from the sendspin sync task only.
    size_t write(const uint8_t* data, size_t length, uint32_t sample_rate, uint8_t channels,
                 uint8_t bits_per_sample, uint32_t timeout_ms);

    /// Stop playback, close the device, and join the playback thread.  The
    /// next write() reopens the device.
    void stop();

    /// Flush ring buffer and ALSA queue without closing the device.
    void clear();

    /// Set volume (0–100). Applied as software scaling before writing.
    void set_volume(uint8_t volume);

    /// Set mute state.
    void set_muted(bool muted);

    /// Set ALSA device name (e.g. "plughw:1,0"). Must be called before the
    /// first write(). Empty string uses the system default device.
    void set_device(const std::string& device);

    /// Callback for timing feedback — wired to player.notify_audio_played().
    std::function<void(uint32_t frames, int64_t timestamp)> on_frames_played;

  private:
    // ---- Lifecycle (configure / stop may race between the sync task and the
    //      main loop's idle timeout, so they are serialised) ----
    std::mutex lifecycle_mtx_;
    bool open_failed_{false};  // last configure failed; retry after clear()/stop()

    /// (Re)open the ALSA device for the given format.  lifecycle_mtx_ held.
    bool configure_locked(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
    void stop_locked();

    // ---- ALSA (handle stored as void* to keep <alsa/asoundlib.h> out) ----
    void* pcm_{nullptr};
    std::string device_;
    // Format of the last configure attempt; zero when the device is closed.
    uint32_t sample_rate_{0};
    uint8_t channels_{0};
    uint8_t bits_per_sample_{0};
    size_t bytes_per_frame_{0};
    unsigned long period_frames_{0};
    unsigned long buffer_frames_{0};

    bool open_alsa();
    void close_alsa();
    bool recover_xrun(int err);

    // ---- Ring buffer (decouples sendspin thread from playback thread) ----
    static constexpr size_t RING_CAPACITY = 2 * 1024 * 1024;  // 2 MiB
    std::vector<uint8_t> ring_;
    size_t ring_head_{0};
    size_t ring_tail_{0};
    size_t ring_used_{0};
    std::mutex ring_mtx_;
    std::condition_variable ring_cv_;

    // ---- Playback thread ----
    std::thread playback_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> flush_requested_{false};

    void playback_loop();

    // ---- Volume (written by network thread, read by playback thread) ----
    std::atomic<uint8_t> volume_{100};
    std::atomic<bool> muted_{false};

    void apply_volume(uint8_t* data, size_t length);
};
