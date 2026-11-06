/* The hardware-mixer backend against a model of the openfpgaOS mixer: every
 * sample the mixer plays must be the track's own decode, in order, across
 * ring wraps, stream refills, loops, pauses and starvation. */
#define OF_PC 1
#define OF_HW_MIXER_FAKE 1
#include "../src/openfpga/platform/of_audio_system.c"
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>

void logWarn(const char *fmt, ...) { (void) fmt; }
void logError(const char *fmt, ...) { (void) fmt; abort(); }
void logInfo(const char *fmt, ...) { (void) fmt; }
static uint64_t g_now = 1;
uint64_t nowNanos(void) { return g_now; }
bool ofPerfDetail = false;
static uint64_t loads[OF_LOAD_KINDS];
void ofPerfAddLoad(OfLoadKind kind, uint64_t value) { loads[kind] += value; }
void ofPerfPhase(OfPhase phase) { (void) phase; }
bool ofSavestateDeferFileReads(void) { return false; }
static int softwarePairs;
void of_audio_init(void) {}
void ofPcAudioSnapshotReset(void) {}
int of_audio_free(void) { return 4096; }
int of_audio_write(const int16_t *samples, int pairs) { (void) samples; softwarePairs += pairs; return pairs; }

/* ---- The mixer model: stream-mode voices, one output pair at a time. ---- */

typedef struct {
    bool active;
    const int16_t *ring;
    uint32_t len, wptr, rate;
    uint64_t pos; /* 16.16 */
    int volume;
    uint64_t handle;
    int16_t *heard;
    uint32_t heardCount, heardCap;
} FakeVoice;
static FakeVoice fake[32];
static bool fakeAvailable = true, fakeRefuse = false;
static uint64_t fakeGeneration;

bool hwPortAvailable(void) { return fakeAvailable; }
bool hwPortStart(OfHwVoice *hw, int16_t *ring, uint32_t samples, uint32_t wptr, uint32_t rate) {
    if (fakeRefuse) return false;
    for (int i = 0; i < 31; i++) {
        FakeVoice *f = &fake[i];
        if (f->active) continue;
        free(f->heard);
        memset(f, 0, sizeof(*f));
        f->active = true; f->ring = ring; f->len = samples; f->wptr = wptr; f->rate = rate;
        f->handle = (++fakeGeneration << 8) | (uint64_t) i;
        f->heardCap = 1024; f->heard = malloc(f->heardCap * sizeof(int16_t));
        f->heard[f->heardCount++] = ring[0];
        hw->voice = i; hw->handle = f->handle;
        return true;
    }
    return false;
}
static FakeVoice *fakeFor(const OfHwVoice *hw) {
    FakeVoice *f = &fake[hw->voice];
    return f->active && f->handle == hw->handle ? f : NULL;
}
void hwPortSetWptr(const OfHwVoice *hw, uint32_t wptr) { FakeVoice *f = fakeFor(hw); assert(f && wptr < f->len); f->wptr = wptr; }
int32_t hwPortPosition(const OfHwVoice *hw) { FakeVoice *f = fakeFor(hw); return f ? (int32_t) ((f->pos >> 16) % f->len) : -1; }
void hwPortSetRate(const OfHwVoice *hw, uint32_t rate) { FakeVoice *f = fakeFor(hw); assert(f); f->rate = rate; }
void hwPortSetVolume(const OfHwVoice *hw, int volume) { FakeVoice *f = fakeFor(hw); assert(f && volume >= 0 && volume <= 255); f->volume = volume; }
/* Stopped voices keep what they heard until the slot is reused. */
static FakeVoice stopped[64];
static int stoppedCount;
void hwPortStop(const OfHwVoice *hw) {
    FakeVoice *f = fakeFor(hw); assert(f);
    f->active = false;
    assert(stoppedCount < 64);
    stopped[stoppedCount++] = *f;
    f->heard = NULL;
}

