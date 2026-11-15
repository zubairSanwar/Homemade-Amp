#include <cstdio>
#include <cmath>
#include "structs.inc"

int fails = 0;
void check(bool ok, const char* name){ printf("%s  %s\n", ok?"PASS":"FAIL", name); if(!ok) fails++; }

int main(){
    const int SR = 48000;

    // 1. Everything bypassed must be bit-identical to the input.
    {
        Amp a; a.init(SR); a.flipCabinet(); a.flipEQ();   // clean path: no speaker, no tone stack
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
        Amp a; a.init(SR); a.flipCabinet(); a.flipEQ(); a.flipLoop();
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
        {   Gate gt; gt.init(SR2); gt.threshold = 0.00316f;   // -50 dBFS
            float pk=0;
            for (int i=0;i<SR2;i++){ float y=gt.applyGate(0.0001f*sinf(i*0.1f)); if(fabs(y)>pk) pk=fabs(y); }
            check(pk < 0.0001f*0.05f, "gate crushes hiss below the threshold");
        }
        // b) a real note must pass essentially untouched once open
        {   Gate gt; gt.init(SR2); gt.threshold = 0.00316f;
            float pk=0;
            for (int i=0;i<SR2;i++){ float y=gt.applyGate(0.2f*sinf(2.0f*3.14159265f*220.0f*i/SR2));
                                     if (i>SR2/10 && fabs(y)>pk) pk=fabs(y); }
            check(pk > 0.19f, "gate passes a real note at full level");
        }
        // c) the attack must survive - gate must be open within a few ms of a note starting
        {   Gate gt; gt.init(SR2); gt.threshold = 0.00316f;
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
        {   Gate gt; gt.init(SR2); gt.threshold = 0.01f;
            float prev=1; int flips=0;
            for (int i=0;i<SR2;i++){
                float amp = 0.2f*expf(-3.0f*i/SR2);                 // decaying note
                gt.applyGate(amp*sinf(2.0f*3.14159265f*220.0f*i/SR2));
                if (i>100){ float now = gt.g>0.5f?1.0f:0.0f; if(now!=prev) flips++; prev=now; }
            }
            check(flips <= 1, "gate does not chatter as a note decays through the threshold");
        }
    }

    printf("\n%s\n", fails? "SOME TESTS FAILED" : "all tests passed");
    return fails;
}
