#include <cstdio>
#include <cmath>
#include "../src/amp/amp.h"

int fails = 0;
void check(bool ok, const char* name){ printf("%s  %s\n", ok?"PASS":"FAIL", name); if(!ok) fails++; }

int main(){
    const int SR = 48000;

    // 1. Everything bypassed must be bit-identical to the input.
    {
        Amp a; a.init(SR); a.flipCabinet(); a.flipEQ(); a.flipGate();   // clean path: no speaker, tone stack or gate
        bool same = true;
        for (int i=0;i<SR;i++){ float x = sin(i*0.01f)*0.8f; if (a.processAudio(x)!=x) same=false; }
        check(same, "bypass is transparent (output == input)");
    }

    // 1b. The cabinet must actually be initialised and filtering. This fails if
    //     cabinet.init() is ever forgotten again - the whole point of the test.
    {
        Amp a; a.init(SR);
        auto level = [&](float f){
            Amp b; b.init(SR); float pk=0;
            for (int i=0;i<SR/2;i++){
                float y=b.processAudio(sinf(2.0f*3.14159265f*f*i/SR));
                if (i>SR/4 && fabs(y)>pk) pk=fabs(y);
            }
            return 20.0f*log10f(pk>1e-9f?pk:1e-9f);
        };
        float mid = level(800.0f), top = level(12000.0f);
        check(fabs(mid) < 3.0f,      "cabinet leaves the mids roughly alone");
        check(top < mid - 20.0f,     "cabinet rolls off the high fizz (>20 dB down)");
    }

    // 2. Distortion must be symmetric: f(-x) == -f(x).
    {
        Distortion d; d.new_drive=5; d.cur_drive=5;
        bool sym = true; float maxout = 0;
        for (float x=-1.0f; x<=1.0f; x+=0.01f){
            float a=d.applyDistortion(x), b=d.applyDistortion(-x);
            if (fabs(a+b) > 1e-6f) sym=false;
            if (fabs(a)>maxout) maxout=fabs(a);
        }
        check(sym, "distortion is symmetric (no DC offset / octave-up)");
        check(maxout<=1.0f, "distortion stays within +/-1");
    }

    // 3. Delay index math over absurd knob settings (ASan catches any OOB).
    {
        Delay d; d.init(SR);
        float times[] = {-5.0f, 0.0f, 1e-9f, 0.3f, 1.999f, 2.0f, 50.0f};
        for (float t : times){ d.delay_t = t; for (int i=0;i<SR;i++) d.applyDelay(sin(i*0.01f)*0.5f); }
        check(isfinite(d.applyDelay(0.0f)), "delay survives out-of-range times, stays finite");
    }

    // 4. Feedback must decay, never run away.
    {
        Amp a; a.init(SR); a.flipCabinet(); a.flipEQ(); a.flipDelay(); a.adjustDelayTime(0.1f); a.adjustDelayFadeFactor(5.0f);
        float peak = 0;
        for (int i=0;i<SR*8;i++){ float x=(i<1000)?0.9f:0.0f; float y=a.processAudio(x); if(fabs(y)>peak) peak=fabs(y); }
        check(peak<=1.0f && isfinite(peak), "feedback clamped: no runaway even when asked for >1");
    }

    // 5. Looper: record/close now go through the request flags.
    {
        Amp a; a.init(SR); a.flipCabinet(); a.flipEQ(); a.flipGate(); a.flipLoop();
        a.toggleLoopRecord(); a.applyControls();          // main thread asks, audio thread acts
        for (int i=0;i<1000;i++) a.processAudio((float)(i+1)/1000.0f);
        a.toggleLoopRecord(); a.applyControls();
        check(a.loop.loopLen==1000, "loop length == samples recorded");

        bool ok = true;
        for (int lap=0; lap<3; lap++)
            for (int i=0;i<1000;i++){
                float y = a.processAudio(0.0f);
                if (fabs(y - (float)(i+1)/1000.0f) > 1e-6f) ok = false;
            }
        check(ok, "loop plays back exactly, same every lap (no off-by-one drift)");

        a.toggleLoopOverdub(); a.applyControls();
        for (int i=0;i<1000;i++) a.processAudio(0.1f);
        check(a.loop.loopLen==1000, "overdub leaves loop length alone");
    }

    // 6. Recording past the 60 s buffer must stop cleanly.
    {
        Amp a; a.init(SR); a.flipLoop(); a.toggleLoopRecord(); a.applyControls();
        for (int i=0;i<SR*61;i++) a.processAudio(0.5f);
        check(a.loop.loopLen==SR*60 && !a.loop.recording, "buffer-full auto-closes the loop");
    }

    // 7. The request-flag handshake itself: applied exactly once, never re-fires.
    {
        Amp a; a.init(SR);
        a.resetDelay();                                    // main thread raises it
        check(a.delay.clearReq.load()==true,  "request pending before the callback runs");
        a.applyControls();                                 // audio thread consumes it
        check(a.delay.clearReq.load()==false, "request cleared once applied");
        a.applyControls();
        check(a.delay.clearReq.load()==false, "request does not re-fire");
    }

    // 8. A clear request must actually silence the delay's echoes.
    {
        Amp a; a.init(SR); a.flipCabinet(); a.flipEQ(); a.flipDelay(); a.adjustDelayTime(0.1f);
        for (int i=0;i<1000;i++) a.processAudio(0.9f);     // fill the line with echoes
        a.resetDelay(); a.applyControls();
        float sum=0; for (int i=0;i<SR;i++) sum += fabs(a.processAudio(0.0f));
        check(sum==0.0f, "reset delay clears the echoes");
    }

    // 9. Headroom: the cabinet's +3.5 dB bump must not push us wildly past full scale.
    {
        Amp a; a.init(SR);
        float peak=0;
        for (int i=0;i<SR;i++){ float y=a.processAudio(0.9f*sinf(2.0f*3.14159265f*2500.0f*i/SR));
                                if (fabs(y)>peak) peak=fabs(y); }
        printf("     (hot 2.5 kHz input 0.9 -> peak %.3f before the callback clamp)\n", peak);
        check(peak < 2.0f, "cabinet boost stays within one clamp's worth of headroom");
    }

    // 10. Noise gate behaviour.
    {
        const float SR2 = 48000;
        // a) hiss well under the threshold must be crushed
        {   NoiseGate gt; gt.init(SR2); gt.threshold = 0.00316f;   // -50 dBFS
            float pk=0;
            for (int i=0;i<SR2;i++){ float y=gt.applyGate(0.0001f*sinf(i*0.1f)); if(fabs(y)>pk) pk=fabs(y); }
            check(pk < 0.0001f*0.05f, "gate crushes hiss below the threshold");
        }
        // b) a real note must pass essentially untouched once open
        {   NoiseGate gt; gt.init(SR2); gt.threshold = 0.00316f;
            float pk=0;
            for (int i=0;i<SR2;i++){ float y=gt.applyGate(0.2f*sinf(2.0f*3.14159265f*220.0f*i/SR2));
                                     if (i>SR2/10 && fabs(y)>pk) pk=fabs(y); }
            check(pk > 0.19f, "gate passes a real note at full level");
        }
        // c) the attack must survive - gate must be open within a few ms of a note starting
        {   NoiseGate gt; gt.init(SR2); gt.threshold = 0.00316f;
            int openAt = -1;
            for (int i=0;i<SR2/10;i++){
                float y = gt.applyGate(0.2f*sinf(2.0f*3.14159265f*220.0f*i/SR2));
                float want = 0.2f*fabs(sinf(2.0f*3.14159265f*220.0f*i/SR2));
                if (openAt<0 && want>0.1f && fabs(y) > want*0.9f) openAt = i;
            }
            float ms = openAt*1000.0f/SR2;
            printf("     (gate fully open %.1f ms after the note starts)\n", ms);
            check(openAt >= 0 && ms < 15.0f, "gate opens fast enough not to clip the pick attack");
        }
        // d) no chatter: watch the gate's own multiplier, not the waveform (the sine
        //    crosses zero 440x a second, which is not the gate opening and closing)
        {   NoiseGate gt; gt.init(SR2); gt.threshold = 0.01f;
            float prev=1; int flips=0;
            for (int i=0;i<SR2;i++){
                float amp = 0.2f*expf(-3.0f*i/SR2);                 // decaying note
                gt.applyGate(amp*sinf(2.0f*3.14159265f*220.0f*i/SR2));
                if (i>100){ float now = gt.g>0.5f?1.0f:0.0f; if(now!=prev) flips++; prev=now; }
            }
            check(flips <= 1, "gate does not chatter as a note decays through the threshold");
        }
    }

    // 11. Reverb: must make a decaying tail, stay stable, and respect mix.
    {
        const float SR2 = 48000;
        auto rt60 = [&](float rs){
            Reverb r; r.init(SR2); r.roomSize=rs; r.mix=1.0f;
            const int W=2400; float first=0;
            for (int w=0; w<240; w++){
                double sum=0;
                for (int i=0;i<W;i++){ float y=r.applyReverb((w==0&&i==0)?1.0f:0.0f); sum+=(double)y*y; }
                float rms=sqrtf(sum/W);
                if (w==0) first=rms;
                else if (rms < first*0.001f) return w*W/SR2;
            }
            return 99.0f;
        };
        float small = rt60(0.0f), big = rt60(0.9f);
        printf("     (reverb tail: roomSize 0.0 -> %.2f s, roomSize 0.9 -> %.2f s)\n", small, big);
        check(small > 0.1f && small < 1.5f, "small room gives a short tail");
        check(big > small*2.0f && big < 12.0f, "big room gives a much longer tail");

        // stability at the hottest setting: sustained loud input must not run away
        {   Reverb r; r.init(SR2); r.roomSize=0.9f; r.mix=1.0f;
            float peak=0;
            for (int i=0;i<(int)(SR2*10);i++){
                float y=r.applyReverb(0.9f*sinf(2.0f*3.14159265f*110.0f*i/SR2));
                if (fabs(y)>peak) peak=fabs(y);
            }
            check(std::isfinite(peak) && peak < 4.0f, "reverb stays stable at max room size");
        }
        // mix=0 must be perfectly dry
        {   Reverb r; r.init(SR2); r.roomSize=0.9f; r.mix=0.0f;
            bool dry=true;
            for (int i=0;i<SR2;i++){ float x=0.5f*sinf(i*0.01f); if (fabs(r.applyReverb(x)-x)>1e-6f) dry=false; }
            check(dry, "reverb mix=0 passes the dry signal untouched");
        }
        // the mix knob should change the sound, not the volume. Noise in, RMS out,
        // compared against the dry level at each setting.
        {   float worst=0;
            for (float m : {0.0f,0.2f,0.5f,0.8f,1.0f}){
                Reverb r; r.init(SR2); r.mix=m;
                unsigned seed=1; double sum=0; int n=0;
                for (int i=0;i<SR2*3;i++){
                    seed = seed*1664525u + 1013904223u;
                    float x = 0.3f * ((float)(seed>>8)/8388608.0f - 1.0f);
                    float y = r.applyReverb(x);
                    if (i>SR2){ sum += (double)y*y; n++; }
                }
                float dB = 20.0f*log10f(sqrtf(sum/n)/0.1732f);
                if (fabs(dB)>worst) worst=fabs(dB);
            }
            printf("     (worst level shift across the mix knob: %.1f dB)\n", worst);
            check(worst < 1.5f, "reverb mix knob holds a steady level");
        }
        // damping darkens the tail: measure sample-to-sample change, which rises with
        // high-frequency content
        {   float bright[2];
            float damps[2] = {0.0f, 0.9f};
            for (int k=0;k<2;k++){
                Reverb r; r.init(SR2); r.mix=1.0f; r.roomSize=0.7f; r.damp=damps[k];
                float prev=0; double hf=0, tot=0;
                for (int i=0;i<SR2*3;i++){
                    float x = (i<480)? 0.5f*sinf(2.0f*3.14159265f*2000.0f*i/SR2) : 0.0f;
                    float y = r.applyReverb(x);
                    if (i>SR2/2){ hf += fabs(y-prev); tot += fabs(y); }
                    prev = y;
                }
                bright[k] = (float)(hf/(tot+1e-12));
            }
            printf("     (tail brightness: damp 0.0 -> %.3f, damp 0.9 -> %.3f)\n", bright[0], bright[1]);
            check(bright[1] < bright[0]*0.5f, "damping makes the reverb tail darker");
        }
    }

    // ---- auto-wah ----
    {
        const float SR2 = 48000.0f;

        // the resonant peak must follow how hard you play
        float fQuiet, fLoud;
        {   Wah w; w.init(SR2);
            for (int i=0;i<(int)SR2;i++) w.applyWah(0.02f*sinf(2.0f*3.14159265f*220.0f*i/SR2));
            fQuiet = w.curF;
        }
        {   Wah w; w.init(SR2);
            for (int i=0;i<(int)SR2;i++) w.applyWah(0.25f*sinf(2.0f*3.14159265f*220.0f*i/SR2));
            fLoud = w.curF;
        }
        printf("     (wah peak: quiet %.0f Hz -> loud %.0f Hz)\n", fQuiet, fLoud);
        check(fLoud > fQuiet*2.0f, "wah sweeps up as you play harder");

        // and fall back down when you stop
        {   Wah w; w.init(SR2);
            for (int i=0;i<(int)SR2;i++) w.applyWah(0.25f*sinf(2.0f*3.14159265f*220.0f*i/SR2));
            float opened = w.curF;
            for (int i=0;i<(int)SR2;i++) w.applyWah(0.0f);
            check(w.curF < opened*0.6f, "wah falls back when the note dies away");
        }

        // coefficients are rebuilt in steps of 32 samples - that must not click.
        // A clean sine can only move so far between samples; anything much larger
        // is the filter jumping.
        {   Wah w; w.init(SR2);
            float prev=0, maxJump=0, sigMax=0;
            for (int i=0;i<(int)(SR2*2);i++){
                float shape = (i<SR2)? (float)i/SR2 : 2.0f-(float)i/SR2;   //sweep hard
                float y = w.applyWah(0.4f*shape*sinf(2.0f*3.14159265f*300.0f*i/SR2));
                if (i>100 && fabs(y-prev)>maxJump) maxJump=fabs(y-prev);
                if (fabs(y)>sigMax) sigMax=fabs(y);
                prev=y;
            }
            float expected = 2.0f*3.14159265f*300.0f/SR2*sigMax;
            printf("     (wah step vs a clean sine's own slew: ratio %.2f)\n", maxJump/expected);
            check(maxJump < expected*1.5f, "wah sweep is smooth (no zipper from stepped coefficients)");
        }

        // stable and bounded however the knobs are set
        {   bool ok=true;
            float qs[2]={1.0f,8.0f}, ss[2]={0.5f,30.0f};
            for (int qi=0;qi<2;qi++) for (int si=0;si<2;si++){
                Wah w; w.init(SR2); w.resonance=qs[qi]; w.sensitivity=ss[si];
                float pk=0;
                for (int i=0;i<(int)(SR2*3);i++){
                    float y=w.applyWah(0.9f*sinf(2.0f*3.14159265f*110.0f*i/SR2));
                    if (fabs(y)>pk) pk=fabs(y);
                }
                if (!std::isfinite(pk) || pk>20.0f) ok=false;
            }
            check(ok, "wah stays finite and bounded at every knob extreme");
        }

        // a hard strum through the whole amp must not slam into the output clamp
        {   Amp a; a.init((int)SR2); a.flipWah();
            float pk=0;
            for (int i=0;i<(int)(SR2*2);i++){
                float envl = expf(-2.0f*(i%(int)SR2)/SR2);
                float x = 0.5f*envl*(sinf(2.0f*3.14159265f*196.0f*i/SR2)
                                   + 0.6f*sinf(2.0f*3.14159265f*587.0f*i/SR2));
                float y = a.processAudio(x);
                if (fabs(y)>pk) pk=fabs(y);
            }
            printf("     (hard strum through the wah: peak %.3f)\n", pk);
            check(pk <= 1.0f, "wah resonance does not push a strum past full scale");
        }
    }

    // ---- tremolo ----
    {
        const float SR2 = 48000.0f;

        // Amp::init() must reach every effect. Tremolo was added without being
        // wired in here, and an uninitialised sampleRate turned the whole output
        // to NaN - which the callback's isfinite guard then muted. Same class of
        // bug as the forgotten cabinet.init().
        {   Amp a; a.init((int)SR2); a.flipTremolo();
            int bad=0;
            for (int i=0;i<(int)SR2;i++)
                if (!std::isfinite(a.processAudio(0.5f*sinf(2.0f*3.14159265f*220.0f*i/SR2)))) bad++;
            check(bad==0, "every effect is initialised by Amp::init (tremolo produces finite audio)");
        }

        // the volume really should wobble, at roughly the rate asked for
        {   Tremolo t; t.init(SR2); t.rate=5.0f; t.depth=1.0f;
            int crossings=0; bool above=false;
            float mn=1e9f, mx=-1e9f;
            for (int i=0;i<(int)(SR2*2);i++){
                float y=t.applyTremolo(1.0f);      // DC in, so output == the LFO shape
                if (y>mn) {}
                if (y<mn) mn=y;
                if (y>mx) mx=y;
                bool nowAbove = y>0.5f;
                if (nowAbove && !above) crossings++;
                above=nowAbove;
            }
            printf("     (tremolo at 5 Hz: %d cycles in 2 s, depth %.2f..%.2f)\n", crossings, mn, mx);
            check(crossings>=9 && crossings<=11, "tremolo runs at the rate you set");
            check(mn<0.05f && mx>0.95f, "tremolo at full depth swings all the way");
        }

        // depth 0 must be a true bypass, not a slight wobble
        {   Tremolo t; t.init(SR2); t.rate=5.0f; t.depth=0.0f;
            bool flat=true;
            for (int i=0;i<(int)SR2;i++){ float x=0.3f*sinf(i*0.01f); if (fabs(t.applyTremolo(x)-x)>1e-6f) flat=false; }
            check(flat, "tremolo depth 0 passes the signal untouched");
        }

        // never inverts the signal or exceeds unity, at any setting
        {   bool ok=true;
            float rates[3]={0.1f,5.0f,20.0f}, depths[3]={0.0f,0.5f,1.0f};
            for (int r=0;r<3;r++) for (int d=0;d<3;d++){
                Tremolo t; t.init(SR2); t.rate=rates[r]; t.depth=depths[d];
                for (int i=0;i<(int)SR2;i++){
                    float y=t.applyTremolo(1.0f);
                    if (!std::isfinite(y) || y<-1e-6f || y>1.0f+1e-6f) ok=false;
                }
            }
            check(ok, "tremolo gain stays within 0..1 at every rate and depth");
        }
    }

    printf("\n%s\n", fails? "SOME TESTS FAILED" : "all tests passed");
    return fails;
}