static void fakeTick(uint32_t pairs) {
    for (int i = 0; i < 32; i++) {
        FakeVoice *f = &fake[i];
        if (!f->active) continue;
        for (uint32_t p = 0; p < pairs; p++) {
            uint32_t at = (uint32_t) ((f->pos >> 16) % f->len);
            if (((f->wptr - at) & (f->len - 1)) < 4) continue; /* starved: hold */
            uint64_t next = f->pos + f->rate;
            for (uint64_t s = (f->pos >> 16) + 1; s <= (next >> 16); s++) {
                if (f->heardCount == f->heardCap) {
                    f->heardCap *= 2; f->heard = realloc(f->heard, f->heardCap * sizeof(int16_t));
                }
                f->heard[f->heardCount++] = f->ring[s % f->len];
            }
            f->pos = next;
        }
    }
}

/* ---- Fixtures ---- */

#define SHORT_SAMPLES 8192u
#define LONG_SAMPLES 200000u /* 100,000 bytes: streamed */
static int16_t refShort[SHORT_SAMPLES], refLong[LONG_SAMPLES];

static void decodeRef(const uint8_t *data, uint32_t samples, int16_t *out) {
    OfAdpcmState s = {0};
    for (uint32_t i = 0; i < samples; i++) out[i] = ofAdpcmDecode(&s, (i & 1) ? data[i / 2] >> 4 : data[i / 2] & 0x0F);
}

static void makePack(void) {
    FILE *f = fopen("music.bin", "wb"); assert(f);
    uint32_t count = 2;
    /* Sorted by name, as mkmusic writes them. */
    OfMusicTrack tracks[2] = {{.name = "long", .sampleCount = LONG_SAMPLES, .sampleRate = 32000},
                              {.name = "short", .sampleCount = SHORT_SAMPLES, .sampleRate = 32000}};
    tracks[1].offset = 8 + sizeof(tracks);
    tracks[0].offset = tracks[1].offset + SHORT_SAMPLES / 2;
    assert(fwrite("UTM1", 1, 4, f) == 4 && fwrite(&count, 4, 1, f) == 1 && fwrite(tracks, sizeof(tracks), 1, f) == 1);
    static uint8_t a[SHORT_SAMPLES / 2], b[LONG_SAMPLES / 2];
    uint32_t seed = 12345;
    for (unsigned i = 0; i < sizeof(a); i++) { seed = seed * 1103515245u + 12345u; a[i] = (uint8_t) (seed >> 16); }
    for (unsigned i = 0; i < sizeof(b); i++) { seed = seed * 1103515245u + 12345u; b[i] = (uint8_t) (seed >> 16); }
    assert(fwrite(a, 1, sizeof(a), f) == sizeof(a) && fwrite(b, 1, sizeof(b), f) == sizeof(b));
    assert(!fclose(f));
    decodeRef(a, SHORT_SAMPLES, refShort);
    decodeRef(b, LONG_SAMPLES, refLong);
}

static OfAudioSystem *newSystem(void) {
    for (int i = 0; i < 32; i++) { free(fake[i].heard); memset(&fake[i], 0, sizeof(fake[i])); }
    for (int i = 0; i < stoppedCount; i++) free(stopped[i].heard);
    stoppedCount = 0;
    OfAudioSystem *ut = calloc(1, sizeof(*ut)); assert(ut);
    ut->base.vtable = &ofVtable; ut->masterGain = 1; ut->nextInstanceId = OF_INSTANCE_ID_BASE;
    adpcmDiffInit();
    openPack(ut); assert(ut->file);
    hwMixerInit(ut);
    g_audio = ut;
    return ut;
}

/* One 10 ms frame: the mixer plays, then the game's audio update runs. */
static void frame(OfAudioSystem *ut) {
    g_now += 10000000ull;
    fakeTick(480);
    ofUpdate(&ut->base, 0.01f);
}

static int play(OfAudioSystem *ut, const char *name, bool loop) {
    int32_t stream = ofCreateStream(&ut->base, name); assert(stream >= OF_STREAM_INDEX_BASE);
    int id = ofPlaySound(&ut->base, stream, 0, loop); assert(id >= OF_INSTANCE_ID_BASE);
    return id;
}

static const FakeVoice *voiceHeard(int index) { return fake[index].active ? &fake[index] : NULL; }

static void checkHeard(const int16_t *heard, uint32_t count, const int16_t *ref, uint32_t refCount, bool loop) {
    for (uint32_t i = 0; i < count; i++) {
        int16_t expected = loop || i < refCount ? ref[i % refCount] : 0;
        if (heard[i] != expected) { fprintf(stderr, "sample %u: %d, expected %d\n", i, heard[i], expected); abort(); }
    }
}

