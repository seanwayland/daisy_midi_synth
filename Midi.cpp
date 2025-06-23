#include "daisy_pod.h"
#include "daisysp.h"
#include <vector>

using namespace daisy;
using namespace daisysp;

// Effects Constants
#define MAX_DELAY static_cast<size_t>(48000 * 2.5f)
#define CHRDEL 0
#define DEL 1
#define COR 2
#define PHR 3
#define OCT 4
#define FM 5

// FM Synth Constants
const int kNumVoices = 8;
const float kMaxOutputLevel = 0.7f; // Safe output level to prevent clipping

DaisyPod hw;

// Effects Objects
static Chorus crs, crs2, crs3, crs4;
static PitchShifter pst;
static Phaser psr, psr2;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS dell;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delr;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS dell2;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delr2;

// Global Variables
int mode = FM;
int numstages;
uint32_t octDelSize;
float currentDelay, feedback, delayTarget, freq, freqtarget, lfotarget, lfo;
float drywet;

// FM Synth Implementation
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
        feedback = 0.3f;
        mod_index = 1.0f;
        base_mod_index = 1.0f;
        ratio = 1.0f;
        gate = false;
        note_number = -1;
        velocity_amount = 1.0f;
        prev_mod_signalL = prev_mod_signalR = 0.0f;
        
        env.SetAttackTime(0.03f);
        env.SetDecayTime(0.3f);
        env.SetReleaseTime(0.03f);
        env.SetSustainLevel(0.7f);
        
        // Set safe amplitude limits
        carrierL.SetAmp(0.5f);
        carrierR.SetAmp(0.5f);
        modulatorL.SetAmp(0.5f);
        modulatorR.SetAmp(0.5f);
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
        note_pos = fclamp(note_pos, 0.0f, 1.0f);
        float feedback_scaling = 1.0f - note_pos * 0.2f;
        float mod_scaling = note_pos * 0.2f + 0.5f;
        
        // More gentle modulation with velocity
        float effective_mod_index = base_mod_index * 
                                  (0.3f + velocity_amount * 0.4f) * // Reduced modulation range
                                  mod_scaling;
        
        // Left channel
        modulatorL.SetFreq(current_freq * ratio);
        float mod_signalL = modulatorL.Process() + (prev_mod_signalL * feedback * feedback_scaling * 0.9f); // Reduced feedback
        float phase_modL = mod_signalL * effective_mod_index * 0.15f; // Reduced modulation depth
        carrierL.PhaseAdd(phase_modL);
        float carrier_signalL = carrierL.Process();
        
        // Right channel
        modulatorR.SetFreq(current_freq * ratio);
        float mod_signalR = modulatorR.Process() + (prev_mod_signalR * feedback * feedback_scaling * 0.9f); // Reduced feedback
        float phase_modR = mod_signalR * effective_mod_index * 0.16f; // Reduced modulation depth
        carrierR.PhaseAdd(phase_modR);
        float carrier_signalR = carrierR.Process();
        
        float env_val = env.Process(gate);
        *out_left = carrier_signalL * env_val * velocity_amount * 0.8f; // Reduced output level
        *out_right = carrier_signalR * env_val * velocity_amount * 0.8f; // Reduced output level
        
        prev_mod_signalL = mod_signalL * 0.5f; // Dampen feedback signal
        prev_mod_signalR = mod_signalR * 0.5f; // Dampen feedback signal
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
        // Safe amplitude limits
        carrierL.SetAmp(fclamp(amp, 0.1f, 0.6f));
        carrierR.SetAmp(fclamp(amp, 0.1f, 0.6f));
    }
    
    void Trigger(float freq, float velocity, int note) {
        SetFreq(freq);
        velocity_amount = fclamp(velocity, 0.1f, 1.0f); // Clamp velocity
        SetAmp(velocity_amount * 0.6f); // Reduced max amplitude
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
            out_left[i] = fclamp(mixL, -kMaxOutputLevel, kMaxOutputLevel);
            out_right[i] = fclamp(mixR, -kMaxOutputLevel, kMaxOutputLevel);
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
        
        float normalized_velocity = fclamp(velocity / 127.0f, 0.1f, 1.0f);
        voices[voice_idx].Trigger(mtof(note), normalized_velocity, note);
        voices[voice_idx].feedback = feedback * normalized_velocity * 9.0f; // Reduced feedback scaling
        voices[voice_idx].base_mod_index = mod_index * normalized_velocity * 0.6f; // Reduced modulation
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
        value = fclamp(value, 0.0f, 1.0f); // Clamp all parameter values
        
        switch(cc) {
            case 1:
                mod_index = value * 1.5f; // Reduced max modulation
                feedback = value * 0.8f; // Reduced max feedback
                for (int i = 0; i < kNumVoices; i++) {
                    if (voices[i].IsActive()) {
                        voices[i].base_mod_index = mod_index * 0.5f;
                        voices[i].feedback = feedback * 0.5f;
                    }
                }
                break;

            case 2:
                feedback = value * 0.8f; // Reduced max feedback
                for (int i = 0; i < kNumVoices; i++) {
                    if (voices[i].IsActive()) {
                        voices[i].feedback = feedback;
                    }
                }
                break;
            case 3:
                ratio = 0.5f + value * 2.5f; // Reduced ratio range
                for (int i = 0; i < kNumVoices; i++) {
                    if (voices[i].IsActive()) {
                        voices[i].ratio = ratio;
                        voices[i].UpdateFrequencies();
                    }
                }
                break;
            case 4: case 5: case 6: case 7:
                for (int i = 0; i < kNumVoices; i++) {
                    if (cc == 4) voices[i].env.SetAttackTime(value * 1.5f); // Reduced max time
                    if (cc == 5) voices[i].env.SetDecayTime(value * 1.5f); // Reduced max time
                    if (cc == 6) voices[i].env.SetSustainLevel(value);
                    if (cc == 7) voices[i].env.SetReleaseTime(value * 1.5f); // Reduced max time
                }
                break;
        }
    }
};

