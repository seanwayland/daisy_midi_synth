#include "daisy_pod.h"
#include "daisysp.h"
#include <vector>

using namespace daisy;
using namespace daisysp;

DaisyPod hw;

constexpr size_t kMaxVoices = 8;

struct Voice {
    Oscillator osc;
    Svf        filt;
    bool       active = false;
    int        note = -1;
    float      cutoff = 1000.0f;
    float      resonance = 0.1f;

    void Init(float samplerate) {
        osc.Init(samplerate);
        osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
        osc.SetPw(0.85f);
        osc.SetAmp(0.8f);

        filt.Init(samplerate);
        filt.SetRes(resonance);
        filt.SetFreq(cutoff);
    }

    void NoteOn(int n, float velocity) {
        note = n;
        osc.SetFreq(mtof(n));
        osc.SetAmp(velocity / 127.0f);
        active = true;
    }

    void NoteOff(int n) {
        if(note == n) {
            active = false;
            osc.SetAmp(0.0f);
        }
    }

    void SetFilter(float cutoffHz, float res) {
        cutoff = cutoffHz;
        resonance = res;
        filt.SetFreq(cutoff);
        filt.SetRes(resonance);
    }

    float Process() {
        if(!active) return 0.0f;
        float sig = osc.Process();
        filt.Process(sig);
        return filt.Low();
    }
};

std::vector<Voice> voices(kMaxVoices);

// Shared filter settings (controlled via MIDI CC)
float globalCutoff = 1000.0f;
float globalResonance = 0.1f;

void UpdateFilters() {
    for(auto& voice : voices) {
        voice.SetFilter(globalCutoff, globalResonance);
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t size) {
    float sig;
    for(size_t i = 0; i < size; i += 2) {
        sig = 0.0f;
        for(auto& voice : voices) {
            sig += voice.Process();
        }
        sig /= kMaxVoices;  // prevent clipping
        out[i] = out[i + 1] = sig;
    }
}

void HandleNoteOn(uint8_t note, uint8_t velocity) {
    for(auto& voice : voices) {
        if(!voice.active) {
            voice.NoteOn(note, velocity);
            return;
        }
    }
    // Voice stealing
    voices[0].NoteOn(note, velocity);
}

void HandleNoteOff(uint8_t note) {
    for(auto& voice : voices) {
        voice.NoteOff(note);
    }
}

void HandleMidiMessage(MidiEvent m) {
    switch(m.type) {
        case NoteOn: {
            NoteOnEvent p = m.AsNoteOn();
            if(p.velocity != 0)
                HandleNoteOn(p.note, p.velocity);
            else
                HandleNoteOff(p.note);
            break;
        }
        case NoteOff: {
            NoteOffEvent p = m.AsNoteOff();
            HandleNoteOff(p.note);
            break;
        }
        case ControlChange: {
            ControlChangeEvent p = m.AsControlChange();
            switch(p.control_number) {
                case 1: // Cutoff
                    globalCutoff = mtof((float)p.value);
                    UpdateFilters();
                    break;
                case 2: // Resonance
                    globalResonance = (float)p.value / 127.0f;
                    UpdateFilters();
                    break;
            }
            break;
        }
        default: break;
    }
}

int main(void) {
    float samplerate;
    hw.Init();
    hw.SetAudioBlockSize(4);
    hw.seed.usb_handle.Init(UsbHandle::FS_INTERNAL);
    System::Delay(250);

    samplerate = hw.AudioSampleRate();
    for(auto& voice : voices) {
        voice.Init(samplerate);
        voice.SetFilter(globalCutoff, globalResonance);
    }

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    for(;;) {
        hw.midi.Listen();
        while(hw.midi.HasEvents()) {
            HandleMidiMessage(hw.midi.PopEvent());
        }
    }
}