static void testShortSoundPlaysOutAndStops(void) {
    OfAudioSystem *ut = newSystem(); assert(ut->hwMixer);
    int id = play(ut, "short", false);
    int frames = 0;
    while (ofIsPlaying(&ut->base, id)) { frame(ut); assert(++frames < 100); }
    /* 8192 samples at 32 kHz is 256 ms. */
    assert(frames >= 25 && frames <= 30);
    assert(stoppedCount == 1 && stopped[0].heardCount >= SHORT_SAMPLES);
    checkHeard(stopped[0].heard, stopped[0].heardCount, refShort, SHORT_SAMPLES, false);
    assert(softwarePairs == 0);
    ofDestroy(&ut->base);
}

static void testLoopedStreamAcrossWrapsAndLoops(void) {
    OfAudioSystem *ut = newSystem();
    int id = play(ut, "long", true);
    for (int f = 0; f < 900; f++) {
        frame(ut);
        if (f == 300) {
            /* What is heard is where the track position says. */
            const FakeVoice *v = voiceHeard(0); assert(v);
            float position = ofGetTrackPosition(&ut->base, id);
            float heard = (float) (v->heardCount - 1) / 32000.0f;
            assert(fabsf(position - heard) < 0.0002f);
        }
    }
    const FakeVoice *v = voiceHeard(0); assert(v);
    assert(v->heardCount > LONG_SAMPLES + 50000); /* looped once */
    checkHeard(v->heard, v->heardCount, refLong, LONG_SAMPLES, true);
    ofDestroy(&ut->base);
}

static void testPauseHoldsAndResumes(void) {
    OfAudioSystem *ut = newSystem();
    int id = play(ut, "long", true);
    for (int f = 0; f < 50; f++) frame(ut);
    ofPauseSound(&ut->base, id);
    frame(ut);
    uint32_t held = voiceHeard(0)->heardCount;
    for (int f = 0; f < 50; f++) frame(ut);
    assert(voiceHeard(0)->heardCount == held && fake[0].rate == 0 && fake[0].volume == 0);
    ofResumeSound(&ut->base, id);
    for (int f = 0; f < 50; f++) frame(ut);
    ofPauseAll(&ut->base); frame(ut); held = voiceHeard(0)->heardCount;
    frame(ut); assert(voiceHeard(0)->heardCount == held);
    ofResumeAll(&ut->base);
    for (int f = 0; f < 50; f++) frame(ut);
    const FakeVoice *v = voiceHeard(0);
    assert(v->heardCount > held && v->volume == 255);
    checkHeard(v->heard, v->heardCount, refLong, LONG_SAMPLES, true);
    ofDestroy(&ut->base);
}

static void testGainPitchAndStop(void) {
    OfAudioSystem *ut = newSystem();
    int id = play(ut, "long", true);
    frame(ut); frame(ut);
    ofSetSoundGain(&ut->base, id, 0.5f, 0);
    frame(ut);
    assert(fake[0].volume == 128);
    ofSetSoundGain(&ut->base, id, 0.0f, 100); /* a 100 ms fade */
    frame(ut); assert(fake[0].volume > 90 && fake[0].volume < 128);
    for (int f = 0; f < 12; f++) frame(ut);
    assert(fake[0].volume == 0);
    ofSetSoundPitch(&ut->base, id, 1.5f);
    frame(ut); assert(fake[0].rate == 65536);
    ofSetSoundPitch(&ut->base, id, 4.0f);
    frame(ut); assert(fake[0].rate == OF_HW_MAX_RATE);
    ofSetMasterGain(&ut->base, 0.5f); ofSetSoundGain(&ut->base, id, 1.0f, 0);
    frame(ut); assert(fake[0].volume == 128);
    ofStopSound(&ut->base, id);
    assert(!fake[0].active && !ofIsPlaying(&ut->base, id));
    for (int f = 0; f < 3; f++) frame(ut);
    assert(!fake[0].active);
    ofDestroy(&ut->base);
}

