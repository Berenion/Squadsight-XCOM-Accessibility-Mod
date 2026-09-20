// The field mixer.  See audio.h.

#include "audio.h"
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RATE        44100
#define CHANNELS    2
// About 12 ms a buffer, four of them. Short enough that the field answers a
// step as it is taken; long enough that a stall on the game's thread -- a
// level streaming in, a save being written -- does not starve the device into
// a click.
#define FRAMES      512
#define BUFFERS     4

// Above this the output is bent rather than cut. See mix().
#define LIMIT_KNEE 0.8f

#define PI_F 3.14159265f
#define TWO_PI_F 6.28318531f

// ---- the bands -------------------------------------------------------------
//
// One resonant noise band per direction. Q is what makes it a band rather than
// a hiss or a tone: too low and the four are indistinguishable, too high and
// the filter rings and the thing becomes the sine drone this replaced. North is
// the loosest -- a narrow band up at 920 Hz is a whistle, and a whistle in the
// ear for an hour is not something anyone will keep switched on. South is the
// tightest, because a low hum needs definition to be heard as a pitch at all.
//
// ---- keeping the four apart ------------------------------------------------
//
// Pitch alone does not do it. The first version panned east and west to 0.92
// and left north and south both dead centre, and the report was the obvious
// one: they blend. Of course they do -- two of the four were in the same place,
// told apart only by timbre, and the other two were not far enough out to be
// clearly elsewhere. Four sounds at once need four *positions*, and stereo has
// only one axis to spend. So each direction is given a different kind of image
// rather than a different point along that axis:
//
//   east / west   a point, hard over to that side
//   north         wide: independent noise in each ear, so it is everywhere
//                 at once and nowhere in particular -- a wash, not a source
//   south         a point, dead centre, and the only one there
//
// Decorrelated noise and mono noise are not two positions on a line, they are
// two different things, and the ear separates them instantly. That is what
// gives north and south their own places without either of them moving.
//
// The sides are panned nearly the whole way out. That leaves the far ear at
// about -30 dB, which is all but nothing, so the two cues below barely apply
// any more -- but they cost little, and they are what makes the sides bearable
// rather than merely separated if SIDE_PAN is ever pulled back.
#define SIDE_PAN 0.97f

// The head's own width in sound: about 22 cm at 343 m/s. Unlike the struck
// version this has to be a real delay, because noise has no phase to shift.
#define SIDE_ITD 0.00066f

// And the far ear hears a duller sound, because a head is in the way. One pole
// is the whole of it; at the sides' own pitch it is worth a decibel or two,
// which is about what a real head does down there.
#define SHADOW_HZ 900.0f

// ---- getting heard over the game -------------------------------------------
//
// A steady noise band is the single hardest thing to hear through broadband
// noise, and broadband noise is what weather is. The first version was reported
// lost under rainfall, which is exactly the case the design walked into: the
// masker and the signal were the same kind of sound, so the only thing that
// separated them was level, and no level that is comfortable in a quiet map
// survives a loud one.
//
// Turning it up is not the answer, or not the whole answer. What the ear has
// that a level meter does not is a separate sense for the *envelope* of a
// sound -- how it swells and falls -- and it is most sensitive to that around
// four or five times a second. Rain has no envelope: it is stationary, its
// fluctuations are fast and random and average out. So each band is given a
// slow, regular pulse, and that pulse is what carries it through. It is heard
// as a thing breathing behind the weather rather than as part of it, and it
// costs no loudness at all.
//
// The rate goes with the pitch -- the low band slow and heavy, the bright one
// quick and light -- which makes it a second way of telling the four apart on
// top of pitch and position. East and west share a rate as they share a pitch,
// because they are one thing mirrored. And every rate is well above anything
// the player's own movement can produce: a glide is a tenth of a second of
// smooth change, never a regular flutter, so a pulse can never be mistaken for
// a wall approaching.

typedef struct {
    float centre;   // Hz
    float q;
    float pan;      // -1 hard left .. +1 hard right
    float itd;      // seconds the far ear lags; the near ear is the panned side
    int   wide;     // independent noise per ear: a wash rather than a source
    float pulse;    // Hz, the tremolo that carries it through weather
} BandSpec;