FmSynth fm_synth;

// Effect Processing Functions
void GetReverbSample(float &outl, float &outr, float inl, float inr) {
    fonepole(currentDelay, delayTarget, .00007f);
    delr.SetDelay(currentDelay);
    dell.SetDelay(currentDelay);
    delr2.SetDelay(20000.0f);
    dell2.SetDelay(30000.0f);
    outl = dell.Read() + dell2.Read();
    outr = delr.Read() + delr2.Read();
    dell.Write((feedback * outl) + inl);
    dell2.Write((feedback * outl) + inl);
    delr.Write((feedback * outr) + inr);
    delr2.Write((feedback * outr) + inr);

    crs.Process(inl);
    crs2.Process(inr);
    crs3.Process(inl);
    crs4.Process(inr);
    
    outl = crs.GetLeft() * drywet * 2.0f  + crs3.GetLeft() * drywet * 2.0f + inl * (0.5f - drywet) + (feedback * outl * drywet * 0.6f) + ((1.0f - feedback) * inl * drywet);
    outr = crs2.GetRight() * drywet * 2.0f + crs4.GetRight() * drywet * 2.0f + inr * (0.5f - drywet) + (feedback * outr * drywet * 0.6f) + ((1.0f - feedback) * inr * drywet);
}

void GetDelaySample(float &outl, float &outr, float inl, float inr) {
    fonepole(currentDelay, delayTarget, .00007f);
    delr.SetDelay(currentDelay);
    dell.SetDelay(currentDelay);
    delr2.SetDelay(20000.0f);
    dell2.SetDelay(30000.0f);
    outl = dell.Read() + dell2.Read();
    outr = delr.Read() + delr2.Read();

    dell.Write((feedback * outl) + inl);
    dell2.Write((feedback * outl) + inl);
    outl = (feedback * outl) + ((1.0f - feedback) * inl);

    delr.Write((feedback * outr) + inr);
    delr2.Write((feedback * outr) + inr);
    outr = (feedback * outr) + ((1.0f - feedback) * inr);
}

void GetChorusSample(float &outl, float &outr, float inl, float inr) {
    crs.Process(inl);
    crs2.Process(inr);
    crs3.Process(inl);
    crs4.Process(inr);
    
    outl = crs.GetLeft() * drywet * 2.5f + crs3.GetLeft() * drywet * 2.5f + inl * (0.5f - drywet);
    outr = crs2.GetRight() * drywet * 2.5f + crs4.GetRight() * drywet * 2.5f + inr * (0.5f - drywet);
}