static void testSeekRestartsTheRing(void) {
    OfAudioSystem *ut = newSystem();
    int id = play(ut, "long", true);
    for (int f = 0; f < 20; f++) frame(ut);
    ofSetTrackPosition(&ut->base, id, 3.0f);
    assert(stoppedCount == 1 && !fake[0].active);
    float position = ofGetTrackPosition(&ut->base, id);
    assert(fabsf(position - 3.0f) < 0.001f);
    for (int f = 0; f < 10; f++) frame(ut);
    position = ofGetTrackPosition(&ut->base, id);
    assert(position > 3.05f && position < 3.11f);
    ofDestroy(&ut->base);
}

static void testTwoVoicesAndMute(void) {
    OfAudioSystem *ut = newSystem();
    int music = play(ut, "long", true);
    for (int f = 0; f < 10; f++) frame(ut);
    int sfx = play(ut, "short", false);
    for (int f = 0; f < 40; f++) frame(ut);
    assert(!ofIsPlaying(&ut->base, sfx) && ofIsPlaying(&ut->base, music));
    assert(stoppedCount == 1);
    checkHeard(stopped[0].heard, stopped[0].heardCount, refShort, SHORT_SAMPLES, false);
    checkHeard(fake[0].heard, fake[0].heardCount, refLong, LONG_SAMPLES, true);
    /* A stream is music: muting music releases its voice, and the track goes on unheard. */
    float before = ofGetTrackPosition(&ut->base, music);
    ofAudioSetMode(OF_AUDIO_SOUND_ONLY);
    frame(ut);
    assert(!fake[0].active || fake[0].handle != stopped[0].handle);
    for (int f = 0; f < 100; f++) frame(ut);
    float muted = ofGetTrackPosition(&ut->base, music);
    assert(muted > before + 0.95f && muted < before + 1.06f);
    ofAudioSetMode(OF_AUDIO_NORMAL);
    for (int f = 0; f < 5; f++) frame(ut);
    int heardVoices = 0;
    for (int i = 0; i < 32; i++) heardVoices += fake[i].active;
    assert(heardVoices == 1);
    /* With everything muted nothing pumps, so the voice must go at once. */
    ofAudioSetMode(OF_AUDIO_DISABLED);
    for (int i = 0; i < 32; i++) assert(!fake[i].active);
    ofAudioSetMode(OF_AUDIO_NORMAL);
    ofDestroy(&ut->base);
}

static void testStatesRefusedAndSoftwareFallback(void) {
    OfAudioSystem *ut = newSystem();
    play(ut, "long", true);
    frame(ut); frame(ut);
    uint8_t *data = NULL; size_t size = 0; const char *reason = NULL;
    assert(!ofAudioSnapshot(&ut->base, &data, &size, &reason) && reason);
    ofDestroy(&ut->base);

    fakeAvailable = false;
    ut = newSystem(); assert(!ut->hwMixer);
    play(ut, "long", true);
    for (int f = 0; f < 5; f++) frame(ut);
    assert(softwarePairs > 0 && !fake[0].active);
    ofDestroy(&ut->base);
    fakeAvailable = true;

    /* A voice the OS will not start sends everything back to software, from where it was. */
    ut = newSystem(); assert(ut->hwMixer);
    fakeRefuse = true;
    int id = play(ut, "short", false);
    softwarePairs = 0;
    frame(ut);
    assert(!ut->hwMixer && softwarePairs > 0 && ofIsPlaying(&ut->base, id));
    assert(ut->voices[0].samplesDecoded < 4000); /* the software mix only, not the failed start too */
    fakeRefuse = false;
    ofDestroy(&ut->base);

    ofAudioUseSoftwareMixer(true);
    ut = newSystem(); assert(!ut->hwMixer);
    ofDestroy(&ut->base);
    ofAudioUseSoftwareMixer(false);
}

int main(int argc, char **argv) {
    (void) argc;
    char dir[] = "/tmp/of_audio_hw_XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    (void) argv;
    makePack();
    testShortSoundPlaysOutAndStops();
    testLoopedStreamAcrossWrapsAndLoops();
    testPauseHoldsAndResumes();
    testGainPitchAndStop();
    testSeekRestartsTheRing();
    testTwoVoicesAndMute();
    testStatesRefusedAndSoftwareFallback();
    remove("music.bin"); rmdir(dir);
    printf("PASS hardware mixer: decode, wraps, loops, pause, gain, pitch, seek, mute, fallback\n");
    return 0;
}
