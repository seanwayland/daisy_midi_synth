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

// Synth Constants
constexpr size_t kMaxVoices = 16;
constexpr size_t kVoicesPerSide = kMaxVoices / 2;
constexpr float kPitchBendRange = 7.0f;
constexpr float kVelocityCutoffBoost = 1200.0f;
constexpr float kVelocityPwModAmount = 0.05f;
constexpr float kBasePwLeft = 0.82f;
constexpr float kBasePwRight = 0.86f;
constexpr float kPwKeyTrackingAmount = 0.0009f;
constexpr float kNoteFilterTrackingAmount = 10.0f;
constexpr float kFilterEnvAmount = 2000.0f;

// LFO Parameters
constexpr float kLFOMinRate = 0.1f;    // Minimum LFO rate in Hz
constexpr float kLFOMaxRate = 10.0f;   // Maximum LFO rate in Hz
constexpr float kLFOMinDepth = 0.0f;   // Minimum pitch modulation depth (semitones)
constexpr float kLFOMaxDepth = 0.5f;   // Maximum pitch modulation depth (semitones)

DaisyPod hw;

// Global filter envelope amount controlled by mod wheel
float globalFilterEnvAmount = 0.0f;

// LFO controls
float lfoLeftRate = 0.4f;    // Default LFO rate for left channel (Hz)
float lfoLeftDepth = 0.1f;   // Default LFO depth for left channel (semitones)
float lfoRightRate = 0.3f;   // Default LFO rate for right channel (Hz)
float lfoRightDepth = 0.08f;  // Default LFO depth for right channel (semitones)

// LFO objects
Oscillator lfoLeft, lfoRight;

// Synth Voice Structure
struct Voice {
    Oscillator osc;
    Svf        filt;
    Adsr       env;
    Adsr       filter_env;
    bool       active = false;
    int        note = -1;
    float      base_cutoff = 1000.0f;
    float      resonance = 0.1f;
    bool       gate = false;
    static constexpr int kControlRateDivider = 8;
    int control_counter = 0;

    float pitch_bend_semitones = 0.0f;
    float velocity_amp = 1.0f;
    float velocity_cutoff_boost = 0.0f;
    float current_pw = 0.5f;
    float current_freq = 0.0f;
    bool  is_left = false; // Track which side this voice belongs to

    void Init(float samplerate) {
        osc.Init(samplerate);
        osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
        osc.SetAmp(1.0f);
        osc.SetPw(0.5f);

        filt.Init(samplerate);
        filt.SetFreq(base_cutoff);
        filt.SetRes(resonance);

        // Initialize amplitude envelope
        env.Init(samplerate);
        env.SetTime(ADSR_SEG_ATTACK, 0.04f);
        env.SetTime(ADSR_SEG_DECAY, 0.9f);
        env.SetSustainLevel(0.9f);
        env.SetTime(ADSR_SEG_RELEASE, 0.015f);

        // Initialize filter envelope
        filter_env.Init(samplerate);
        filter_env.SetTime(ADSR_SEG_ATTACK, 0.009f);
        filter_env.SetTime(ADSR_SEG_DECAY, 0.03f);
        filter_env.SetSustainLevel(0.01f);
        filter_env.SetTime(ADSR_SEG_RELEASE, 0.5f);
    }

    void NoteOn(int n, float velocity, bool left) {
        note = n;
        active = true;
        gate = true;
        is_left = left;
        velocity_amp = velocity / 127.0f;
        velocity_cutoff_boost = velocity_amp * kVelocityCutoffBoost;
        
        float base_pw = left ? kBasePwLeft : kBasePwRight;
        float key_tracking = (127.0f - n) * kPwKeyTrackingAmount;
        float vel_mod = (1.0f - velocity_amp) * kVelocityPwModAmount;
        
        current_pw = base_pw + key_tracking + vel_mod;
        current_pw = fclamp(current_pw, 0.01f, 0.99f);
        osc.SetPw(current_pw);
        
        UpdateFilter();
        UpdateFrequency();
    }

    void NoteOff(int n) {
        if(note == n) {
            gate = false;
        }
    }

    void SetFilter(float cutoffHz, float res) {
        base_cutoff = cutoffHz;
        resonance = res;
        UpdateFilter();
    }

