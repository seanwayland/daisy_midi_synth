#include "daisy_pod.h"
#include "daisysp.h"
#include <vector>

using namespace daisy;
using namespace daisysp;

DaisyPod hw;

constexpr size_t kMaxVoices = 16;
constexpr size_t kVoicesPerSide = kMaxVoices / 2;
constexpr float kPitchBendRange = 7.0f;  // +/- 7 semitones pitch bend range

struct Voice {
    Oscillator osc;
    Svf        filt;
    Adsr       env;
    bool       active = false;
    int        note = -1;
    float      cutoff = 1000.0f;
    float      resonance = 0.1f;
    bool       gate = false;

    float pitch_bend_semitones = 0.0f;
    float velocity_amp = 1.0f;
    //float velocity_cutoff_boost = 0.0f; // <-- affects brightness

    void Init(float samplerate) {
        osc.Init(samplerate);
        osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
        osc.SetAmp(1.0f);
        osc.SetPw(0.9f);

        filt.Init(samplerate);
        filt.SetFreq(cutoff);
        filt.SetRes(resonance);

        env.Init(samplerate);
        env.SetTime(ADSR_SEG_ATTACK, 0.02f);
        env.SetTime(ADSR_SEG_DECAY, 0.2f);
        env.SetSustainLevel(0.7f);
        env.SetTime(ADSR_SEG_RELEASE, 0.01f);
    }

    void NoteOn(int n, float velocity) {
        note = n;
        active = true;
        gate = true;
        velocity_amp = velocity / 127.0f;
        //velocity_cutoff_boost = velocity_amp * 400.0f; // <-- subtle boost to cutoff
        UpdateFrequency();
    }

    void NoteOff(int n) {
        if(note == n) {
            gate = false;
        }
    }

    void SetFilter(float cutoffHz, float res) {
        cutoff = cutoffHz;
        resonance = res;
        filt.SetFreq(cutoff);
        filt.SetRes(resonance);
    }

    void SetPitchBend(float semitones) {
        pitch_bend_semitones = semitones;
        UpdateFrequency();
    }

    void UpdateFrequency() {
        if(note >= 0) {
            float freq = mtof(static_cast<float>(note) + pitch_bend_semitones);
            osc.SetFreq(freq);
        }
    }

    void SetPulseWidth(float pw) {
        osc.SetPw(pw);
    }

    float Process() {
        if(!active) return 0.0f;

        float amp = env.Process(gate);
        if(amp <= 0.0001f && !gate) {
            active = false;
            return 0.0f;
        }
        
        float sig = osc.Process() * amp * velocity_amp;
        filt.Process(sig);
        return filt.Low();
    }
};

std::vector<Voice> voices_left(kVoicesPerSide);
std::vector<Voice> voices_right(kVoicesPerSide);

float globalCutoffLeft = 1020.0f;
float globalCutoffRight = 1000.0f;
float globalResonance = 0.1f;
float globalPitchBend = 0.0f;

void UpdateFilters() {
    for(auto& v : voices_left) {
        v.SetFilter(globalCutoffLeft, globalResonance);
    }
    for(auto& v : voices_right) {
        v.SetFilter(globalCutoffRight, globalResonance);
    }
}

void UpdatePitchBend(float semitones) {
    globalPitchBend = semitones;
    for(auto& v : voices_left) {
        v.SetPitchBend(semitones);
    }
    for(auto& v : voices_right) {
        v.SetPitchBend(semitones);
    }
}

Voice* FindFreeVoice(std::vector<Voice>& voices) {
    for(auto& v : voices) {
        if(!v.active)
            return &v;
    }
    return &voices[0];
}

void AudioCallback(AudioHandle::InterleavingInputBuffer in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t size) {
    for(size_t i = 0; i < size; i += 2) {
        float sig_left = 0.0f;
        float sig_right = 0.0f;

        for(auto& v : voices_left) {
            sig_left += v.Process();
        }
        for(auto& v : voices_right) {
            sig_right += v.Process();
        }

        sig_left /= static_cast<float>(kVoicesPerSide);
        sig_right /= static_cast<float>(kVoicesPerSide);

        out[i] = sig_left;
        out[i + 1] = sig_right;
    }
}

void HandleNoteOn(uint8_t note, uint8_t velocity) {
    {
        Voice* voice = FindFreeVoice(voices_left);
        voice->NoteOn(note, velocity);
        voice->SetPitchBend(globalPitchBend);
    }
    {
        Voice* voice = FindFreeVoice(voices_right);
        voice->NoteOn(note, velocity);
        voice->SetPitchBend(globalPitchBend);
    }
}

void HandleNoteOff(uint8_t note) {
    for(auto& v : voices_left) {
        v.NoteOff(note);
    }
    for(auto& v : voices_right) {
        v.NoteOff(note);
    }
}

void HandleMidiMessage(MidiEvent m) {
    switch(m.type) {
        case NoteOn: {
            auto p = m.AsNoteOn();
            if(p.velocity != 0)
                HandleNoteOn(p.note, p.velocity);
            else
                HandleNoteOff(p.note);
            break;
        }
        case NoteOff: {
            auto p = m.AsNoteOff();
            HandleNoteOff(p.note);
            break;
        }
        case ControlChange: {
            auto p = m.AsControlChange();
            switch(p.control_number) {
                case 1:
                    globalCutoffLeft = mtof(static_cast<float>(p.value)) + 10.0f;
                    globalCutoffRight = mtof(static_cast<float>(p.value));
                    UpdateFilters();
                    break;
                case 2:
                    globalResonance = static_cast<float>(p.value) / 400.0f;
                    UpdateFilters();
                    break;
            }
            break;
        }
        case PitchBend: {
            auto p = m.AsPitchBend();
            float bendVal = static_cast<float>(p.value);
            float norm = (bendVal / 8192.0f);
            float semitones = norm * kPitchBendRange;
            UpdatePitchBend(semitones);
            break;
        }
        default: break;
    }
}

int main(void) {
    hw.Init();
    hw.SetAudioBlockSize(4);
    float samplerate = hw.AudioSampleRate();

    for(auto& v : voices_left) {
        v.Init(samplerate);
        v.SetFilter(globalCutoffLeft, globalResonance);
        v.SetPulseWidth(0.87f);
    }
    for(auto& v : voices_right) {
        v.Init(samplerate);
        v.SetFilter(globalCutoffRight, globalResonance);
        v.SetPulseWidth(0.9f);
    }

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    while(true) {
        hw.midi.Listen();
        while(hw.midi.HasEvents()) {
            HandleMidiMessage(hw.midi.PopEvent());
        }
    }
}
