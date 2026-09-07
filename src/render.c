/*
 * songviz-render — audio spectrum visualizer renderer
 *
 * Reads raw PCM (f32le mono) from a file or stdin, renders a glowing
 * bar-spectrum video, and writes raw RGB24 frames to stdout, ready to be
 * piped into ffmpeg:
 *
 *   ffmpeg -i song.mp3 -f f32le -ac 1 -ar 44100 - \
 *     | songviz-render --title "Song" --fps 30 > frames.rgb
 *
 * Build (macOS):
 *   clang -O2 -framework Accelerate -framework CoreText \
 *     -framework CoreGraphics -framework CoreFoundation \
 *     src/render.c -o songviz-render
 *
 * Build (Linux, with -DSONGVIZ_NO_CORETEXT to disable titles):
 *   clang -O2 -DSONGVIZ_NO_CORETEXT -DUSE_OPENMP -fopenmp \
 *     src/render.c -o songviz-render -lfftw3f -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>

#if defined(__APPLE__)
#  include <Accelerate/Accelerate.h>
#  define SONGVIZ_HAS_VDSP 1
#else
#  include <complex.h>
#  include <fftw3.h>
#endif

#if !defined(SONGVIZ_NO_CORETEXT) && defined(__APPLE__)
#  include <CoreText/CoreText.h>
#  include <CoreGraphics/CoreGraphics.h>
#  include <CoreFoundation/CoreFoundation.h>
#  define SONGVIZ_HAS_CORETEXT 1
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AUDIO_SR 44100
#define MAX_BARS 256

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int   width;
    int   height;
    int   fps;
    int   bars;
    float gain;
    float fmin;
    float fmax;
    const char *palette;
    const char *title;
    const char *subtitle;
    float title_size;      /* 0 = auto */
    float subtitle_size;   /* 0 = auto */
    double max_seconds;    /* <=0 means full length */
} opts_t;

static opts_t opt = {
    .width = 1920, .height = 1080, .fps = 30, .bars = 64,
    .gain = 1.0f, .fmin = 30.0f, .fmax = 14000.0f,
    .palette = "neon", .title = NULL, .subtitle = NULL,
    .title_size = 0, .subtitle_size = 0, .max_seconds = 0,
};

/* ------------------------------------------------------------------ */
/* Palettes                                                            */
/* ------------------------------------------------------------------ */

typedef struct { float r, g, b; } rgb_t;

typedef struct {
    const char *name;
    int n;
    rgb_t stops[8];
} palette_t;

static const palette_t PALETTES[] = {
    { "neon", 3, { {0.95f,0.10f,0.75f}, {1.00f,0.65f,0.60f}, {0.45f,1.00f,1.00f} } },
    { "sunset", 3, { {1.00f,0.85f,0.30f}, {1.00f,0.45f,0.25f}, {0.55f,0.20f,0.75f} } },
    { "ocean", 3, { {0.10f,0.95f,0.85f}, {0.15f,0.45f,1.00f}, {0.45f,0.15f,0.90f} } },
    { "mono", 2, { {0.95f,0.95f,0.95f}, {0.35f,0.35f,0.35f} } },
    { "candy", 3, { {1.00f,0.40f,0.70f}, {0.65f,0.85f,1.00f}, {1.00f,1.00f,1.00f} } },
    { "matrix", 2, { {0.20f,1.00f,0.30f}, {0.00f,0.35f,0.10f} } },
};

static const rgb_t *palette_stops(int *n_stops) {
    for (size_t i = 0; i < sizeof(PALETTES)/sizeof(PALETTES[0]); i++) {
        if (strcmp(PALETTES[i].name, opt.palette) == 0) {
            *n_stops = PALETTES[i].n;
            return PALETTES[i].stops;
        }
    }
    *n_stops = PALETTES[0].n;
    return PALETTES[0].stops;
}

/* Per-bar colors, computed once from the palette gradient. */
static rgb_t bar_col[MAX_BARS];

static void init_bar_colors(void) {
    int n;
    const rgb_t *st = palette_stops(&n);
    for (int i = 0; i < opt.bars; i++) {
        float t = (opt.bars > 1) ? (float)i / (opt.bars - 1) : 0.0f;
        float seg = t * (n - 1);
        int s = (int)seg; if (s >= n - 1) s = n - 2;
        float u = seg - s;
        bar_col[i].r = st[s].r + (st[s+1].r - st[s].r) * u;
        bar_col[i].g = st[s].g + (st[s+1].g - st[s].g) * u;
        bar_col[i].b = st[s].b + (st[s+1].b - st[s].b) * u;
    }
}