    void SetShape(int shape) {
        switch(shape) {
            case 1: 
                osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE); 
                break;
            case 2: 
                osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SAW); 
                break;
            default:
                osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
                break;
        }
    }

    void UpdateFilter() {
        float filter_env_val = filter_env.Process(gate);
        float note_tracking = (127.0f - note) * kNoteFilterTrackingAmount;
        float env_modulation = filter_env_val * globalFilterEnvAmount * kFilterEnvAmount;
        float modulated_cutoff = base_cutoff + velocity_cutoff_boost - note_tracking + 0.35*env_modulation;
        filt.SetFreq(fclamp(modulated_cutoff, 20.0f, 20000.0f));
        filt.SetRes(resonance);
    }

    void SetPitchBend(float semitones) {
        pitch_bend_semitones = semitones;
        UpdateFrequency();
    }

    void UpdateFrequency() {
        if(note >= 0) {
            current_freq = mtof(static_cast<float>(note) + pitch_bend_semitones);
            osc.SetFreq(current_freq);
        }
    }

    void ApplyLFOModulation() {
        float lfoValue = is_left ? lfoLeft.Process() : lfoRight.Process();
        float lfoDepth = is_left ? lfoLeftDepth : lfoRightDepth;
        float modulated_freq = current_freq * powf(2.0f, lfoValue * lfoDepth / 12.0f);
        osc.SetFreq(modulated_freq);
    }

    float Process() {
        if(!active) return 0.0f;

        float amp = env.Process(gate);
        if(amp <= 0.0001f && !gate) {
            active = false;
            return 0.0f;
        }
        
        if(++control_counter >= kControlRateDivider) {
            control_counter = 0;
            UpdateFilter();
            ApplyLFOModulation();
        }
        
        // Apply LFO modulation every sample
        
        
        float sig = osc.Process() * amp * velocity_amp;
        filt.Process(sig);
        return filt.Low();
    }
};

// Effects Objects
static Chorus crs, crs2, crs3, crs4;
static PitchShifter pst;
static Phaser psr, psr2;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS dell;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delr;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS dell2;
static DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delr2;

// Global Variables
std::vector<Voice> voices_left(kVoicesPerSide);
std::vector<Voice> voices_right(kVoicesPerSide);

float globalCutoffLeft = 1020.0f;
float globalCutoffRight = 1000.0f;
float globalResonance = 0.1f;
float globalPitchBend = 0.0f;

int mode = CHRDEL;
int numstages;
uint32_t octDelSize;
float currentDelay, feedback, delayTarget, freq, freqtarget, lfotarget, lfo;
float drywet;

// Helper Functions
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