void GetPhaserSample(float &outl, float &outr, float inl, float inr) {
    freq = 7000.0f;
    fonepole(freq, freqtarget, .0001f);
    psr.SetFreq(freq);
    fonepole(lfo, lfotarget, .0001f);
    psr.SetLfoDepth(lfo);
    psr.SetFeedback(.2f);
    psr2.SetFeedback(.3f);
    psr.SetLfoFreq(30.0f);
    psr2.SetLfoFreq(40.0f);

    freq = 4000.0f;
    fonepole(freq, freqtarget, .0001f);
    psr2.SetFreq(freq);
    fonepole(lfo, lfotarget, .0001f);
    psr2.SetLfoDepth(lfo);
    crs.Process(inl);

    outl = crs.GetLeft() * drywet * 0.5f + psr.Process(inl) * drywet + inl * (1.f - drywet);
    outr = crs.GetRight() * drywet * 0.3f + psr2.Process(inr) * drywet + inr * (1.f - drywet);
}

void GetOctaveSample(float &outl, float &outr, float inl, float inr) {
    outl = crs.GetLeft() * drywet * 0.2f + pst.Process(inl) * drywet + inl * (1.f - drywet);
    outr = crs.GetRight() * drywet * 0.2f + pst.Process(inr) * drywet + inr * (1.f - drywet);
}

void GetFmSample(float &outl, float &outr, float inl, float inr) {
    // Process FM synth
    float fm_left, fm_right;
    fm_synth.Process(&fm_left, &fm_right, 1);
    
    // Apply delay effect
    fonepole(currentDelay, delayTarget, .00007f);
    delr.SetDelay(currentDelay);
    dell.SetDelay(currentDelay);
    delr2.SetDelay(20000.0f);
    dell2.SetDelay(30000.0f);
    
    outl = dell.Read();
    outr = delr.Read();
    
    dell.Write((feedback * outl) + fm_left);
    dell2.Write((feedback * outl) + fm_left);
    outl = (feedback * outl) + ((1.0f - feedback) * fm_left);

    delr.Write((feedback * outr) + fm_right);
    delr2.Write((feedback * outr) + fm_right);
    outr = (feedback * outr) + ((1.0f - feedback) * fm_right);
}

void UpdateKnobs(float &k1, float &k2) {
    k1 = hw.knob1.Process();
    k2 = hw.knob2.Process();

    switch(mode) {
        case CHRDEL:
            drywet = k1;
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k1 * 0.2f; 
            crs.SetLfoDepth(4.0f + k1 * 1.1f);
            crs2.SetLfoDepth(5.0f + k1 * 1.2f);
            crs3.SetLfoDepth(6.0f + k1 * 0.9f);
            crs4.SetLfoDepth(7.0f + k1 * 0.8f);
            crs.SetLfoFreq(k2 * 0.6f);
            crs2.SetLfoFreq(k2 * 0.7f);
            crs3.SetLfoFreq(k2 * 0.8f); 
            crs4.SetLfoFreq(k2 * 0.9f); 
            break;
        case DEL:
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k2 * 0.8f; // Reduced max feedback
            break;
        case COR:
            drywet = k1;
            crs.SetLfoDepth(4.0f + k1 * 1.1f);
            crs2.SetLfoDepth(5.0f + k1 * 1.2f);
            crs3.SetLfoDepth(6.0f + k1 * 0.9f);
            crs4.SetLfoDepth(7.0f + k1 * 0.8f);
            crs.SetLfoFreq(k2 * 0.6f);
            crs2.SetLfoFreq(k2 * 0.7f);
            crs3.SetLfoFreq(k2 * 0.8f); 
            crs4.SetLfoFreq(k2 * 0.9f); 
            break;
        case PHR:
            drywet = k1;
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k1 * 0.2f; 
            crs.SetLfoDepth(4.0f + k1 * 1.1f);
            crs2.SetLfoDepth(5.0f + k1 * 1.2f);
            crs3.SetLfoDepth(6.0f + k1 * 0.9f);
            crs4.SetLfoDepth(7.0f + k1 * 0.8f);
            crs.SetLfoFreq(k2 * 0.6f);
            crs2.SetLfoFreq(k2 * 0.7f);
            crs3.SetLfoFreq(k2 * 0.8f); 
            crs4.SetLfoFreq(k2 * 0.9f); 
            break;
        case OCT:
            drywet = k1;
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k1 * 0.2f; 
            crs.SetLfoDepth(4.0f + k1 * 1.1f);
            crs2.SetLfoDepth(5.0f + k1 * 1.2f);
            crs3.SetLfoDepth(6.0f + k1 * 0.9f);
            crs4.SetLfoDepth(7.0f + k1 * 0.8f);
            crs.SetLfoFreq(k2 * 0.6f);
            crs2.SetLfoFreq(k2 * 0.7f);
            crs3.SetLfoFreq(k2 * 0.8f); 
            crs4.SetLfoFreq(k2 * 0.9f); 
            break;
        case FM:
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k2 * 0.8f; // Reduced max feedback
            drywet = k1;
            break;
    }
}

