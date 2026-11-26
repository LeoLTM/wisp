/// @file wispd: headless Sendspin client daemon for ARMv6 (Raspberry Pi Zero W).
///
/// Connects to a Sendspin server and plays audio through the 3.5mm jack
/// via aplay (ALSA).  Designed to run as a systemd service.

#include "sendspin/client.h"
#include "sendspin/metadata_role.h"
#include "sendspin/player_role.h"

#include "alsa_pipe_sink.h"
#include "config.h"
#include "file_persistence.h"
#include "oled_display.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

using namespace sendspin;

static constexpr const char* DEFAULT_CONFIG_PATH = "/etc/wispd.conf";
static constexpr const char* VERSION = "0.2.0";

static std::atomic<bool> running{true};

static void signal_handler(int /*sig*/) { running.store(false); }

static bool parse_log_level(const std::string& str, LogLevel& level) {
    if (str == "none") { level = LogLevel::NONE; return true; }
    if (str == "error") { level = LogLevel::ERROR; return true; }
    if (str == "warn") { level = LogLevel::WARN; return true; }
    if (str == "info") { level = LogLevel::INFO; return true; }
    if (str == "debug") { level = LogLevel::DEBUG; return true; }
    if (str == "verbose") { level = LogLevel::VERBOSE; return true; }
    return false;
}

