// SPDX-License-Identifier: MIT
//
// SDL3 audio sink: one playback stream on the chosen device (the default one unless set_device picked another).
// SDL converts and resamples from the AI's 32 or 48 kHz to whatever the device runs at.
#include "gcrecomp/audio/audio_sink.h"

#ifdef GCRECOMP_HAVE_SDL3
#include <SDL3/SDL.h>

#include <cstdio>
#include <mutex>

namespace gcrecomp::audio {

namespace {

// The id of the playback device called `name`, or the default device when `name` is empty or not there.
SDL_AudioDeviceID find_playback_device(const std::string& name) {
    if (name.empty()) return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    int count = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count)) {
        for (int i = 0; i < count; ++i) {
            const char* n = SDL_GetAudioDeviceName(ids[i]);
            if (n && name == n) {
                found = ids[i];
                break;
            }
        }
        SDL_free(ids);
    }
    if (found == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) std::fprintf(stderr, "[audio] device \"%s\" not found: using the default one\n", name.c_str());
    return found;
}

class SdlSink final : public AudioSink {
public:
    ~SdlSink() override { close(); }

    bool open(uint32_t sample_rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        close_locked();
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
            return false;
        }
        inited_ = true;
        rate_ = sample_rate;
        return open_stream();
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        close_locked();
    }

    bool set_sample_rate(uint32_t sample_rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stream_) return false;
        if (sample_rate == rate_) return true;
        const SDL_AudioSpec src = make_spec(sample_rate);
        if (!SDL_SetAudioStreamFormat(stream_, &src, nullptr)) return false;
        rate_ = sample_rate;
        return true;
    }

    void submit(const int16_t* interleaved, size_t frames) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_) SDL_PutAudioStreamData(stream_, interleaved, int(frames * 4));
    }

    size_t queued_frames() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stream_) return 0;
        const int bytes = SDL_GetAudioStreamQueued(stream_);
        return bytes > 0 ? size_t(bytes) / 4 : 0;
    }

    void set_volume(float gain) override {
        std::lock_guard<std::mutex> lock(mutex_);
        gain_ = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
        if (stream_) SDL_SetAudioStreamGain(stream_, gain_);
    }

    bool set_device(const std::string& name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!inited_ || name == device_) return true;
        device_ = name;
        close_stream();
        return open_stream();
    }

private:
    bool open_stream() {
        const SDL_AudioSpec spec = make_spec(rate_);
        stream_ = SDL_OpenAudioDeviceStream(find_playback_device(device_), &spec, nullptr, nullptr);
        if (!stream_ && !device_.empty()) {  // the chosen device cannot be opened: the system's default then
            std::fprintf(stderr, "[audio] cannot open \"%s\": %s\n", device_.c_str(), SDL_GetError());
            stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        }
        if (!stream_) {
            std::fprintf(stderr, "[audio] cannot open the playback device: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetAudioStreamGain(stream_, gain_);
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }

    void close_stream() {
        if (stream_) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }

    void close_locked() {
        close_stream();
        if (inited_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        inited_ = false;
    }

    static SDL_AudioSpec make_spec(uint32_t rate) {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;  // host-endian
        spec.channels = 2;
        spec.freq = int(rate);
        return spec;
    }

    SDL_AudioStream* stream_ = nullptr;
    bool inited_ = false;
    uint32_t rate_ = 0;
    float gain_ = 1.0f;
    std::string device_;
    std::mutex mutex_;  // the settings (window thread) and the AI (guest thread) both reach the sink
};

}  // namespace

std::unique_ptr<AudioSink> make_sdl_sink() { return std::make_unique<SdlSink>(); }

std::vector<std::string> list_playback_devices() {
    std::vector<std::string> names;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return names;
    int count = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count)) {
        for (int i = 0; i < count; ++i)
            if (const char* n = SDL_GetAudioDeviceName(ids[i])) names.emplace_back(n);
        SDL_free(ids);
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return names;
}

}  // namespace gcrecomp::audio

#else

namespace gcrecomp::audio {
std::unique_ptr<AudioSink> make_sdl_sink() { return nullptr; }
std::vector<std::string> list_playback_devices() { return {}; }
}  // namespace gcrecomp::audio

#endif
