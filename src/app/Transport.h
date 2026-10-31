// 播放：pianoroll-core 的 PlaybackEngine（参考音色，不讲究）接到系统声卡。
// 时间：引擎用秒，画布用 tick，换算用 Document 的 tempo map。
#pragma once
#include "core/Project.h"
#include "core/TempoMap.h"

#include <PianoRollCore/Playback.h>
#include <juce_audio_devices/juce_audio_devices.h>

namespace reader {

class Transport : private juce::AudioIODeviceCallback {
public:
    Transport();
    ~Transport() override;

    juce::String audioStatus() const { return status; }
    void setSchedule(const core::Projection&, const core::TempoMap&);   // 投影换了就调
    void clear();

    void togglePlay();
    void play();
    void pause();
    void stop();
    void home();
    void seekTick(double tick);
    void setSolo(int laneIndex);   // -1 = 全部
    int solo() const { return engine.solo(); }
    bool isPlaying() const { return engine.isPlaying(); }
    double positionTick() const;
    double positionSeconds() const { return engine.position(); }
    juce::AudioDeviceManager& deviceManager() { return devices; }

private:
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    juce::AudioDeviceManager devices;
    pianoroll::PlaybackEngine engine;
    core::TempoMap tempo;
    juce::String status;
};

} // namespace reader
