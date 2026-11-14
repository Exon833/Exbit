// The waveform at the top of the Explorer (sample_waveform.py): the exact audio of the selected
// sample (a MIDI file: what the built-in synth plays of it), left channel above, right below (one
// lane if they are the same). A line follows the preview while it plays; click to play from that
// point. The audio is loaded by the Explorer in a background thread; this only shows it.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "ui/toolkit/widget.h"

class SampleWaveform : public ui::Widget {
public:
    static constexpr int HEIGHT = 76;  // px

    explicit SampleWaveform(ui::Widget* parent = nullptr);

    ui::Signal<double> clicked;  // where it was clicked: 0 (start) … 1 (end)

    // show this audio (stereo, at rate sr), or nothing (audio null)
    void setAudio(AudioPtr audio, const std::string& name, int sr);
    // the line of the preview playing: 0…1, or none
    void setPosition(std::optional<double> fraction);

    AudioPtr audio() const { return audio_; }
    const std::string& name() const { return name_; }
    double seconds() const { return seconds_; }
    std::optional<double> position() const { return position_; }

    // per lane (one per channel, or one if both are the same): the lowest and highest sample of each
    // pixel column, from every sample of the audio (cached per width)
    struct Lane { std::vector<float> mins, maxs; };
    const std::vector<Lane>& lanes(int width);

    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;

private:
    AudioPtr audio_;
    std::string name_;
    double seconds_ = 0;
    std::optional<double> position_;
    int peaksWidth_ = -1;
    std::vector<Lane> peaks_;
};
