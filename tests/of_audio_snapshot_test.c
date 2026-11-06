#define OF_PC 1
#include "../src/openfpga/platform/of_audio_system.c"
#include "../src/openfpga/platform/of_snapshot_parts.h"
#include <assert.h>

void logWarn(const char *fmt, ...) { (void)fmt; }
void logError(const char *fmt, ...) { (void)fmt; abort(); }
void logInfo(const char *fmt, ...);
uint64_t nowNanos(void) { return 0; }
bool ofPerfDetail = false;
void ofPerfAddLoad(OfLoadKind kind, uint64_t value) { (void)kind; (void)value; }
void ofPerfPhase(OfPhase phase) { (void)phase; }
void ofLoadingTick(void) {}
#include <stdarg.h>
static char lastInfo[512];
void logInfo(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(lastInfo, sizeof(lastInfo), fmt, ap); va_end(ap);
}
static int freePairs;
static int16_t submitted[256 * 2];
static bool overrideWriteResult;
static int writeResult;
static int submittedCount;
static bool stateRequestPending;
static int deferAfterChecks = -1;
bool ofSavestateDeferFileReads(void) {
    return stateRequestPending || (deferAfterChecks >= 0 && deferAfterChecks-- == 0);
}
int of_audio_free(void) { return freePairs; }
void of_audio_init(void) { submittedCount = 0; }
void ofPcAudioSnapshotReset(void) { submittedCount = 0; }
int of_audio_write(const int16_t *samples, int count) {
    assert(count <= 256);
    memcpy(submitted, samples, (size_t)count * 4);
    submittedCount = count;
    return overrideWriteResult ? writeResult : count;
}

/* Independent scalar oracle: preserves the original uint64 wrap semantics. */
static void referenceAppend(int16_t *ring, int capacity, uint64_t *written,
                            const int16_t *pcm, int pairs) {
    for (int i = 0; i < pairs; ++i) {
        size_t at = (size_t)((*written)++ % (uint64_t)capacity) * 2;
        ring[at] = pcm[i * 2]; ring[at + 1] = pcm[i * 2 + 1];
    }
}

/* Catch channel swaps, stale cursors, span overruns, and uint64 wrap drift. */
static void testRingAppend(void) {
    const int capacities[] = {1, 3, 7, 16, 131072};
    for (unsigned c = 0; c < sizeof(capacities) / sizeof(*capacities); c++) {
        int capacity = capacities[c];
        const uint64_t starts[] = {0, (uint64_t)capacity - 1, (uint64_t)capacity,
            (uint64_t)UINT32_MAX + 5, UINT64_MAX - (uint64_t)capacity, UINT64_MAX - 1};
        const int counts[] = {0, 1, capacity - 1, 2, capacity, capacity * 2 + 3, 0};
        size_t bytes = (size_t)capacity * 2 * sizeof(int16_t);
        int16_t *expected = malloc(bytes); assert(expected);
        int16_t *pcm = malloc((size_t)(capacity * 2 + 3) * 2 * sizeof(int16_t));
        assert(pcm);
        for (int i = 0; i < capacity * 2 + 3; i++) {
            pcm[i * 2] = (int16_t)(1 + i % 30000);
            pcm[i * 2 + 1] = (int16_t)(-1 - (i * 7) % 30000);
        }
        for (unsigned start = 0; start < sizeof(starts) / sizeof(*starts); start++) {
            OfAudioSystem ut = {0};
            ut.queueCapacity = capacity; ut.snapshotWritten = starts[start];
            ut.snapshotPcm = malloc(bytes); assert(ut.snapshotPcm);
            for (int i = 0; i < capacity; i++) {
                ut.snapshotPcm[i * 2] = -12345;
                ut.snapshotPcm[i * 2 + 1] = 23456;
            }
            memcpy(expected, ut.snapshotPcm, bytes);
            uint64_t written = starts[start];
            for (unsigned n = 0; n < sizeof(counts) / sizeof(*counts); n++) {
                referenceAppend(expected, capacity, &written, pcm, counts[n]);
                rememberAcceptedPcm(&ut, pcm, counts[n]);
                assert(ut.snapshotWritten == written);
                assert(memcmp(ut.snapshotPcm, expected, bytes) == 0);
            }
            free(ut.snapshotPcm);
        }
        free(pcm); free(expected);
    }
    puts("audio PCM ring append: passed");
}

