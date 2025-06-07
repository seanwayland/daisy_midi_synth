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
constexpr float kVelocityCutoffBoost = 1000.0f;
constexpr float kVelocityPwModAmount = 0.05f;
constexpr float kBasePwLeft = 0.82f;
constexpr float kBasePwRight = 0.86f;
constexpr float kPwKeyTrackingAmount = 0.0005f;
constexpr float kNoteFilterTrackingAmount = 10.0f; // Added constant for note-based filter tracking

DaisyPod hw;

// Synth Voice Structure
struct Voice {
    Oscillator osc[2]; // Two oscillators
    Svf        filt[2]; // Two filters
    Adsr       env[2]; // Two envelopes
    bool       active = false;
    int        note = -1;
    float      base_cutoff = 3000.0f;
    float      resonance = 0.01f;
    bool       gate = false;

    float pitch_bend_semitones = 0.0f;
    float velocity_amp = 1.0f;
    float velocity_cutoff_boost = 0.8f;
    float current_pw[2] = {0.85f, 0.86f}; // Two pulse widths

    void Init(float samplerate) {
        for (int i = 0; i < 2; i++) {
            osc[i].Init(samplerate);
            osc[i].SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
            osc[i].SetAmp(0.5f); // Each oscillator at half amplitude
            osc[i].SetPw(0.5f);

            filt[i].Init(samplerate);
            filt[0].SetFreq(base_cutoff);
            filt[0].SetRes(resonance);
            filt[1].SetFreq(base_cutoff+3000.0f);
            filt[1].SetRes(resonance);

            env[0].Init(samplerate);
            env[0].SetTime(ADSR_SEG_ATTACK, 0.04f);
            env[0].SetTime(ADSR_SEG_DECAY, 4.0f);
            env[0].SetSustainLevel(0.7f);
            env[0].SetTime(ADSR_SEG_RELEASE, 0.015f);

            env[1].Init(samplerate);
            env[1].SetTime(ADSR_SEG_ATTACK, 0.04f);
            env[1].SetTime(ADSR_SEG_DECAY, 1.5f);
            env[1].SetSustainLevel(0.2f);
            env[1].SetTime(ADSR_SEG_RELEASE, 0.015f);
        }
    }

    void NoteOn(int n, float velocity, bool is_left) {
        note = n;
        active = true;
        gate = true;
        velocity_amp = velocity / 127.0f;
        velocity_cutoff_boost = velocity_amp * kVelocityCutoffBoost;
        
        float base_pw = is_left ? kBasePwLeft : kBasePwRight;
        float key_tracking = (127.0f - n) * kPwKeyTrackingAmount;
        float vel_mod = (1.0f - velocity_amp) * kVelocityPwModAmount;
        
        // Set slightly different pulse widths for the two oscillators
        current_pw[0] = fclamp(base_pw + key_tracking + vel_mod, 0.01f, 0.99f);
        current_pw[1] = fclamp(base_pw + key_tracking * 0.8f + vel_mod * 1.2f, 0.01f, 0.99f);
        
        osc[0].SetPw(current_pw[0]);
        osc[1].SetPw(current_pw[1]);
        
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
                osc[0].SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE); 
                osc[1].SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
                break;
            case 2: 
                osc[0].SetWaveform(Oscillator::WAVE_POLYBLEP_SAW); 
                osc[1].SetWaveform(Oscillator::WAVE_POLYBLEP_SAW);
                break;
            // Add more waveform cases as needed
            default:
                osc[0].SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
                osc[1].SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
                break;
        }
    }

    void UpdateFilter() {
        // Add note-based filter tracking - higher notes will have slightly brighter filter
        float note_tracking = (127.0f - note) * kNoteFilterTrackingAmount;
        float modulated_cutoff = base_cutoff + velocity_cutoff_boost - note_tracking;
        
        // Set slightly different cutoff frequencies for the two filters
        filt[0].SetFreq(fclamp(modulated_cutoff, 20.0f, 20000.0f));
        filt[1].SetFreq(fclamp(modulated_cutoff * 0.9f, 20.0f, 20000.0f));
        
        filt[0].SetRes(resonance);
        filt[1].SetRes(resonance * 1.1f);
    }

    void SetPitchBend(float semitones) {
        pitch_bend_semitones = semitones;
        UpdateFrequency();
    }

    void UpdateFrequency() {
        if(note >= 0) {
            float freq = mtof(static_cast<float>(note) + pitch_bend_semitones);
            osc[0].SetFreq(freq);
            // Slightly detune the second oscillator
            osc[1].SetFreq(freq * 1.005f);
        }
    }

    float Process() {
        if(!active) return 0.0f;

        float amp[2];
        amp[0] = env[0].Process(gate);
        amp[1] = env[1].Process(gate);
        
        if(amp[0] <= 0.0001f && amp[1] <= 0.0001f && !gate) {
            active = false;
            return 0.0f;
        }
        
        // Process both oscillators and filters
        float sig = 0.0f;
        // for (int i = 0; i < 2; i++) {
        //     sig += osc[i].Process() * amp[i] * velocity_amp;
        //     filt[i].Process(sig);
        //     sig = filt[i].Low();
        // }
        float osc0 = osc[0].Process() * amp[0] * velocity_amp;
        filt[0].Process(osc0);
        float sig0 = filt[0].Low();
        float osc1 = osc[1].Process() * amp[1] * velocity_amp;
        filt[1].Process(osc1);
        float sig1 = filt[1].Low();
        sig = sig0 + sig1;

        
        return sig;
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
float globalResonance = 0.4f;
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
                case 1:
                    globalCutoffLeft = mtof(static_cast<float>(p.value)) + 20.0f;
                    globalCutoffRight = mtof(static_cast<float>(p.value));
                    UpdateFilters();
                    break;
                case 2:
                    //globalResonance = static_cast<float>(p.value) / 400.0f;
                    //UpdateFilters();
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
            feedback = k1*0.2f + k2*0.1f; 
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
            feedback = k1*0.2f + k2*0.1f; 
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
        v.SetShape(2);
    }
    for(auto& v : voices_right) {
        v.SetShape(2);
    }
            drywet = k1;
            delayTarget = hw.knob1.Process() * MAX_DELAY;
            feedback = k1*0.2f + k2*0.1f; 
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
    hw.SetAudioBlockSize(128);
    float samplerate = hw.AudioSampleRate();

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
    }
}