static const BandSpec BAND[SONAR_DIRS] = {
    /* west  */ { 470.0f, 4.5f, -SIDE_PAN, SIDE_ITD, 0, 4.7f },
    /* north */ { 920.0f, 2.4f,  0.0f,     0.0f,     1, 6.9f },
    /* south */ { 210.0f, 7.0f,  0.0f,     0.0f,     0, 3.1f },
    /* east  */ { 470.0f, 4.5f,  SIDE_PAN, SIDE_ITD, 0, 4.7f },
};

// How deep the pulse goes: the quiet part of the cycle is this much down from
// the loud part. Just over half, which is seven decibels -- plainly a pulse, and
// not so deep that the band disappears between beats and has to be waited for.
#define PULSE_DEPTH 0.55f

// Pulsing costs average power, and average power is the level that says how far
// away the wall is, so it has to be given back. The mean square of
// (1 - d*u) over a raised-cosine u, whose mean is 1/2 and mean square 3/8, is
// 1 - d + 3d^2/8; the amplitude correction is one over the root of that. Which
// puts the loud part of the cycle *above* where the steady band sat -- 1.33
// times, at this depth -- and that is the part that does the hearing.
#define PULSE_POWER (1.0f - PULSE_DEPTH + 0.375f * PULSE_DEPTH * PULSE_DEPTH)

// Where in its cycle each band starts. A quarter turn apart, so that four bands
// do not beat together and read as one thing pulsing.
static const float PULSE_PHASE[SONAR_DIRS] = {
    0.0f, 1.5707963f, 3.1415927f, 4.712389f
};

// ---- the player's own level -------------------------------------------------
//
// Even with the pulse there is no one right loudness: a rainstorm and a quiet
// interior are a long way apart, and headphones and laptop speakers further
// still. So the field's level is adjustable, five notches three decibels apart
// around the built-in one, set from the practice menu (learn.h) where it can be
// heard against whatever the game is playing. It is not saved anywhere -- the
// mod has no settings file yet -- so it starts at the middle every run.
//
// At the top notch a position walled in on all four sides puts noise crests over
// the limiter's knee. That is left alone deliberately: what the knee rounds off
// there is the tips of a noise waveform, a few samples at a time, while the rms
// -- which is what says how far the wall is -- stays far below it. Rounding a
// crest is not the same fault as flattening a level, and the player who chose
// the loudest setting wants the loudness more than the last decibel of crest.
#define VOLUME_NOTCHES 5
#define VOLUME_MIDDLE  2
static const float VOLUME_STEP[VOLUME_NOTCHES] = {
    0.50f, 0.71f, 1.00f, 1.41f, 2.00f
};

// The whole field's level, and the first knob to turn if it wears on the ear
// or cannot be heard. It is far below what the struck chord ran at, and has
// to be: a sound that fires on a key press may be as loud as the news it
// carries, and a sound that never stops may not.
//
// Set so that the fullest field the game can produce -- four directions all
// walled in at once -- lands around -20 dBFS rms, which is a presence under
// speech rather than something to talk over, and so that the level a player
// actually spends most of their time at, a wall or two a couple of tiles off,
// is still clearly there. Most levels are well under full: a wall two tiles
// away is a quarter of one against the tile, so tuning this against the boxed-in
// case alone leaves the ordinary case inaudible.
#define FIELD_VOLUME 0.055f

// How long a level takes to glide most of the way to a new one. This is what
// makes the field continuous: the cursor arrives at tiles in steps, and a
// tenth of a second of glide is enough to hear the change as an approach and
// short enough that it is not lagging behind the player.
#define GLIDE_SECONDS 0.10f

// The reference the loudness trims are taken against: the sides' own pitch,
// so east and west are never trimmed at all and the other two are matched to
// them.
#define TRIM_REF_HZ 470.0f
// A-weighting is the right shape but it is measured loud, and the ear's
// curves are steeper than it down in the bass at the level this runs at --
// so the low band will still sound a shade quieter than the rest. Correcting
// further would cost more headroom than the difference is worth, and this cap
// is what stops a very low band from eating all of it.
#define TRIM_MAX 3.0f

// A field is news about this moment, so it has to be renewed to stay. When
// nothing has renewed it for this long the mod has stopped looking -- the
// mission ended and the cursor with it, or whatever drives the field is no
// longer being called -- and walls left sounding would be describing a map
// that is gone. So it lapses on its own instead of waiting to be switched off
// by code that may never run.
#define FIELD_STALE_MS 250