/* Rejecting a write or accepting a prefix must not remember the other pairs. */
static void testAcceptedPrefix(void) {
    const int capacities[] = {7, 263};
    const int requests[] = {256, 273};
    const int results[] = {37, 0, -1, 257};
    int16_t pcm[256 * 2];
    for (unsigned i = 0; i < sizeof(pcm) / sizeof(*pcm); i++) pcm[i] = 2301;
    for (unsigned c = 0; c < sizeof(capacities) / sizeof(*capacities); c++) {
        for (unsigned q = 0; q < sizeof(requests) / sizeof(*requests); q++) {
            for (unsigned r = 0; r <= sizeof(results) / sizeof(*results); r++) {
                OfAudioSystem ut = {0};
                OfMusicTrack track = {0}; ut.tracks = &track;
                ut.queueCapacity = capacities[c]; ut.masterGain = 1;
                ut.snapshotWritten = (uint64_t)UINT32_MAX + 9;
                size_t bytes = (size_t)ut.queueCapacity * 2 * sizeof(int16_t);
                ut.snapshotPcm = malloc(bytes); assert(ut.snapshotPcm);
                int16_t *expected = malloc(bytes); assert(expected);
                for (int i = 0; i < ut.queueCapacity * 2; i++)
                    ut.snapshotPcm[i] = (int16_t)(-1000 - i);
                memcpy(expected, ut.snapshotPcm, bytes);
                uint64_t written = ut.snapshotWritten;
                /* A stationary nonzero voice makes rejected suffixes observable. */
                ut.voices[0].active = true;
                ut.voices[0].sample0 = ut.voices[0].sample1 = 2301;
                ut.voices[0].gainNow = ut.voices[0].gainTarget = OF_GAIN_ONE << 8;
                overrideWriteResult = r < sizeof(results) / sizeof(*results);
                if (overrideWriteResult) writeResult = results[r];
                for (int left = requests[q]; left > 0;) {
                    int chunk = left < 256 ? left : 256;
                    int accepted = overrideWriteResult ? writeResult : chunk;
                    if (accepted > 0 && accepted <= chunk)
                        referenceAppend(expected, ut.queueCapacity, &written, pcm, accepted);
                    left -= chunk;
                }
                mixAndWrite(&ut, requests[q]);
                assert(ut.snapshotWritten == written);
                assert(memcmp(ut.snapshotPcm, expected, bytes) == 0);
                free(expected); free(ut.snapshotPcm);
            }
        }
    }
    overrideWriteResult = false;
    OfAudioSystem withoutShadow = {0};
    withoutShadow.snapshotWritten = 17;
    mixAndWrite(&withoutShadow, 256);
    assert(withoutShadow.snapshotWritten == 17);
    puts("audio accepted-prefix shadow: passed");
}

static void testGroupVolume(void) {
    OfAudioSystem ut = {0};
    DataWin dw = {0}; Sound sounds[2] = {{0}, {0}};
    OfMusicTrack track = {0};
    dw.agrp.count = 2; dw.sond.count = 2; dw.sond.sounds = sounds;
    sounds[1].audioGroup = 1;
    ut.base.dw = &dw; ut.base.vtable = &ofVtable;
    ut.tracks = &track; track.sampleCount = 128;
    OfVoice voice = {0};
    voice.sample0 = voice.sample1 = 16000; voice.step = 1;
    voice.sourceIndex = 1; voice.gainNow = voice.gainTarget = 4096 << 8;
    /* Continue resets group gain before restoring master volume. */
    assert(ofVtable.setGroupGain != NULL);
    ofVtable.setGroupGain(&ut.base, 1, 0.5f, 0);
    int32_t pcm[4] = {0};
    mixVoice(&ut, &voice, pcm, 1); assert(pcm[0] == 8000);
    voice.sourceIndex = 0;
    mixVoice(&ut, &voice, pcm + 1, 1); assert(pcm[1] == 16000);
    voice.sourceIndex = 300000; /* Dynamic streams belong to default group. */
    mixVoice(&ut, &voice, pcm + 2, 1); assert(pcm[2] == 16000);
    ofVtable.setGroupGain(&ut.base, 1, 0, 1000);
    ofVtable.update(&ut.base, 0.5f);
    voice.sourceIndex = 1;
    mixVoice(&ut, &voice, pcm + 3, 1); assert(pcm[3] == 4000);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    ut.nextInstanceId = OF_INSTANCE_ID_BASE;
    assert(ofAudioSnapshot(&ut.base, &bytes, &size, &reason));
    ofVtable.setGroupGain(&ut.base, 1, 1, 0);
    void *prepared = ofAudioPrepareSnapshot(&ut.base, bytes, size, &reason);
    assert(prepared);
    ofAudioApplySnapshot(&ut.base, prepared); ofAudioFreeSnapshot(prepared);
    free(bytes);
    assert(AudioSystem_getGroupGain(&ut.base, 1) == 0.25f);
    ofVtable.pauseAll(&ut.base);
    ofVtable.update(&ut.base, 0.5f);
    assert(AudioSystem_getGroupGain(&ut.base, 1) == 0.25f);
    ofVtable.resumeAll(&ut.base);
    ofVtable.update(&ut.base, 0.5f);
    assert(AudioSystem_getGroupGain(&ut.base, 1) == 0);
    free(ut.base.groupGains);
}

