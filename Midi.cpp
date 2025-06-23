#include "daisy_pod.h"
#include "daisysp.h"
#include <stdio.h>
#include <string.h>

using namespace daisy;
using namespace daisysp;

DaisyPod hw;

const int kNumVoices = 8;

struct FmVoice {
    Oscillator carrierL, carrierR;
    Oscillator modulatorL, modulatorR;
    Adsr env;
    float carrier_freq;
    float current_freq;
    float pitch_bend;
    float feedback;
    float mod_index;
    float base_mod_index;
    float ratio;
    bool gate;
    int note_number;
    float velocity_amount;
    
    float prev_mod_signalL, prev_mod_signalR;
    float prev_carrier_signalL, prev_carrier_signalR;
    
    void Init(float sample_rate) {
        carrierL.Init(sample_rate);
        carrierR.Init(sample_rate);
        modulatorL.Init(sample_rate);
        modulatorR.Init(sample_rate);
        env.Init(sample_rate);
        
        carrierL.SetWaveform(Oscillator::WAVE_SIN);
        carrierR.SetWaveform(Oscillator::WAVE_SIN);
        modulatorL.SetWaveform(Oscillator::WAVE_SIN);
        modulatorR.SetWaveform(Oscillator::WAVE_SIN);
        
        carrier_freq = 440.0f;
        current_freq = 440.0f;
        pitch_bend = 0.0f;
        feedback = 0.0f;
        mod_index = 1.0f;
        base_mod_index = 1.0f;
        ratio = 1.0f;
        gate = false;
        note_number = -1;
        velocity_amount = 1.0f;
        prev_mod_signalL = prev_mod_signalR = 0.0f;
        prev_carrier_signalL = prev_carrier_signalR = 0.0f;
        
        env.SetAttackTime(0.03f);
        env.SetDecayTime(0.3f);
        env.SetReleaseTime(0.03f);
        env.SetSustainLevel(0.7f);
    }
    
    void UpdateFrequencies() {
        current_freq = carrier_freq * powf(2.0f, pitch_bend / 12.0f);
        carrierL.SetFreq(current_freq);
        carrierR.SetFreq(current_freq);
        modulatorL.SetFreq(current_freq * ratio);
        modulatorR.SetFreq(current_freq * ratio);
    }
    
    void Process(float* out_left, float* out_right) {
        if (!gate && env.GetCurrentSegment() == ADSR_SEG_IDLE) {
            *out_left = 0.0f;
            *out_right = 0.0f;
            return;
        }
        
        float note_pos = (float)(note_number - 36) / 60.0f;
        note_pos = fminf(fmaxf(note_pos, 0.0f), 1.0f);
        float feedback_scaling = 1.0f - note_pos * 0.3f;
        float mod_scaling = note_pos * 0.5f + 0.5f;
        
        float effective_mod_index = base_mod_index * 
                                  (0.5f + velocity_amount * 0.5f) *
                                  mod_scaling;
        
        // Left channel (more intense)
        modulatorL.SetFreq(current_freq * ratio);
        float mod_signalL = modulatorL.Process() + (prev_mod_signalL * feedback * feedback_scaling * 1.3f);
        float phase_modL = mod_signalL * effective_mod_index * 0.75f * 0.1f;
        carrierL.PhaseAdd(phase_modL);
        float carrier_signalL = carrierL.Process();
        
        // Right channel (less intense)
        modulatorR.SetFreq(current_freq * ratio);
        float mod_signalR = modulatorR.Process() + (prev_mod_signalR * feedback * feedback_scaling * 0.75f);
        float phase_modR = mod_signalR * effective_mod_index * 1.2f * 0.1f;
        carrierR.PhaseAdd(phase_modR);
        float carrier_signalR = carrierR.Process();
        
        float env_val = env.Process(gate);
        *out_left = carrier_signalL * env_val * velocity_amount;
        *out_right = carrier_signalR * env_val * velocity_amount;
        
        prev_mod_signalL = mod_signalL;
        prev_mod_signalR = mod_signalR;
        prev_carrier_signalL = carrier_signalL;
        prev_carrier_signalR = carrier_signalR;
    }
    
    void SetFreq(float freq) {
        carrier_freq = freq;
        UpdateFrequencies();
    }
    
    void SetPitchBend(float bend) {
        pitch_bend = bend * 7.0f;
        UpdateFrequencies();
    }
    
    void SetAmp(float amp) {
        carrierL.SetAmp(amp);
        carrierR.SetAmp(amp);
    }
    
    void Trigger(float freq, float velocity, int note) {
        SetFreq(freq);
        velocity_amount = velocity;
        SetAmp(velocity * 0.8f);
        note_number = note;
        gate = true;
    }
    
    void Release() {
        gate = false;
    }
    
    bool IsActive() {
        return gate || env.GetCurrentSegment() != ADSR_SEG_IDLE;
    }
};

struct FmSynth {
    FmVoice voices[kNumVoices];
    float feedback;
    float mod_index;
    float ratio;
    float pitch_bend;
    