static const char* codec_name(SendspinCodecFormat codec) {
    switch (codec) {
        case SendspinCodecFormat::FLAC: return "FLAC";
        case SendspinCodecFormat::OPUS: return "Opus";
        case SendspinCodecFormat::PCM: return "PCM";
        default: return "";
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c PATH   Config file (default: %s)\n", DEFAULT_CONFIG_PATH);
    fprintf(stderr, "  -t SECS   Idle timeout in seconds before releasing audio device (0=disable)\n");
    fprintf(stderr, "  -h        Show this help\n");
    fprintf(stderr, "  -V        Show version\n");
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse command line
    std::string config_path = DEFAULT_CONFIG_PATH;
    int idle_timeout_override = -1;
    int opt;
    while ((opt = getopt(argc, argv, "c:t:hV")) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 't': idle_timeout_override = std::atoi(optarg); break;
            case 'h': print_usage(argv[0]); return 0;
            case 'V': fprintf(stdout, "wispd %s\n", VERSION); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    // Load configuration
    Config cfg;
    if (!load_config(config_path, cfg)) {
        return 1;
    }

    if (idle_timeout_override >= 0) {
        cfg.idle_timeout_s = idle_timeout_override;
    }

    if (cfg.server_url.empty()) {
        fprintf(stderr, "Error: server_url is required in %s\n", config_path.c_str());
        return 1;
    }
    if (cfg.state_dir.empty()) {
        fprintf(stderr, "Error: state_dir must not be empty in %s\n", config_path.c_str());
        return 1;
    }

    // Apply log level
    LogLevel log_level = LogLevel::INFO;
    if (!parse_log_level(cfg.log_level, log_level)) {
        fprintf(stderr, "Warning: unknown log level '%s', using 'info'\n",
                cfg.log_level.c_str());
    }
    SendspinClient::set_log_level(log_level);

    // --- STARTUP LOGGING BLOCK ---
    fprintf(stderr, "\n");
    fprintf(stderr, "=================================================================\n");
    fprintf(stderr, " wisp (wispd) - Version: %s\n", VERSION);
    fprintf(stderr, " Build Date: %s %s\n", __DATE__, __TIME__);
    fprintf(stderr, "=================================================================\n");
    fprintf(stderr, " Active Configuration:\n");
    fprintf(stderr, "  Config File   : %s\n", config_path.c_str());
    fprintf(stderr, "  Friendly Name : %s\n", cfg.name.c_str());
    fprintf(stderr, "  Audio Device  : %s\n", cfg.device.empty() ? "System Default" : cfg.device.c_str());
    fprintf(stderr, "  Server URL    : %s\n", cfg.server_url.c_str());
    fprintf(stderr, "  Idle Timeout  : %s\n", cfg.idle_timeout_s == 0 ? "inaktiv" : (std::to_string(cfg.idle_timeout_s) + " seconds").c_str());
    fprintf(stderr, "  State Dir     : %s\n", cfg.state_dir.c_str());
    fprintf(stderr, "  Unpaired Play : %s\n", cfg.unpaired_access ? "allowed" : "pairing required");
    if (cfg.display == "none") {
        fprintf(stderr, "  Display       : disabled\n");
    } else {
        fprintf(stderr, "  Display       : %s 128x%d, /dev/i2c-%d, address 0x%02X\n",
                cfg.display.c_str(), cfg.display_height, cfg.display_i2c_bus,
                cfg.display_i2c_address);
    }
    
    const char* log_level_str = "info";
    switch (log_level) {
        case LogLevel::NONE: log_level_str = "none"; break;
        case LogLevel::ERROR: log_level_str = "error"; break;
        case LogLevel::WARN: log_level_str = "warn"; break;
        case LogLevel::INFO: log_level_str = "info"; break;
        case LogLevel::DEBUG: log_level_str = "debug"; break;
        case LogLevel::VERBOSE: log_level_str = "verbose"; break;
    }
    fprintf(stderr, "  Log Level     : %s\n", log_level_str);
    fprintf(stderr, "=================================================================\n\n");
    // --- END STARTUP LOGGING BLOCK ---

    // The client_id is derived by sendspin-cpp from an X25519 keypair generated
    // on first start.  The keypair and the pairing records live in state_dir,
    // so the device keeps its identity and pairings across restarts.
    FilePersistence persistence(cfg.state_dir);
    if (!persistence.init()) {
        return 1;
    }

    // Optional now-playing OLED.  Created before the client so it outlives
    // every callback that can reach it.
    std::unique_ptr<OledDisplay> display;
    if (cfg.display != "none") {
        OledConfig oled_config;
        oled_config.controller = cfg.display == "sh1106" ? Ssd1306::Controller::SH1106
                                                         : Ssd1306::Controller::SSD1306;
        oled_config.i2c_bus = cfg.display_i2c_bus;
        oled_config.i2c_address = cfg.display_i2c_address;
        oled_config.height = cfg.display_height;
        oled_config.rotate180 = cfg.display_rotate == 180;
        oled_config.contrast = static_cast<uint8_t>(cfg.display_contrast);
        oled_config.sleep_s = cfg.display_sleep_s;
        display = std::make_unique<OledDisplay>(oled_config, cfg.name);
    }

    // Configure client
    SendspinClientConfig client_config;
    client_config.name = cfg.name;
    client_config.product_name = "wisp";
    client_config.manufacturer = "wisp";
    client_config.software_version = VERSION;

    SendspinClient client(std::move(client_config));
    client.set_persistence_provider(&persistence);
    client.set_unpaired_access_enabled(cfg.unpaired_access);

    // Add player role with supported audio formats
    PlayerRoleConfig player_config;
    player_config.audio_formats = {
        {SendspinCodecFormat::FLAC, 2, 44100, 16},
        {SendspinCodecFormat::FLAC, 2, 48000, 16},
        {SendspinCodecFormat::OPUS, 2, 48000, 16},
        {SendspinCodecFormat::PCM, 2, 44100, 16},
        {SendspinCodecFormat::PCM, 2, 48000, 16},
    };
    // Larger ring buffer absorbs CPU scheduling jitter on the single-core ARMv6
    // without dropping encoded chunks before the sync task can decode them.
    player_config.audio_buffer_capacity = 2000000;
    // With direct ALSA and snd_pcm_delay()-based timestamps, the reported
    // playback time already accounts for the hardware buffer.  No additional
    // fixed delay compensation is required.
    player_config.fixed_delay_us = AlsaPipeSink::PIPELINE_DELAY_US;
    if (cfg.initial_output_delay_ms >= 0) {
        // Only used until the server sets a delay; that one is persisted.
        player_config.initial_output_delay_ms =
            static_cast<uint16_t>(cfg.initial_output_delay_ms);
    }
    auto& player = client.add_player(std::move(player_config));

    // Track metadata is only requested from the server when a display will
    // show it; headless players skip the role entirely.  The display shows
    // title, artist and album, so the other text fields are not stored.
    MetadataRole* metadata =
        display ? &client.add_metadata({.album_artist = false, .artwork_url = false}) : nullptr;

    // Audio output via aplay pipe
    AlsaPipeSink audio_sink;
    if (!cfg.device.empty()) {
        audio_sink.set_device(cfg.device);
    }

    // --- Listener implementations ---

    struct ArmPlayerListener : PlayerRoleListener {
        bool stream_active{false};
        bool device_open{false};
        std::chrono::steady_clock::time_point last_stream_end_time{std::chrono::steady_clock::now()};
        int idle_timeout_s{0};

        AlsaPipeSink& sink;
        OledDisplay* display;  // null when the display is disabled
        ArmPlayerListener(AlsaPipeSink& s, int timeout_s, OledDisplay* d)
            : idle_timeout_s(timeout_s), sink(s), display(d) {}

        // Runs on the sync task.  Each write carries the format of exactly
        // these bytes; the sink (re)opens ALSA whenever it changes, which
        // also covers an in-place stream/start with a new sample rate.
        size_t on_audio_output(std::span<const uint8_t> pcm, const AudioFormat& format,
                               uint32_t timeout_ms) override {
            return sink.write(pcm.data(), pcm.size(), format.sample_rate, format.channels,
                              format.bits_per_sample, timeout_ms);
        }

        void on_stream_start() override {
            fprintf(stderr, ">>> Stream started\n");
            this->stream_active = true;
            this->device_open = true;
            if (display) display->set_stream_active(true);
        }

        void on_stream_end() override {
            fprintf(stderr, ">>> Stream ended\n");
            this->stream_active = false;
            this->last_stream_end_time = std::chrono::steady_clock::now();
            if (display) display->set_stream_active(false);

            if (this->idle_timeout_s > 0) {
                fprintf(stderr, ">>> Audio device will be released in %d seconds if idle\n", this->idle_timeout_s);
            }
            sink.clear();
        }

        void on_volume_changed(uint8_t vol) override {
            sink.set_volume(vol);
            if (display) display->set_volume(vol);
        }
        void on_mute_changed(bool muted) override {
            sink.set_muted(muted);
            if (display) display->set_muted(muted);
        }
        void on_output_delay_changed(uint16_t delay_ms) override {
            fprintf(stderr, ">>> Output delay changed to %u ms\n", delay_ms);
        }
    };

    struct ArmClientListener : SendspinClientListener {
        void on_time_sync_updated(float error) override {
            if (SendspinClient::get_log_level() >= LogLevel::DEBUG) {
                fprintf(stderr, ">>> Time sync error: %.1f us\n", error);
            }
        }

        void on_pairing_attempt(std::string_view server_id) override {
            fprintf(stderr, ">>> Pairing started with server %.*s\n",
                    static_cast<int>(server_id.size()), server_id.data());
        }
        void on_paired(std::string_view server_id) override {
            fprintf(stderr, ">>> Paired with server %.*s\n",
                    static_cast<int>(server_id.size()), server_id.data());
        }
        void on_pairing_aborted(std::string_view server_id,
                                SendspinPairAbortReason reason) override {
            fprintf(stderr, ">>> Pairing with server %.*s aborted (reason %d)\n",
                    static_cast<int>(server_id.size()), server_id.data(),
                    static_cast<int>(reason));
        }
    };

    struct ArmMetadataListener : MetadataRoleListener {
        OledDisplay& display;
        explicit ArmMetadataListener(OledDisplay& d) : display(d) {}

        // Text arrives as string_views valid only during the callback; the
        // display copies it.
        void on_track_metadata(const TrackMetadata& m) override {
            display.set_track(std::string(m.title.value_or("")),
                              std::string(m.artist.value_or("")),
                              std::string(m.album.value_or("")), m.progress.has_value(),
                              m.progress.has_value() && m.progress->playback_speed == 0);
        }
        void on_metadata_clear() override { display.clear_track(); }
    };

    struct HostNetworkProvider : SendspinNetworkProvider {
        bool is_network_ready() override { return true; }
    };

    ArmPlayerListener player_listener(audio_sink, cfg.idle_timeout_s, display.get());
    audio_sink.on_frames_played = [&player](uint32_t frames, int64_t timestamp) {
        player.notify_audio_played(frames, timestamp);
    };

    ArmClientListener client_listener;
    HostNetworkProvider network_provider;

    player.set_listener(&player_listener);
    player.set_output_delay_adjustable(true);
    client.set_listener(&client_listener);
    client.set_network_provider(&network_provider);

    std::unique_ptr<ArmMetadataListener> metadata_listener;
    if (metadata) {
        metadata_listener = std::make_unique<ArmMetadataListener>(*display);
        metadata->set_listener(metadata_listener.get());
    }
    if (display) display->start();

    if (!client.start()) {
        fprintf(stderr, "Failed to start the Sendspin client\n");
        return 1;
    }

    // Since Sendspin 1.0 a server must pair with the player before it may
    // play (unless unpaired_access is on).  The pairing token carries this
    // player's identity and Pairing PSK: paste it into the server to pair.
    fprintf(stderr, "Client ID     : %s\n", client.client_id().c_str());
    if (auto token = client.client_pairing_token()) {
        fprintf(stderr, "Pairing token : %.*s\n", static_cast<int>(token->size()),
                token->data());
    }

    // Apply the configured startup volume before the first connection so the
    // hardware is at the expected level even before the server issues a
    // VOLUME command.  Without this the ALSA sink defaults to 100 while the
    // SDK reports 0 until the server speaks.
    if (cfg.initial_volume >= 0) {
        auto vol = static_cast<uint8_t>(cfg.initial_volume);
        audio_sink.set_volume(vol);
        player.update_volume(vol);
        if (display) display->set_volume(vol);
        fprintf(stderr, "Initial volume set to %u\n", vol);
    }

    // Connect to the configured Sendspin server and reconnect automatically
    // whenever the connection drops (e.g. server restart mid-playback).
    // The first attempt fires immediately; subsequent ones every 5 seconds.
    using clock = std::chrono::steady_clock;
    static constexpr auto RECONNECT_INTERVAL = std::chrono::seconds(5);
    auto last_connect_attempt = clock::now() - RECONNECT_INTERVAL;
    static constexpr auto DISPLAY_POLL_INTERVAL = std::chrono::milliseconds(250);
    auto last_display_poll = clock::now();

    while (running.load()) {
        client.loop();

        // Idle timeout check to release audio device
        if (cfg.idle_timeout_s > 0 && !player_listener.stream_active && player_listener.device_open) {
            auto now = clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - player_listener.last_stream_end_time).count();
            if (elapsed >= cfg.idle_timeout_s) {
                fprintf(stderr, ">>> Idle timeout reached (%d s), releasing audio device\n", cfg.idle_timeout_s);
                audio_sink.stop();
                player_listener.device_open = false;
            }
        }

        // Feed the display the state that has no callback.  Cheap: the
        // display only redraws when something visible actually changed.
        if (display && clock::now() - last_display_poll >= DISPLAY_POLL_INTERVAL) {
            last_display_poll = clock::now();
            display->set_connected(client.is_connected());
            display->set_progress(metadata->get_track_progress_ms(),
                                  metadata->get_track_duration_ms());
            // Polled rather than set on stream start so an in-place format
            // change is shown too.  nullopt while no stream is active.
            if (auto info = player.stream_info()) {
                display->set_audio_format(codec_name(info->codec), info->sample_rate,
                                          info->bit_depth);
            } else {
                display->set_audio_format("", 0, 0);
            }
        }

        if (!client.is_connected()) {
            auto now = clock::now();
            if (now - last_connect_attempt >= RECONNECT_INTERVAL) {
                fprintf(stderr, "Connecting to %s...\n", cfg.server_url.c_str());
                client.connect_to(cfg.server_url);
                last_connect_attempt = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    fprintf(stderr, "\nShutting down...\n");
    // Sends client/goodbye (shutdown), joins the library's threads and
    // flushes any pending state writes before returning.
    client.stop();
    if (display) display->stop();
    return 0;
}