// ---- a state-variable filter ------------------------------------------------
//
// Zavalishin's topology-preserving two-pole, band output. Coefficients can be
// replaced while it is running without disturbing the state, which is what
// lets the centre frequency follow the low-cover share.
typedef struct {
    float a1, a2, a3;
    float ic1, ic2;
} Svf;

static void svf_set(Svf* s, float fc, float q)
{
    if (fc < 20.0f) fc = 20.0f;
    if (fc > RATE * 0.45f) fc = RATE * 0.45f;
    if (q < 0.5f) q = 0.5f;
    float g = tanf(PI_F * fc / (float)RATE);
    float k = 1.0f / q;
    s->a1 = 1.0f / (1.0f + g * (g + k));
    s->a2 = g * s->a1;
    s->a3 = g * s->a2;
}

static float svf_band(Svf* s, float in)
{
    float v3 = in - s->ic2;
    float v1 = s->a1 * s->ic1 + s->a2 * v3;
    float v2 = s->ic2 + s->a2 * s->ic1 + s->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    return v1;
}

// Enough randomness for a noise band, and cheap enough to run per sample.
// Each band carries its own, so two of them never share a sequence and
// phantom into one source in the middle of the head.
static unsigned xorshift(unsigned* r)
{
    *r ^= *r << 13;
    *r ^= *r >> 17;
    *r ^= *r << 5;
    return *r;
}

