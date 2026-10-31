#include "app/Transport.h"

namespace reader {

Transport::Transport() {
    const auto err = devices.initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty()) status = juce::String::fromUTF8("没有可用的声卡：") + err;
    devices.addAudioCallback(this);
}

Transport::~Transport() {
    devices.removeAudioCallback(this);
    devices.closeAudioDevice();
}

void Transport::audioDeviceAboutToStart(juce::AudioIODevice* d) {
    engine.prepare(d->getCurrentSampleRate());
}

void Transport::audioDeviceStopped() { engine.suspendAudio(); }

void Transport::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut, int numSamples,
                                                 const juce::AudioIODeviceCallbackContext&) {
    for (int c = 0; c < numOut; ++c)
        if (out[c]) juce::FloatVectorOperations::clear(out[c], numSamples);
    engine.render(out, numOut, numSamples);
}

void Transport::setSchedule(const core::Projection& p, const core::TempoMap& tm) {
    tempo = tm;
    std::vector<pianoroll::PlaybackNote> notes;
    int source = 0;
    for (size_t li = 0; li < p.lanes.size(); ++li) {
        for (auto& n : p.lanes[li].notes) {
            if (n.flags & core::noteFlagKeySwitch) continue;   // keyswitch 不出声
            pianoroll::PlaybackNote pn;
            pn.start = tempo.secondsAt((double)n.start);
            pn.end = tempo.secondsAt((double)(n.start + n.length));
            if (pn.end <= pn.start) pn.end = pn.start + .01;
            pn.pitch = n.pitch;
            pn.velocity = std::max(1, n.velocity);
            pn.group = (int)li;
            pn.source = source + (int)li;
            notes.push_back(pn);
        }
    }
    const double length = tempo.secondsAt((double)p.documentLength);
    std::string err;
    auto schedule = pianoroll::makePlaybackSchedule(std::move(notes), {}, length, &err);
    if (!schedule) {
        status = juce::String::fromUTF8("播放数据有问题：") + juce::String(err);
        engine.clear();
        return;
    }
    engine.setSchedule(schedule, engine.position());
}

void Transport::clear() { engine.clear(); }
void Transport::togglePlay() { engine.isPlaying() ? engine.pause() : engine.play(); }
void Transport::play() { engine.play(); }
void Transport::pause() { engine.pause(); }
void Transport::stop() { engine.stop(); }
void Transport::home() { engine.home(); }
void Transport::seekTick(double tick) { engine.seek(tempo.secondsAt(tick)); }
void Transport::setSolo(int lane) { engine.setSolo(lane); }
double Transport::positionTick() const { return tempo.tickAt(engine.position()); }

} // namespace reader