int main(void) {
    testRingAppend();
    testAcceptedPrefix();
    testGroupVolume();
    OfAudioSystem ut = {0};
    ut.base.vtable = &ofVtable;
    OfMusicTrack track = {0};
    uint8_t adpcm[64];
    for (unsigned i = 0; i < sizeof(adpcm); i++) adpcm[i] = (uint8_t)(i * 19);
    strcpy(track.name, "test");
    track.sampleCount = 128; track.sampleRate = 32000;
    ut.tracks = &track; ut.trackCount = 1; ut.masterGain = 1;
    ut.file = tmpfile(); assert(ut.file);
    assert(fwrite(adpcm, 1, sizeof(adpcm), ut.file) == sizeof(adpcm));
    ut.cachedSounds = calloc(1, sizeof(uint8_t *));
    ut.cachedLastUse = calloc(1, sizeof(uint32_t));
    ut.cachedSounds[0] = malloc(sizeof(adpcm));
    memcpy(ut.cachedSounds[0], adpcm, sizeof(adpcm));
    ut.cachedBytes = sizeof(adpcm);
    OfVoice *v = &ut.voices[0];
    v->active = true; v->streamSlot = -1; v->data = ut.cachedSounds[0];
    v->bufferLen = sizeof(adpcm); v->step = 43700; v->frac = 65536;
    v->instanceId = 100012; v->sourceIndex = 7; v->gainNow = 4096 << 8;
    v->gainTarget = 2000 << 8; v->gainStepPerPair = 100; v->pitch = 1;
    ut.voices[1] = *v;
    ut.voices[1].streamSlot = 0;
    ut.voices[1].instanceId = 100011;
    ut.voices[1].fileBytePos = sizeof(adpcm);
    memcpy(ut.streamBuffers[0], adpcm, sizeof(adpcm));
    ut.voices[1].data = ut.streamBuffers[0];
    ut.nextInstanceId = 100013;
    adpcmDiffInit();
    int32_t prefix[9] = {0}, expected[30] = {0}, actual[30] = {0};
    /* Spatial requests must keep Pocket's non-spatial playback usable. */
    assert(ut.base.vtable->setSoundSpatial && ut.base.vtable->setListenerPosition);
    OfVoice unpositioned = *v;
    ut.base.vtable->setSoundSpatial(&ut.base, v->instanceId, 5, 6, 7, 1, 2, 3, 4, 20, 1);
    ut.base.vtable->setListenerPosition(&ut.base, 10, 20, 30);
    int32_t positionedPcm[9] = {0}, unpositionedPcm[9] = {0};
    OfVoice positioned = *v;
    mixVoice(&ut, &positioned, positionedPcm, 9);
    mixVoice(&ut, &unpositioned, unpositionedPcm, 9);
    assert(!memcmp(positionedPcm, unpositionedPcm, sizeof(positionedPcm)));
    mixVoice(&ut, v, prefix, 9);
    mixVoice(&ut, &ut.voices[1], prefix, 9);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    ofVtable.pauseAll(&ut.base);
    assert(ofAudioSnapshot(&ut.base, &bytes, &size, &reason));
    ofVtable.resumeAll(&ut.base);
    assert(!ofVtable.isAllPaused(&ut.base));
    mixVoice(&ut, v, expected, 30);
    mixVoice(&ut, &ut.voices[1], expected, 30);
    void *p = ofAudioPrepareSnapshot(&ut.base, bytes, size, &reason);
    assert(p);
    ofAudioApplySnapshot(&ut.base, p); ofAudioFreeSnapshot(p);
    assert(ofVtable.isAllPaused(&ut.base));
    ofVtable.resumeAll(&ut.base);
    mixVoice(&ut, &ut.voices[0], actual, 30);
    mixVoice(&ut, &ut.voices[1], actual, 30);
    assert(memcmp(expected, actual, sizeof(actual)) == 0);
    /* A Pocket state request must reach the frame boundary without issuing
     * another music read. Preserve read-ahead and decoder state until then. */
    OfVoice deferred = ut.voices[1];
    deferred.bufferPos = 4; deferred.bufferLen = 12; deferred.fileBytePos = 12;
    uint8_t beforeBuffer[64]; memcpy(beforeBuffer, deferred.data, sizeof(beforeBuffer));
    OfVoice beforeVoice = deferred;
    stateRequestPending = true;
    refillVoice(&ut, &deferred);
    assert(memcmp(&beforeVoice, &deferred, sizeof(deferred)) == 0);
    assert(memcmp(beforeBuffer, deferred.data, sizeof(beforeBuffer)) == 0);
    stateRequestPending = false;
    refillVoice(&ut, &deferred);
    assert(deferred.fileBytePos == sizeof(adpcm));
    assert(deferred.bufferPos == 0 && deferred.bufferLen == sizeof(adpcm) - 4);
    /* A request can arrive after the refill entry check. Do not start a
     * fallback file read; retaining the compacted unread tail is sufficient. */
    deferred = beforeVoice;
    memcpy(deferred.data, beforeBuffer, sizeof(beforeBuffer));
    deferAfterChecks = 1;
    refillVoice(&ut, &deferred);
    assert(deferred.fileBytePos == beforeVoice.fileBytePos);
    assert(deferred.bufferPos == 0 && deferred.bufferLen == 8);
    deferAfterChecks = -1;
    refillVoice(&ut, &deferred);
    assert(deferred.fileBytePos == sizeof(adpcm));
    assert(ut.voices[0].instanceId == 100012 && ut.nextInstanceId == 100013);
    for (size_t n = 0; n < size; n++)
        assert(ofAudioPrepareSnapshot(&ut.base, bytes, n, &reason) == NULL);
    uint8_t id[4]; memcpy(id, bytes + 12, 4);
    memset(bytes + 12, 0xff, 4); bytes[15] = 0x7f;
    assert(ofAudioPrepareSnapshot(&ut.base, bytes, size, &reason) == NULL);
    memcpy(bytes + 12, id, 4);
    free(bytes);
    ut.queueCapacity = 8; freePairs = 3; ut.snapshotWritten = 8;
    ut.snapshotPcm = calloc(16, sizeof(int16_t));
    for (unsigned i = 0; i < 16; i++) ut.snapshotPcm[i] = (int16_t)(i * 100);
    assert(ofAudioSnapshot(&ut.base, &bytes, &size, &reason));
    p = ofAudioPrepareSnapshot(&ut.base, bytes, size, &reason); assert(p);
    assert(((OfAudioSnapshot *)p)->pcmPairs == 5);
    ofAudioApplySnapshot(&ut.base, p); ofAudioFreeSnapshot(p);
    assert(submittedCount == 5 && submitted[0] == 600 && submitted[9] == 1500);
    free(bytes); bytes = NULL;
    /* Pocket's inactive capacity is 8; an active stream reserves one pair,
     * hence free=2 means five unread pairs, not six. */
    ut.snapshotReservedPairs = 1; ut.snapshotWritten = 8; freePairs = 2;
    for (unsigned i = 0; i < 16; i++) ut.snapshotPcm[i] = (int16_t)(i * 100);
    assert(ofAudioSnapshot(&ut.base, &bytes, &size, &reason));
    p = ofAudioPrepareSnapshot(&ut.base, bytes, size, &reason); assert(p);
    assert(((OfAudioSnapshot *)p)->pcmPairs == 5);
    ofAudioApplySnapshot(&ut.base, p); ofAudioFreeSnapshot(p);
    assert(submittedCount == 5 && submitted[0] == 600 && submitted[9] == 1500);
    /* Two more capture/restore cycles preserve the unread PCM and decoder,
     * including Pocket's reserved pair. The host's read pointer stays put. */
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        free(bytes); bytes = NULL;
        assert(ofAudioSnapshot(&ut.base, &bytes, &size, &reason));
        p = ofAudioPrepareSnapshot(&ut.base, bytes, size, &reason); assert(p);
        assert(((OfAudioSnapshot *)p)->pcmPairs == 5);
        ofAudioApplySnapshot(&ut.base, p); ofAudioFreeSnapshot(p);
        assert(submittedCount == 5 && submitted[0] == 600 && submitted[9] == 1500);
    }
    /* Appending after restore starts at the restored accepted-pair total. */
    int16_t restoredRing[16]; memcpy(restoredRing, ut.snapshotPcm, sizeof(restoredRing));
    uint64_t restoredWritten = ut.snapshotWritten;
    mixAndWrite(&ut, 4);
    referenceAppend(restoredRing, ut.queueCapacity, &restoredWritten, submitted, 4);
    assert(ut.snapshotWritten == restoredWritten);
    assert(memcmp(ut.snapshotPcm, restoredRing, sizeof(restoredRing)) == 0);
    /* Reject an impossible queue with enough evidence to distinguish hardware
     * accounting from voice validity and allocation failure. */
    free(bytes); bytes = NULL;
    ut.snapshotWritten = 3; freePairs = 0;
    assert(!ofAudioSnapshot(&ut.base, &bytes, &size, &reason));
    assert(strstr(lastInfo, "queue shadow") && strstr(lastInfo, "written 3"));
    free(ut.snapshotPcm);
    free(bytes); free(ut.cachedSounds[0]); free(ut.cachedSounds); free(ut.cachedLastUse);
    fclose(ut.file);
    puts("audio decoder snapshot: passed");
}
