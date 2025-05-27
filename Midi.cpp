#include "daisy_pod.h"
#include "daisysp.h"
#include <vector>

using namespace daisy;
using namespace daisysp;

DaisyPod hw;

constexpr size_t kMaxVoices = 8;
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

    // Current pitch bend in semitones applied to this voice
    float pitch_bend_semitones = 0.0f;

    void Init(float samplerate) {
        osc.Init(samplerate);
        osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
        osc.SetAmp(1.0f);
        osc.SetPw(0.9f);

        filt.Init(samplerate);
        filt.SetFreq(cutoff);
        filt.SetRes(resonance);

        env.Init(samplerate);
        env.SetTime(ADSR_SEG_ATTACK, 0.03f);
        env.SetTime(ADSR_SEG_DECAY, 0.2f);
        env.SetSustainLevel(0.7f);
        env.SetTime(ADSR_SEG_RELEASE, 0.03f);
    }

    void NoteOn(int n, float velocity) {
        note = n;
        active = true;
        gate = true;
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

    float Process() {
        if(!active) return 0.0f;

        float amp = env.Process(gate);
        if(amp <= 0.0001f && !gate) {
            active = false;
            return 0.0f;
        }

        float sig = osc.Process() * amp;
        filt.Process(sig);
        return filt.Low();
    }
};

std::vector<Voice> voices(kMaxVoices);

float globalCutoff = 1000.0f;
float globalResonance = 0.1f;
float globalPitchBend = 0.0f;

void UpdateFilters() {
    for(auto& voice : voices) {
        voice.SetFilter(globalCutoff, globalResonance);
    }
}

void UpdatePitchBend(float semitones) {
    globalPitchBend = semitones;
    for(auto& voice : voices) {
        voice.SetPitchBend(semitones);
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t size) {
    for(size_t i = 0; i < size; i += 2) {
        float sig = 0.0f;
        for(auto& voice : voices) {
            sig += voice.Process();
        }
        sig /= static_cast<float>(kMaxVoices);
        out[i] = out[i + 1] = sig;
    }
}

void HandleNoteOn(uint8_t note, uint8_t velocity) {
    for(auto& voice : voices) {
        if(!voice.active) {
            voice.NoteOn(note, velocity);
            voice.SetPitchBend(globalPitchBend);
            return;
        }
    }
    // Voice stealing
    voices[0].NoteOn(note, velocity);
    voices[0].SetPitchBend(globalPitchBend);
}

void HandleNoteOff(uint8_t note) {
    for(auto& voice : voices) {
        voice.NoteOff(note);
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
                    globalCutoff = mtof(static_cast<float>(p.value));
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
            //float norm = (bendVal - 8192.0f) / 8192.0f; // Normalize [-1,1]
            float norm = (bendVal  / 8192.0f); // Normalize [-1,1]
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

    for(auto& voice : voices) {
        voice.Init(samplerate);
        voice.SetFilter(globalCutoff, globalResonance);
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