void UpdateEncoder() {
    mode = mode + hw.encoder.Increment();
    mode = (mode % 6 + 6) % 6;
}

void UpdateLeds(float k1, float k2) {
    hw.led1.Set(
        k1 * (mode == 2), k1 * (mode == 1), k1 * (mode == 0 || mode == 3 || mode == 5));
    hw.led2.Set(
        k2 * (mode == 3 || mode == 5), k2 * (mode == 2 || mode == 4), k2 * (mode == 0 || mode == 4));

    hw.UpdateLeds();
}

void Controls() {
    float k1, k2;
    delayTarget = feedback = drywet = 0;

    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();

    UpdateKnobs(k1, k2);
    UpdateEncoder();
    UpdateLeds(k1, k2);
}

void HandleMidiMessage(MidiEvent m) {
    switch(m.type) {
        case NoteOn: {
            auto p = m.AsNoteOn();
            if(p.velocity != 0)
                fm_synth.NoteOn(p.note, p.velocity);
            else
                fm_synth.NoteOff(p.note);
            break;
        }
        case NoteOff: {
            auto p = m.AsNoteOff();
            fm_synth.NoteOff(p.note);
            break;
        }
        case ControlChange: {
            auto p = m.AsControlChange();
            fm_synth.SetParam(p.control_number, static_cast<float>(p.value) / 127.0f);
            break;
        }
        case PitchBend: {
            auto p = m.AsPitchBend();
            float bendVal = static_cast<float>(p.value);
            float norm = (bendVal / 8192.0f);
            fm_synth.SetPitchBend(norm);
            break;
        }
        default: break;
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t size) {
    Controls();
    
    for(size_t i = 0; i < size; i += 2) {
        // Process FM synth
        float fm_left, fm_right;
        fm_synth.Process(&fm_left, &fm_right, 1);
        
        // Process effects
        float outl, outr;
        switch(mode) {
            case CHRDEL: GetReverbSample(outl, outr, fm_left, fm_right); break;
            case DEL: GetDelaySample(outl, outr, fm_left, fm_right); break;
            case COR: GetChorusSample(outl, outr, fm_left, fm_right); break;
            case PHR: GetPhaserSample(outl, outr, fm_left, fm_right); break;
            case OCT: GetOctaveSample(outl, outr, fm_left, fm_right); break;
            case FM: GetFmSample(outl, outr, fm_left, fm_right); break;
            default: outl = outr = 0;
        }
        
        // Safe output with clamping
        out[i] = fclamp(outl * 1.2f, -0.9f, 0.9f);
        out[i + 1] = fclamp(outr * 1.5f, -0.9f, 0.9f);
    }
}

int main(void) {
    // Initialize hardware
    hw.Init();
    hw.SetAudioBlockSize(4);
    float samplerate = hw.AudioSampleRate();

    // Initialize FM synth
    fm_synth.Init(samplerate);

    // Initialize effects
    dell.Init();
    delr.Init();
    dell2.Init();
    delr2.Init();
    crs.Init(samplerate);
    crs2.Init(samplerate);
    crs3.Init(samplerate);
    crs4.Init(samplerate);
    psr.Init(samplerate);
    psr2.Init(samplerate);
    pst.Init(samplerate);

    // Set initial effect parameters
    currentDelay = delayTarget = samplerate * 0.75f;
    dell.SetDelay(currentDelay);
    delr.SetDelay(currentDelay);
    dell2.SetDelay(currentDelay + 500);
    delr2.SetDelay(currentDelay + 1000);

    crs.SetFeedback(.1f);
    crs.SetDelay(.7f);
    crs2.SetFeedback(.1f);
    crs2.SetDelay(.82f);
    crs3.SetFeedback(.1f);
    crs3.SetDelay(.9f);
    crs4.SetFeedback(.1f);
    crs4.SetDelay(.97f);

    numstages = 4;
    psr.SetPoles(numstages);
    psr2.SetPoles(numstages);
    freqtarget = freq = 0.f;
    lfotarget = lfo = 0.f;

    pst.SetTransposition(12.0f);
    octDelSize = 256;
    pst.SetDelSize(octDelSize);

    // Start audio
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