static float white(unsigned* r)
{
    return (float)(int)(xorshift(r) >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

// The far ear's delay, in samples. 64 covers a head's width over three times.
#define DELAY_LEN 64

typedef struct {
    // Two of each, but only the wide band uses both: one filter and one noise
    // source per ear, so that what the two ears hear is unrelated. Everything
    // else runs on the first and reaches the far ear through the delay and the
    // shadow below.
    Svf      f[2];
    unsigned rng[2];
    int      wide;
    float    shadow;        // the far ear's one-pole state
    float    shadow_a;      // its coefficient
    float    level;         // glided towards `target` every sample
    float    target;
    // Level and the pulse are the only things about a band that move. Its
    // pitch, its filter coefficients, its loudness correction and its place in
    // the stereo field are all settled once, at startup, and then left alone.
    float    amp;           // norm * trim * FIELD_VOLUME * the pulse correction
    float    pulse_phase;
    float    pulse_step;    // radians a sample
    float    gain_l, gain_r;
    int      far_left;      // the left ear is the lagging one (it sounds east)
    int      itd_samples;
    float    delay[DELAY_LEN];
    int      dpos;
} Band;

static HWAVEOUT         g_dev;
static HANDLE           g_event;
static HANDLE           g_thread;
static volatile LONG    g_quit;
static WAVEHDR          g_hdr[BUFFERS];
static short            g_buf[BUFFERS][FRAMES * CHANNELS];
static float            g_mix[FRAMES * CHANNELS];
static Band             g_band[SONAR_DIRS];
static CRITICAL_SECTION g_lock;
static volatile int     g_ready;
static float            g_glide;        // per-sample level coefficient
static int              g_stale;        // frames since the field was renewed
static int              g_notch = VOLUME_MIDDLE;
static float            g_volume = 1.0f;

// Constant power: a sound thrown hard to one side is as loud as the same
// sound in the middle. A linear pan dips through the centre, which would make
// north and south -- which sit there -- seem further off than east and west
// purely because of where they are placed.
static void pan_gains(float pan, float* l, float* r)
{
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    float t = (pan + 1.0f) * 0.5f * 1.57079633f;
    *l = cosf(t);
    *r = sinf(t);
}

// The A-weighting response at a frequency: how much of it the ear actually
// hears. Used only as a ratio, so its 1 kHz offset is left out.
static float a_weight(float f)
{
    float f2 = f * f;
    float num = 12194.0f * 12194.0f * f2 * f2;
    float den = (f2 + 20.6f * 20.6f) *
                sqrtf((f2 + 107.7f * 107.7f) * (f2 + 737.9f * 737.9f)) *
                (f2 + 12194.0f * 12194.0f);
    return den > 0.0f ? num / den : 0.0f;
}

// What a band must be scaled by to be as loud as one at the reference pitch.
// Loudness is distance in this field -- it is the whole message -- so a
// direction that merely sounds louder is a direction reported closer than it
// is, and matching the bands is not a nicety.
static float band_trim(float fc)
{
    float a = a_weight(fc);
    if (a <= 0.0f) return 1.0f;
    float t = a_weight(TRIM_REF_HZ) / a;
    if (t > TRIM_MAX) t = TRIM_MAX;
    if (t < 1.0f / TRIM_MAX) t = 1.0f / TRIM_MAX;
    return t;
}

// The rms a band actually reaches, measured by running it. Without this the
// bands arrive at the mixer at whatever level their Q happens to give -- a
// tight filter passes a fraction of the noise a loose one does -- and the
// loudness trim above would then be matching the wrong thing. Measured rather
// than written down, so changing a Q needs no second edit. Slow enough that it
// belongs at startup and nowhere else.
static float band_rms(float fc, float q)
{
    Svf s;
    memset(&s, 0, sizeof s);
    svf_set(&s, fc, q);
    unsigned r = 0x9E3779B9u;
    // Long enough that the estimate is steady to a fraction of a decibel,
    // after enough warm-up for the filter to have forgotten its zero state.
    for (int i = 0; i < 4096; i++) svf_band(&s, white(&r));
    double sum = 0.0;
    const int n = 1 << 16;
    for (int i = 0; i < n; i++) {
        float v = svf_band(&s, white(&r));
        sum += (double)v * v;
    }
    return (float)sqrt(sum / n);
}

// A band's filter, and the one gain that everything except its level folds
// into. Run once per band at startup, which is the only place it can be run:
// band_rms below renders a second of noise to measure, which is nothing at
// startup and out of the question on the mixer thread.
static void band_settle(Band* b, const BandSpec* spec)
{
    svf_set(&b->f[0], spec->centre, spec->q);
    svf_set(&b->f[1], spec->centre, spec->q);
    float rms = band_rms(spec->centre, spec->q);
    float norm = rms > 1e-9f ? 1.0f / rms : 0.0f;
    b->amp = norm * band_trim(spec->centre) * FIELD_VOLUME /
             sqrtf(PULSE_POWER);
    b->pulse_step = TWO_PI_F * spec->pulse / (float)RATE;
}

// One buffer's worth of every band, summed.
static void mix(short* out)
{
    memset(g_mix, 0, sizeof g_mix);

    EnterCriticalSection(&g_lock);
    g_stale += FRAMES;
    if (g_stale > FIELD_STALE_MS * RATE / 1000) {
        for (int d = 0; d < SONAR_DIRS; d++) g_band[d].target = 0.0f;
        g_stale = FIELD_STALE_MS * RATE / 1000 + 1;      // no need to run away
    }
    for (int d = 0; d < SONAR_DIRS; d++) {
        Band* b = &g_band[d];

        // A band at rest costs nothing. It has to be properly at rest, not
        // merely quiet: the filter state is left alone, so when it is asked
        // for again it picks up where it left off rather than from silence,
        // and there is no transient at the start.
        if (b->level < 1e-5f && b->target < 1e-5f) {
            b->level = 0.0f;
            continue;
        }

        float amp = b->amp * g_volume;
        for (int f = 0; f < FRAMES; f++) {
            b->level += (b->target - b->level) * g_glide;

            // The pulse. One multiplier for both ears, always: modulating them
            // separately would wobble the image from side to side, and the wide
            // band -- whose ears are already unrelated -- would come apart
            // altogether.
            float u = 0.5f - 0.5f * cosf(b->pulse_phase);
            b->pulse_phase += b->pulse_step;
            if (b->pulse_phase >= TWO_PI_F) b->pulse_phase -= TWO_PI_F;

            float v = b->level * amp * (1.0f - PULSE_DEPTH * u);
            float ls, rs;

            if (b->wide) {
                // Two noises, two filters, nothing shared: the ears are given
                // unrelated sound of the same colour, and the result has no
                // direction in it at all. That is the point -- it is the one
                // band that is not anywhere.
                ls = svf_band(&b->f[0], white(&b->rng[0]));
                rs = svf_band(&b->f[1], white(&b->rng[1]));
            } else {
                float s = svf_band(&b->f[0], white(&b->rng[0]));

                // The far ear hears the same band a little behind, and duller.
                b->delay[b->dpos] = s;
                int back = b->dpos - b->itd_samples;
                if (back < 0) back += DELAY_LEN;
                float far_s = b->itd_samples ? b->delay[back] : s;
                b->dpos = (b->dpos + 1) & (DELAY_LEN - 1);
                if (b->shadow_a > 0.0f) {
                    b->shadow += (far_s - b->shadow) * b->shadow_a;
                    far_s = b->shadow;
                }

                ls = b->far_left ? far_s : s;
                rs = b->far_left ? s : far_s;
            }

            g_mix[f * 2]     += ls * v * b->gain_l;
            g_mix[f * 2 + 1] += rs * v * b->gain_r;
        }
    }
    LeaveCriticalSection(&g_lock);

    // The one guard between the sum and the device. Below the knee it changes
    // nothing; above it the overshoot is folded into what headroom is left, so
    // four bands landing together round off instead of tearing. Clamping each
    // band on the way in would distort every one of them and still let their
    // sum clip.
    for (int i = 0; i < FRAMES * CHANNELS; i++) {
        float s = g_mix[i];
        float mag = s < 0.0f ? -s : s;
        if (mag > LIMIT_KNEE) {
            float soft = LIMIT_KNEE + (1.0f - LIMIT_KNEE) *
                         (float)tanh((mag - LIMIT_KNEE) / (1.0f - LIMIT_KNEE));
            s = s < 0.0f ? -soft : soft;
        }
        int q = (int)(s * 32767.0f);
        if (q >  32767) q =  32767;
        if (q < -32768) q = -32768;
        out[i] = (short)q;
    }
}

static DWORD WINAPI pump(LPVOID arg)
{
    (void)arg;
    // Nothing is in flight yet, so the first pass fills and writes all four.
    for (int b = 0; b < BUFFERS; b++) g_hdr[b].dwFlags |= WHDR_DONE;

    while (!g_quit) {
        int wrote = 0;
        for (int b = 0; b < BUFFERS; b++) {
            if (!(g_hdr[b].dwFlags & WHDR_DONE)) continue;
            mix(g_buf[b]);
            g_hdr[b].dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(g_dev, &g_hdr[b], sizeof g_hdr[b]) != MMSYSERR_NOERROR) {
                // The device has gone -- headphones unplugged, a driver
                // reset. Stop feeding it; the mod carries on without sound.
                g_ready = 0;
                return 0;
            }
            wrote = 1;
        }
        // The wait is bounded so that a completion event arriving while the
        // loop was elsewhere cannot park the thread: the worst case is one
        // pass of polling every 10 ms.
        if (!wrote) WaitForSingleObject(g_event, 10);
    }
    return 0;
}

static void bands_init(void)
{
    memset(g_band, 0, sizeof g_band);
    for (int d = 0; d < SONAR_DIRS; d++) {
        Band* b = &g_band[d];
        const BandSpec* spec = &BAND[d];
        // Seeded per direction and per ear, never zero, which xorshift cannot
        // leave. Two bands drawing the same sequence would phantom into one
        // source in the middle of the head, which is exactly what the wide
        // band exists to avoid.
        for (int e = 0; e < 2; e++) {
            b->rng[e] = 0x2545F491u ^ (unsigned)((d * 2 + e) * 2654435761u);
            if (!b->rng[e]) b->rng[e] = 1u;
        }
        b->wide = spec->wide;
        b->pulse_phase = PULSE_PHASE[d];
        b->itd_samples = (int)(spec->itd * RATE + 0.5f);
        if (b->itd_samples >= DELAY_LEN) b->itd_samples = DELAY_LEN - 1;
        b->far_left = spec->pan > 0.0f;
        b->shadow_a = spec->pan != 0.0f
                      ? 1.0f - expf(-2.0f * PI_F * SHADOW_HZ / RATE) : 0.0f;
        pan_gains(spec->pan, &b->gain_l, &b->gain_r);
        // The slow part of starting the device: four filters each run over a
        // second of noise to be measured.
        band_settle(b, spec);
    }
    g_glide = 1.0f - expf(-1.0f / (GLIDE_SECONDS * RATE));
}

int audio_start(char* why, size_t why_sz)
{
    if (why && why_sz) why[0] = 0;
    if (g_ready) {
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "already open");
        return 1;
    }

    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = CHANNELS;
    wf.nSamplesPerSec  = RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(CHANNELS * 2);
    wf.nAvgBytesPerSec = RATE * CHANNELS * 2;

    g_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_event) {
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "no event (0x%08lx)",
                             GetLastError());
        return 0;
    }

    MMRESULT mr = waveOutOpen(&g_dev, WAVE_MAPPER, &wf, (DWORD_PTR)g_event, 0,
                              CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR) {
        CloseHandle(g_event);
        g_event = NULL;
        if (why) _snprintf_s(why, why_sz, _TRUNCATE,
                             "no output device (waveOutOpen %u) -- field off",
                             (unsigned)mr);
        return 0;
    }

    InitializeCriticalSection(&g_lock);
    bands_init();
    for (int b = 0; b < BUFFERS; b++) {
        memset(&g_hdr[b], 0, sizeof g_hdr[b]);
        g_hdr[b].lpData         = (LPSTR)g_buf[b];
        g_hdr[b].dwBufferLength = sizeof g_buf[b];
        waveOutPrepareHeader(g_dev, &g_hdr[b], sizeof g_hdr[b]);
    }

    g_quit = 0;
    g_ready = 1;
    g_thread = CreateThread(NULL, 0, pump, NULL, 0, NULL);
    if (!g_thread) {
        DWORD err = GetLastError();
        g_ready = 0;
        for (int b = 0; b < BUFFERS; b++)
            waveOutUnprepareHeader(g_dev, &g_hdr[b], sizeof g_hdr[b]);
        waveOutClose(g_dev);
        DeleteCriticalSection(&g_lock);
        CloseHandle(g_event);
        g_dev = NULL;
        g_event = NULL;
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "no mixer thread (0x%08lx)", err);
        return 0;
    }

    if (why) _snprintf_s(why, why_sz, _TRUNCATE,
                         "%d Hz stereo, %d x %d frames (%.0f ms), %d bands at "
                         "%.0f/%.0f/%.0f/%.0f Hz pulsing at "
                         "%.1f/%.1f/%.1f/%.1f Hz, sides panned %.2f",
                         RATE, BUFFERS, FRAMES,
                         1000.0 * BUFFERS * FRAMES / RATE, SONAR_DIRS,
                         BAND[0].centre, BAND[1].centre, BAND[2].centre,
                         BAND[3].centre, BAND[0].pulse, BAND[1].pulse,
                         BAND[2].pulse, BAND[3].pulse, SIDE_PAN);
    return 1;
}