/* ------------------------------------------------------------------ */
/* Audio + FFT state                                                   */
/* ------------------------------------------------------------------ */

static float *audio = NULL;      /* mono f32 */
static long   n_samples = 0;
static long   samples_per_frame = AUDIO_SR / 30;

#if defined(SONGVIZ_HAS_VDSP)
static FFTSetup        fft_setup;
static DSPSplitComplex split;
static float          *fft_re, *fft_im;
static vDSP_Length     fft_log2n;
#else
static fftwf_plan      fft_plan;
static float          *fft_in, *fft_out;
#endif
static float hann[4096];
static float window_gain = 1.0f;
static int   n_fft = 4096;

static int   bin_lo[MAX_BARS], bin_hi[MAX_BARS];
static float bar_level[MAX_BARS];
static float bar_peak[MAX_BARS];
static float agc = 1.0f;

static void load_audio_file(const char *path) {
    FILE *p = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (!p) { fprintf(stderr, "songviz: cannot open PCM file '%s'\n", path); exit(1); }
    long cap = 1 << 22;
    audio = malloc((size_t)cap * sizeof(float));
    while (1) {
        if (n_samples == cap) {
            cap *= 2;
            audio = realloc(audio, (size_t)cap * sizeof(float));
            if (!audio) { fprintf(stderr, "songviz: out of memory\n"); exit(1); }
        }
        size_t got = fread(audio + n_samples, sizeof(float), (size_t)(cap - n_samples), p);
        n_samples += (long)got;
        if (got == 0) break;
    }
    if (p != stdin) fclose(p);
    if (n_samples < n_fft) {
        fprintf(stderr, "songviz: PCM too short (%ld samples)\n", n_samples);
        exit(1);
    }
    fprintf(stderr, "songviz: loaded %.2f s of audio\n", n_samples / (double)AUDIO_SR);
}

static void init_fft(void) {
    fft_log2n = (vDSP_Length)log2((double)n_fft);
#if defined(SONGVIZ_HAS_VDSP)
    fft_setup = vDSP_create_fftsetup(fft_log2n, kFFTRadix2);
    fft_re = malloc((size_t)n_fft * sizeof(float));
    fft_im = malloc((size_t)n_fft * sizeof(float));
    split.realp = fft_re;
    split.imagp = fft_im;
#else
    fft_in  = fftwf_malloc((size_t)n_fft * sizeof(float));
    fft_out = fftwf_malloc((size_t)n_fft * sizeof(float));
    fft_plan = fftwf_plan_dft_r2c_1d(n_fft, fft_in, (fftwf_complex*)fft_out, FFTW_MEASURE);
#endif
    double sum = 0;
    for (int i = 0; i < n_fft; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (n_fft - 1));
        hann[i] = (float)w;
        sum += w;
    }
    window_gain = (float)(2.0 / sum);
}

static void init_bins(void) {
    float bin_hz = (float)AUDIO_SR / (float)n_fft;
    for (int i = 0; i <= opt.bars; i++) {
        float f = opt.fmin * powf(opt.fmax / opt.fmin, (float)i / opt.bars);
        int b = (int)(f / bin_hz);
        if (b < 1) b = 1;
        if (b > n_fft / 2 - 1) b = n_fft / 2 - 1;
        if (i < opt.bars) bin_lo[i] = b; else bin_hi[opt.bars - 1] = b;
        if (i > 0) bin_hi[i - 1] = b;
    }
}