void HandleNoteOn(uint8_t note, uint8_t velocity) {
    {
        Voice* voice = FindFreeVoice(voices_left);
        voice->NoteOn(note, velocity, true);
        voice->SetPitchBend(globalPitchBend);
    }
    {
        Voice* voice = FindFreeVoice(voices_right);
        voice->NoteOn(note, velocity, false);
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
                case 1: // Mod wheel - now controls filter envelope amount
                    globalFilterEnvAmount = 2.0*(static_cast<float>(p.value) / 127.0f);
                    globalCutoffLeft = mtof(static_cast<float>(p.value)) + 20.0f;
                    globalCutoffRight = mtof(static_cast<float>(p.value));
                    UpdateFilters();
                    break;
                case 2:
                    // globalResonance = static_cast<float>(p.value) / 400.0f;
                    // UpdateFilters();
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
    
    outl = crs.GetLeft() * drywet *2.0f  + crs3.GetLeft() * drywet*2.0f + inl * (0.5f - drywet) + (feedback * outl*drywet*0.6f) + ((1.0f - feedback) * inl*drywet);
    outr = crs2.GetRight() * drywet*2.0f + crs4.GetRight() * drywet*2.0f + inr * (0.5f - drywet) + (feedback * outr*drywet*0.6f) + ((1.0f - feedback) * inr*drywet);
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
    
    outl = crs.GetLeft() * drywet *2.5f + crs3.GetLeft() * drywet*2.5f + inl * (0.5f - drywet);
    outr = crs2.GetRight() * drywet*2.5f + crs4.GetRight() * drywet*2.5f + inr * (0.5f - drywet);
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

    outl = crs.GetLeft() * drywet*0.5f + psr.Process(inl) * drywet + inl * (1.f - drywet);
    outr = crs.GetRight() * drywet*0.3f + psr2.Process(inr) * drywet + inr * (1.f - drywet);
}

void GetOctaveSample(float &outl, float &outr, float inl, float inr) {
    outl = crs.GetLeft() * drywet*0.2f + pst.Process(inl) * drywet + inl * (1.f - drywet);
    outr = crs.GetRight() * drywet*0.2f + pst.Process(inr) * drywet + inr * (1.f - drywet);
}

void UpdateKnobs(float &k1, float &k2) {
    k1 = hw.knob1.Process();
    k2 = hw.knob2.Process();

    switch(mode) {
        case CHRDEL:
                for(auto& v : voices_left) {
        v.SetShape(1);
    }
    for(auto& v : voices_right) {
        v.SetShape(1);
    }
            drywet = k1;
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k1*0.2f; 
            crs.SetLfoDepth(4.0f + k1*1.1f);
            crs2.SetLfoDepth(5.0f + k1*1.2f);
            crs3.SetLfoDepth(6.0f + k1*0.9f);
            crs4.SetLfoDepth(7.0f + k1*0.8f);
            crs.SetLfoFreq(k2*0.6f);
            crs2.SetLfoFreq(k2*0.7f);
            crs3.SetLfoFreq(k2*0.8f); 
            crs4.SetLfoFreq(k2*0.9f); 
            break;
        case DEL:
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k2;
            break;
        case COR:
            drywet = k1;
            crs.SetLfoDepth(4.0f + k1*1.1f);
            crs2.SetLfoDepth(5.0f + k1*1.2f);
            crs3.SetLfoDepth(6.0f + k1*0.9f);
            crs4.SetLfoDepth(7.0f + k1*0.8f);
            crs.SetLfoFreq(k2*0.6f);
            crs2.SetLfoFreq(k2*0.7f);
            crs3.SetLfoFreq(k2*0.8f); 
            crs4.SetLfoFreq(k2*0.9f); 
            break;
        case PHR:

        for(auto& v : voices_left) {
        v.SetShape(1);
    }
    for(auto& v : voices_right) {
        v.SetShape(2);
    }
            drywet = k1;
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k1*0.2f ; 
            crs.SetLfoDepth(4.0f + k1*1.1f);
            crs2.SetLfoDepth(5.0f + k1*1.2f);
            crs3.SetLfoDepth(6.0f + k1*0.9f);
            crs4.SetLfoDepth(7.0f + k1*0.8f);
            crs.SetLfoFreq(k2*0.6f);
            crs2.SetLfoFreq(k2*0.7f);
            crs3.SetLfoFreq(k2*0.8f); 
            crs4.SetLfoFreq(k2*0.9f); 

            break;
        case OCT:
                // Initialize synth voices
    for(auto& v : voices_left) {
        v.SetShape(1);
    }
    for(auto& v : voices_right) {
        v.SetShape(2);
    }
            drywet = k1;
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k1*0.2f ; 
            crs.SetLfoDepth(4.0f + k1*1.1f);
            crs2.SetLfoDepth(5.0f + k1*1.2f);
            crs3.SetLfoDepth(6.0f + k1*0.9f);
            crs4.SetLfoDepth(7.0f + k1*0.8f);
            crs.SetLfoFreq(k2*0.6f);
            crs2.SetLfoFreq(k2*0.7f);
            crs3.SetLfoFreq(k2*0.8f); 
            crs4.SetLfoFreq(k2*0.9f); 
            break;
    }
}

void UpdateEncoder() {
    mode = mode + hw.encoder.Increment();
    mode = (mode % 5 + 5) % 5;
}

void UpdateLeds(float k1, float k2) {
    hw.led1.Set(
        k1 * (mode == 2), k1 * (mode == 1), k1 * (mode == 0 || mode == 3));
    hw.led2.Set(
        k2 * (mode == 3), k2 * (mode == 2 || mode == 4), k2 * (mode == 0 || mode == 4));

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

void AudioCallback(AudioHandle::InterleavingInputBuffer in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t size) {
    Controls();
    // First process synth voices
    for(size_t i = 0; i < size; i += 2) {
        float synth_left = 0.0f;
        float synth_right = 0.0f;

        for(auto& v : voices_left) {
            synth_left += v.Process();
        }
        for(auto& v : voices_right) {
            synth_right += v.Process();
        }

        synth_left /= static_cast<float>(kVoicesPerSide);
        synth_right /= static_cast<float>(kVoicesPerSide);

        // Then process effects
        float outl, outr;
        switch(mode) {
            case CHRDEL: GetReverbSample(outl, outr, synth_left, synth_right); break;
            case DEL: GetDelaySample(outl, outr, synth_left, synth_right); break;
            case COR: GetChorusSample(outl, outr, synth_left, synth_right); break;
            case PHR: GetReverbSample(outl, outr, synth_left, synth_right); break;
            case OCT: GetReverbSample(outl, outr, synth_left, synth_right); break;
            default: outl = outr = 0;
        }

    
        out[i] = 1.5*outl;
        out[i + 1] = 1.5*outr;
    }
}

int main(void) {
    // Initialize hardware
    hw.Init();
    hw.SetAudioBlockSize(512);
    float samplerate = hw.AudioSampleRate();

    // Initialize LFOs
    lfoLeft.Init(samplerate);
    lfoRight.Init(samplerate);
    lfoLeft.SetWaveform(Oscillator::WAVE_SIN);
    lfoRight.SetWaveform(Oscillator::WAVE_SIN);
    lfoLeft.SetFreq(lfoLeftRate);
    lfoRight.SetFreq(lfoRightRate);
    lfoLeft.SetAmp(1.0f);
    lfoRight.SetAmp(1.0f);

    // Initialize synth voices
    for(auto& v : voices_left) {
        v.Init(samplerate);
        v.SetFilter(globalCutoffLeft, globalResonance);
    }
    for(auto& v : voices_right) {
        v.Init(samplerate);
        v.SetFilter(globalCutoffRight, globalResonance);
    }

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
    dell2.SetDelay(currentDelay+500);
    delr2.SetDelay(currentDelay+1000);

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
        
        // Update LFO rates based on some control (you can map these to knobs or MIDI CCs)
        // For now they're fixed at their default values
        lfoLeft.SetFreq(lfoLeftRate);
        lfoRight.SetFreq(lfoRightRate);
    }
}