int audio_available(void) { return g_ready; }

void audio_field(const SonarField* f)
{
    if (!g_ready || !f) return;
    EnterCriticalSection(&g_lock);
    g_stale = 0;
    for (int d = 0; d < SONAR_DIRS; d++) {
        float l = f->level[d];
        if (l < 0.0f) l = 0.0f;
        if (l > 1.0f) l = 1.0f;
        g_band[d].target = l;
    }
    LeaveCriticalSection(&g_lock);
}

int audio_volume(void) { return g_notch; }

int audio_volume_set(int notch)
{
    if (notch < 0) notch = 0;
    if (notch >= VOLUME_NOTCHES) notch = VOLUME_NOTCHES - 1;
    g_notch = notch;
    // Read once per buffer by the mixer, so a torn read is not possible and no
    // lock is needed; a change lands on the next buffer, twelve milliseconds
    // off, which is as good as at once.
    g_volume = VOLUME_STEP[notch];
    return notch;
}

int audio_volume_notches(void) { return VOLUME_NOTCHES; }

void audio_field_off(void)
{
    if (!g_ready) return;
    EnterCriticalSection(&g_lock);
    // Faded, not cut: the levels glide to zero like any other change. A hard
    // stop on a band that is sounding is a step to silence, and a step is a
    // click loud enough to be the thing the player remembers.
    for (int d = 0; d < SONAR_DIRS; d++) g_band[d].target = 0.0f;
    LeaveCriticalSection(&g_lock);
}

void audio_stop(void)
{
    if (!g_ready) return;
    g_ready = 0;
    InterlockedExchange(&g_quit, 1);
    SetEvent(g_event);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;

    waveOutReset(g_dev);
    for (int b = 0; b < BUFFERS; b++)
        waveOutUnprepareHeader(g_dev, &g_hdr[b], sizeof g_hdr[b]);
    waveOutClose(g_dev);
    g_dev = NULL;
    DeleteCriticalSection(&g_lock);
    CloseHandle(g_event);
    g_event = NULL;
}