    void Init(float sample_rate) {
        for (int i = 0; i < kNumVoices; i++) {
            voices[i].Init(sample_rate);
        }
        feedback = 0.0f;
        mod_index = 1.0f;
        ratio = 1.0f;
        pitch_bend = 0.0f;
    }
    
    void Process(float* out_left, float* out_right, size_t size) {
        for (size_t i = 0; i < size; i += 2) {
            float mixL = 0.0f, mixR = 0.0f;
            for (int v = 0; v < kNumVoices; v++) {
                float voiceL, voiceR;
                voices[v].Process(&voiceL, &voiceR);
                mixL += voiceL * (1.0f / kNumVoices);
                mixR += voiceR * (1.0f / kNumVoices);
            }
            out_left[i] = out_left[i+1] = mixL;
            out_right[i] = out_right[i+1] = mixR;
        }
    }
    
    void NoteOn(int note, int velocity) {
        int voice_idx = -1;
        for (int i = 0; i < kNumVoices; i++) {
            if (!voices[i].IsActive()) {
                voice_idx = i;
                break;
            }
        }
        
        if (voice_idx == -1) voice_idx = 0;
        
        float normalized_velocity = velocity / 127.0f;
        voices[voice_idx].Trigger(mtof(note), normalized_velocity, note);
        voices[voice_idx].feedback = feedback*normalized_velocity*8.0;
        voices[voice_idx].base_mod_index = mod_index*normalized_velocity*2.05;
        voices[voice_idx].ratio = ratio;
        voices[voice_idx].SetPitchBend(pitch_bend);
    }
    
    void NoteOff(int note) {
        for (int i = 0; i < kNumVoices; i++) {
            if (voices[i].note_number == note) {
                voices[i].Release();
            }
        }
    }
    
    void SetPitchBend(float bend) {
        pitch_bend = bend;
        for (int i = 0; i < kNumVoices; i++) {
            if (voices[i].IsActive()) {
                voices[i].SetPitchBend(bend);
            }
        }
    }
    
    void SetParam(int cc, float value) {
        switch(cc) {
            case 1:
                mod_index = value * 2.0f;
                for (int i = 0; i < kNumVoices; i++) {
                    if (voices[i].IsActive()) {
                        voices[i].base_mod_index = mod_index;
                    }
                }
                feedback = value;
                for (int i = 0; i < kNumVoices; i++) {
                    if (voices[i].IsActive()) {
                        voices[i].feedback = feedback;
                    }
                }
                break;
            case 2:
                feedback = value;
                for (int i = 0; i < kNumVoices; i++) {
                    if (voices[i].IsActive()) {
                        voices[i].feedback = feedback;
                    }
                }
                break;
            case 3:
                ratio = 0.25f + value * 3.75f;
                for (int i = 0; i < kNumVoices; i++) {
                    if (voices[i].IsActive()) {
                        voices[i].ratio = ratio;
                        voices[i].UpdateFrequencies();
                    }
                }
                break;
            case 4: case 5: case 6: case 7:
                for (int i = 0; i < kNumVoices; i++) {
                    if (cc == 4) voices[i].env.SetAttackTime(value * 2.0f);
                    if (cc == 5) voices[i].env.SetDecayTime(value * 2.0f);
                    if (cc == 6) voices[i].env.SetSustainLevel(value);
                    if (cc == 7) voices[i].env.SetReleaseTime(value * 2.0f);
                }
                break;
        }
    }
};

FmSynth fm_synth;

void AudioCallback(AudioHandle::InterleavingInputBuffer in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t size)
{
    float out_left[size], out_right[size];
    fm_synth.Process(out_left, out_right, size);
    
    for(size_t i = 0; i < size; i += 2) {
        out[i] = out_left[i];
        out[i+1] = out_right[i];
    }
}

void HandleMidiMessage(MidiEvent m)
{
    switch(m.type) {
        case NoteOn: {
            NoteOnEvent p = m.AsNoteOn();
            if(m.data[1] != 0) fm_synth.NoteOn(p.note, p.velocity);
            else fm_synth.NoteOff(p.note);
        } break;
        
        case NoteOff: {
            NoteOffEvent p = m.AsNoteOff();
            fm_synth.NoteOff(p.note);
        } break;
        
        case PitchBend: {
            PitchBendEvent p = m.AsPitchBend();
            float bend = (float)(p.value / 8192.0f);
            fm_synth.SetPitchBend(bend);
        } break;
        
        case ControlChange: {
            ControlChangeEvent p = m.AsControlChange();
            fm_synth.SetParam(p.control_number, (float)p.value / 127.0f);
        } break;
        
        default: break;
    }
}

int main(void)
{
    float samplerate;
    hw.Init();
    hw.SetAudioBlockSize(4);
    hw.seed.usb_handle.Init(UsbHandle::FS_INTERNAL);
    System::Delay(250);

    samplerate = hw.AudioSampleRate();
    fm_synth.Init(samplerate);

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