static float analyze_frame(long frame) {
    long start = frame * samples_per_frame;
#if defined(SONGVIZ_HAS_VDSP)
    for (int i = 0; i < n_fft; i++) {
        long idx = start + i;
        float s = (idx < n_samples) ? audio[idx] : 0.0f;
        fft_re[i] = s * hann[i];
        fft_im[i] = 0.0f;
    }
    vDSP_fft_zrip(fft_setup, &split, 1, fft_log2n, FFT_FORWARD);
    float *mag_re = fft_re, *mag_im = fft_im;
#else
    for (int i = 0; i < n_fft; i++) {
        long idx = start + i;
        fft_in[i] = (idx < n_samples) ? audio[idx] * hann[i] : 0.0f;
    }
    fftwf_execute(fft_plan);
    float *mag_re = fft_out, *mag_im = fft_out + n_fft/2 + 1;
#endif
    int half = n_fft / 2;

    float raw[MAX_BARS];
    float total = 0;
    for (int b = 0; b < opt.bars; b++) {
        double acc = 0;
        for (int k = bin_lo[b]; k <= bin_hi[b] && k < half; k++) {
            double re = mag_re[k], im = mag_im[k];
            acc += sqrt(re * re + im * im);
        }
        int cnt = bin_hi[b] - bin_lo[b] + 1;
        raw[b] = (float)(acc / cnt) * window_gain;

        /* perceptual weighting: tame subs, soften highs */
        float w = 1.0f;
        float pos = (float)b / opt.bars;
        if (pos < 0.10f) w = 0.6f + 4.0f * pos;
        if (pos > 0.75f) w = 0.75f;
        raw[b] *= w * opt.gain;
        total += raw[b];
    }

    /* slow AGC keeps quiet songs visible and loud songs from clipping */
    float target = 0.55f;
    float scale = (total > 0.0001f) ? (target * opt.bars / total) : 0.0f;
    if (scale > 60.0f) scale = 60.0f;
    agc = agc * 0.97f + scale * 0.03f;

    float energy = 0;
    for (int b = 0; b < opt.bars; b++) {
        float v = raw[b] * agc;
        if (v > 1.6f) v = 1.6f;
        float prev = bar_level[b];
        float nv = (v > prev) ? prev + (v - prev) * 0.55f
                              : prev + (v - prev) * 0.18f;
        bar_level[b] = nv;
        if (nv > bar_peak[b]) bar_peak[b] = nv;
        else bar_peak[b] -= 0.012f;
        if (bar_peak[b] < nv) bar_peak[b] = nv;
        energy += nv;
    }
    return energy / opt.bars;
}

/* ------------------------------------------------------------------ */
/* Title rendering (CoreText, macOS only)                              */
/* ------------------------------------------------------------------ */

#if defined(SONGVIZ_HAS_CORETEXT)
static uint8_t *text_canvas = NULL; /* W*H*4 premultiplied RGBA, static */

static void draw_line(CGContextRef ctx, const char *txt, const char *fname,
                      float size, float r, float g, float b,
                      float cx, float y_top, float alpha) {
    CFStringRef str = CFStringCreateWithCString(NULL, txt, kCFStringEncodingUTF8);
    CFStringRef fn   = CFStringCreateWithCString(NULL, fname, kCFStringEncodingUTF8);
    CTFontRef font   = CTFontCreateWithName(fn, size, NULL);
    CGColorRef color = CGColorCreateSRGB(r, g, b, alpha);

    CFStringRef keys[2] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
    CFTypeRef vals[2]   = { font, color };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void**)keys, (const void**)vals, 2,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);

    double width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CGFloat ascent = 0, descent = 0;
    CTLineGetTypographicBounds(line, &ascent, &descent, NULL);

    CGContextSetTextPosition(ctx, cx - width / 2.0, y_top - descent);
    CTLineDraw(line, ctx);

    CFRelease(as); CFRelease(attrs); CFRelease(color); CFRelease(font);
    CFRelease(fn); CFRelease(str); CFRelease(line);
}

static void init_text(void) {
    if (!opt.title && !opt.subtitle) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    text_canvas = calloc((size_t)opt.width * opt.height * 4, 1);
    CGContextRef ctx = CGBitmapContextCreate(text_canvas, opt.width, opt.height, 8,
                                             (size_t)opt.width * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    float cx = opt.width / 2.0f;
    float H  = (float)opt.height;

    /* auto-size from frame width; long titles shrink to fit */
    float ts  = opt.title_size;
    float sts = opt.subtitle_size;
    if (ts == 0)  ts  = H * 0.089f;   /* 96 @1080p */
    if (sts == 0) sts = H * 0.041f;   /* 44 @1080p */
    if (opt.title) {
        float fit = (float)opt.width * 0.90f;
        /* crude width estimate: ~0.62em average glyph advance */
        float est = (float)strlen(opt.title) * ts * 0.62f;
        if (est > fit) ts *= fit / est;
    }

    float y = H * 0.845f;             /* baseline anchor for title, from top */
    if (opt.title)    { draw_line(ctx, opt.title, "Arial Rounded MT Bold", ts,
                                  1, 1, 1, cx, y, 1.0f);
                        y -= ts * 1.35f; }
    if (opt.subtitle) { draw_line(ctx, opt.subtitle, "Arial Rounded MT Bold", sts,
                                  0.91f, 0.61f, 0.91f, cx, y, 1.0f); }

    CFRelease(ctx); CFRelease(cs);
}

static void composite_text(uint8_t *frame, float alpha) {
    if (!text_canvas || alpha <= 0.001f) return;
    size_t n = (size_t)opt.width * opt.height;
    for (size_t i = 0; i < n; i++) {
        uint8_t a = text_canvas[i * 4 + 3];
        if (!a) continue;
        uint8_t *p = frame + i * 3;
        float af = (a / 255.0f) * alpha;
        int r = p[0] + (int)(text_canvas[i*4+0] * af);
        int g = p[1] + (int)(text_canvas[i*4+1] * af);
        int b = p[2] + (int)(text_canvas[i*4+2] * af);
        p[0] = (uint8_t)(r > 255 ? 255 : r);
        p[1] = (uint8_t)(g > 255 ? 255 : g);
        p[2] = (uint8_t)(b > 255 ? 255 : b);
    }
}
#else
static void init_text(void) {}
static void composite_text(uint8_t *frame, float alpha) {
    (void)frame; (void)alpha;
}
#endif

/* ------------------------------------------------------------------ */
/* Frame drawing                                                       */
/* ------------------------------------------------------------------ */

static inline void put_px(uint8_t *buf, int w, int h, int x, int y,
                          float r, float g, float b) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    uint8_t *p = buf + ((size_t)y * w + x) * 3;
    int nr = p[0] + (int)(r * 255);
    int ng = p[1] + (int)(g * 255);
    int nb = p[2] + (int)(b * 255);
    p[0] = (uint8_t)(nr > 255 ? 255 : nr);
    p[1] = (uint8_t)(ng > 255 ? 255 : ng);
    p[2] = (uint8_t)(nb > 255 ? 255 : nb);
}

static volatile sig_atomic_t got_sigpipe = 0;
static void on_sigpipe(int s) { (void)s; got_sigpipe = 1; }

int main(int argc, char **argv) {
    signal(SIGPIPE, on_sigpipe);

    /* ---- args ---- */
    const char *pcm_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (fprintf(stderr, "songviz: %s needs a value\n", a), exit(2), (const char*)NULL))
        if      (!strcmp(a, "--width"))     opt.width  = atoi(NEXT());
        else if (!strcmp(a, "--height"))    opt.height = atoi(NEXT());
        else if (!strcmp(a, "--fps"))       opt.fps    = atoi(NEXT());
        else if (!strcmp(a, "--bars"))      opt.bars   = atoi(NEXT());
        else if (!strcmp(a, "--gain"))      opt.gain   = (float)atof(NEXT());
        else if (!strcmp(a, "--fmin"))      opt.fmin   = (float)atof(NEXT());
        else if (!strcmp(a, "--fmax"))      opt.fmax   = (float)atof(NEXT());
        else if (!strcmp(a, "--palette"))   opt.palette= NEXT();
        else if (!strcmp(a, "--title"))     opt.title  = NEXT();
        else if (!strcmp(a, "--subtitle"))  opt.subtitle = NEXT();
        else if (!strcmp(a, "--title-size"))   opt.title_size    = (float)atof(NEXT());
        else if (!strcmp(a, "--subtitle-size"))opt.subtitle_size = (float)atof(NEXT());
        else if (!strcmp(a, "--seconds"))   opt.max_seconds = atof(NEXT());
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf(
"songviz-render — render a bar-spectrum music visualizer to raw RGB frames\n"
"\n"
"Usage: ffmpeg -i song.mp3 -f f32le -ac 1 -ar 44100 - \\\n"
"       | songviz-render [options] [-] > frames.rgb\n"
"\n"
"Options:\n"
"  --width N --height N   frame size           (default 1920x1080)\n"
"  --fps N                frames per second    (default 30)\n"
"  --bars N               number of bars, 8..%d (default 64)\n"
"  --palette NAME         neon|sunset|ocean|mono|candy|matrix\n"
"  --title TEXT           title text (macOS: rendered with CoreText)\n"
"  --subtitle TEXT        subtitle text\n"
"  --title-size N         override auto title size\n"
"  --subtitle-size N      override auto subtitle size\n"
"  --gain X               bar sensitivity multiplier (default 1.0)\n"
"  --fmin HZ --fmax HZ    spectrum range      (default 30..14000)\n"
"  --seconds S            limit render length (default: full input)\n"
"  -                      read PCM from stdin instead of a file\n",
                MAX_BARS);
            return 0;
        }
        else if (a[0] == '-' && a[1] != 0 && strcmp(a, "-") != 0) {
            fprintf(stderr, "songviz: unknown option '%s' (see --help)\n", a);
            return 2;
        }
        else pcm_path = a; /* "-" or a filename */
    }
    if (!pcm_path) pcm_path = "-";
    if (opt.bars < 8)    opt.bars = 8;
    if (opt.bars > MAX_BARS) opt.bars = MAX_BARS;
    if (opt.width < 64 || opt.height < 64 || opt.fps < 1 || opt.fps > 120) {
        fprintf(stderr, "songviz: invalid width/height/fps\n");
        return 2;
    }

    /* n_fft scales with fps so low-frequency bins stay meaningful */
    n_fft = 4096;
    while (n_fft > 1024 && AUDIO_SR / n_fft < opt.fps * 2) n_fft /= 2;

    samples_per_frame = AUDIO_SR / opt.fps;
    load_audio_file(pcm_path);
    init_fft();
    init_bins();
    init_bar_colors();
    init_text();

    long total_frames = n_samples / samples_per_frame;
    if (opt.max_seconds > 0) {
        long cap = (long)(opt.max_seconds * opt.fps);
        if (cap < total_frames) total_frames = cap;
    }
    fprintf(stderr, "songviz: rendering %ld frames (%dx%d @ %dfps, %d bars, %s)\n",
            total_frames, opt.width, opt.height, opt.fps, opt.bars, opt.palette);

    size_t frame_bytes = (size_t)opt.width * opt.height * 3;
    uint8_t *buf = malloc(frame_bytes);
    if (!buf) { fprintf(stderr, "songviz: out of memory\n"); return 1; }

    float W = (float)opt.width, H = (float)opt.height;
    float bar_area_w = W * 0.792f;             /* 1520 @1920 */
    float bar_gap    = fmaxf(3.0f, W * 0.0031f);
    float bar_w      = (bar_area_w - (opt.bars - 1) * bar_gap) / opt.bars;
    float x0         = (W - bar_area_w) / 2;
    float y_base     = H * 0.889f;             /* 960 @1080 */
    float max_h      = H * 0.519f;             /* 560 @1080 */
    float refl_frac  = 0.28f;
    float glow_h     = H * 0.075f;             /* tip glow height */
    int   y_base_i   = (int)y_base;

    for (long f = 0; f < total_frames; f++) {
        if (got_sigpipe) return 0;
        float energy = analyze_frame(f);
        float t = (float)f / opt.fps;

        /* background gradient with slow pulse */
        float pulse = 0.5f + 0.5f * sinf(t * 0.8f);
        for (int y = 0; y < opt.height; y++) {
            float v = (float)y / opt.height;
            float r = 0.04f + 0.05f * (1.0f - v) + 0.02f * pulse;
            float g = 0.02f + 0.03f * (1.0f - v);
            float b = 0.08f + 0.09f * (1.0f - v) + 0.03f * (1.0f - pulse);
            uint8_t *row = buf + (size_t)y * opt.width * 3;
            uint8_t rr = (uint8_t)(r * 255), gg = (uint8_t)(g * 255), bb = (uint8_t)(b * 255);
            for (int x = 0; x < opt.width; x++) {
                row[x*3+0] = rr; row[x*3+1] = gg; row[x*3+2] = bb;
            }
        }

        /* bass glow */
        float bass = 0;
        int nb = opt.bars < 8 ? opt.bars : 8;
        for (int b = 0; b < nb; b++) bass += bar_level[b];
        bass /= nb;
        int glow_cy = opt.height - (int)(H * 0.13f);
        float gr = (W * 0.198f) + (W * 0.125f) * bass;
        for (int y = glow_cy - (int)gr; y < glow_cy + (int)gr; y += 2) {
            for (int x = (int)(W/2 - gr); x < (int)(W/2 + gr); x += 2) {
                float dx = (x - W/2) / gr;
                float dy = (y - glow_cy) / gr;
                float d = sqrtf(dx*dx + dy*dy);
                if (d < 1.0f) {
                    float a = (1-d)*(1-d) * (0.20f + 0.25f*bass);
                    float ar = a*0.9f, ag = a*0.15f, ab = a*0.9f;
                    put_px(buf, opt.width, opt.height, x,   y,   ar, ag, ab);
                    put_px(buf, opt.width, opt.height, x+1, y,   ar, ag, ab);
                    put_px(buf, opt.width, opt.height, x,   y+1, ar, ag, ab);
                    put_px(buf, opt.width, opt.height, x+1, y+1, ar, ag, ab);
                }
            }
        }

        /* bars */
        for (int bi = 0; bi < opt.bars; bi++) {
            float lvl = bar_level[bi];
            float h = fmaxf(lvl, 0.012f) * max_h;
            float bx = x0 + bi * (bar_w + bar_gap);
            float cr = bar_col[bi].r, cg = bar_col[bi].g, cb = bar_col[bi].b;
            int bxi = (int)bx, bxe = (int)(bx + bar_w);

            int top = (int)(y_base - h);
            for (int y = top; y < y_base_i; y++) {
                float u = (y_base - y) / h;
                float fade = 0.45f + 0.55f * u;
                for (int x = bxi; x < bxe; x++)
                    put_px(buf, opt.width, opt.height, x, y, cr*fade, cg*fade, cb*fade);
            }

            int gy0 = top - (int)glow_h;
            for (int y = gy0; y < top; y++) {
                float d = (top - y) / glow_h;
                float a = (1-d)*(1-d) * 0.5f * fminf(lvl + 0.15f, 1.0f);
                for (int x = bxi; x < bxe; x++)
                    put_px(buf, opt.width, opt.height, x, y, cr*a, cg*a, cb*a);
            }

            int refl_h = (int)(h * refl_frac);
            for (int y = 0; y < refl_h; y++) {
                float d = (float)y / refl_h;
                float a = (1-d)*(1-d) * 0.22f;
                for (int x = bxi; x < bxe; x++)
                    put_px(buf, opt.width, opt.height, x, y_base_i + 18 + y, cr*a, cg*a, cb*a);
            }

            float ph = fmaxf(bar_peak[bi], 0.02f) * max_h;
            for (int y = (int)(y_base - ph) - 4; y < (int)(y_base - ph); y++)
                for (int x = bxi; x < bxe; x++)
                    put_px(buf, opt.width, opt.height, x, y, 1, 1, 1);
        }

        /* title fade in/out */
        float dur = (float)total_frames / opt.fps;
        float talpha = fminf(1.0f, t / 1.2f);
        if (t > dur - 1.5f) talpha = fminf(talpha, (dur - t) / 1.5f);
        composite_text(buf, talpha);

        /* baseline */
        {
            int xs = (int)x0 - 20, xe = (int)(x0 + bar_area_w + 20);
            float edge_in  = 120.0f, edge_out = 120.0f;
            for (int x = xs; x < xe; x++) {
                float e1 = (x - xs) / edge_in;
                float e2 = (xe - x) / edge_out;
                float edge = fminf(1.0f, fminf(e1, e2));
                float a = 0.35f * edge + 0.15f * energy;
                put_px(buf, opt.width, opt.height, x, y_base_i + 6, a, a*0.5f, a);
                put_px(buf, opt.width, opt.height, x, y_base_i + 7, a, a*0.5f, a);
            }
        }

        if (fwrite(buf, 1, frame_bytes, stdout) != frame_bytes) return 0;
        if (opt.fps * 10 > 0 && f % (opt.fps * 10) == 0)
            fprintf(stderr, "songviz: frame %ld / %ld (%.0f%%)\n",
                    f, total_frames, 100.0 * f / total_frames);
    }

    fprintf(stderr, "songviz: done\n");
    return 0;
}
