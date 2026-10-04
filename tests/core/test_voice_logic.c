#include "test.h"
#include "tts_envelope.h"
#include "voice_ring.h"
#include "voice_vad.h"

#include <string.h>

int bongo_cat_test_failures;

static void feed(BongoCatVad *vad, float rms, float echo_floor,
    unsigned frames, BongoCatVadEvent *first) {
    for (unsigned i = 0; i < frames; ++i) {
        BongoCatVadEvent event = bongo_cat_vad_feed(vad, rms, echo_floor);
        if (first && event != BONGO_CAT_VAD_EVENT_NONE && !*first)
            *first = event;
    }
}

/* Frames until an onset is reported: the streak that opens the gate plus the
   confirmation that keeps it. */
#define ONSET_FRAMES (BONGO_CAT_VAD_ONSET_FRAMES + \
    BONGO_CAT_VAD_CONFIRM_MS / BONGO_CAT_VAD_FRAME_MS)
#define BARGE_FRAMES (BONGO_CAT_VAD_BARGE_ONSET_FRAMES + \
    BONGO_CAT_VAD_CONFIRM_MS / BONGO_CAT_VAD_FRAME_MS)

static void test_vad_onset_offset(void) {
    BongoCatVad vad;
    bongo_cat_vad_init(&vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    BongoCatVadEvent event = BONGO_CAT_VAD_EVENT_NONE;
    feed(&vad, 0.001f, 0.0f, 50, NULL);
    CHECK(!vad.speaking);
    feed(&vad, 0.05f, 0.0f, ONSET_FRAMES - 1, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_NONE);
    feed(&vad, 0.05f, 0.0f, 1, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_ONSET);
    CHECK(vad.speaking && vad.confirmed);
    feed(&vad, 0.05f, 0.0f, 40, NULL);
    CHECK(vad.speaking);
    feed(&vad, 0.001f, 0.0f, 22, NULL);
    CHECK(vad.speaking); /* 440 ms is below the 450 ms hangover. */
    event = BONGO_CAT_VAD_EVENT_NONE;
    feed(&vad, 0.001f, 0.0f, 1, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_OFFSET);
    CHECK(!vad.speaking && !vad.confirmed);
}

/* A click or a cough opens the gate but never lasts long enough to be
   confirmed, so the caller is told nothing at all: no bogus utterance. */
static void test_vad_transient_reports_nothing(void) {
    BongoCatVad vad;
    bongo_cat_vad_init(&vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    BongoCatVadEvent event = BONGO_CAT_VAD_EVENT_NONE;
    feed(&vad, 0.001f, 0.0f, 50, NULL);
    feed(&vad, 0.3f, 0.0f, BONGO_CAT_VAD_ONSET_FRAMES + 2u, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_NONE);
    CHECK(vad.speaking && !vad.confirmed);
    feed(&vad, 0.0f, 0.0f, 23, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_NONE);
    CHECK(!vad.speaking && !vad.confirmed);
}

static void test_vad_echo_floor(void) {
    BongoCatVad vad;
    bongo_cat_vad_init(&vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    BongoCatVadEvent event = BONGO_CAT_VAD_EVENT_NONE;
    feed(&vad, 0.001f, 0.0f, 50, NULL);
    /* The pet hears its own voice but stays below the published floor. */
    feed(&vad, 0.09f, 0.1f, 30, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_NONE);
    /* Interrupting needs a longer streak than the silent-room onset does. */
    feed(&vad, 0.15f, 0.1f, BARGE_FRAMES - 1, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_NONE);
    feed(&vad, 0.15f, 0.1f, 1, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_ONSET);
    CHECK(vad.speaking && vad.confirmed);
}

static void test_vad_gate_formula(void) {
    BongoCatVad vad;
    bongo_cat_vad_init(&vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    /* Silence is stricter than the tracking threshold... */
    CHECK(bongo_cat_vad_onset_gate(&vad, 0.0f) > vad.threshold);
    /* ...while the echo floor replaces it, headroom and all. */
    CHECK(bongo_cat_vad_onset_gate(&vad, 0.3f) == 0.3f);
}

static void test_vad_noise_tracking(void) {
    BongoCatVad vad;
    bongo_cat_vad_init(&vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    /* A constant louder "noise" should raise the threshold above itself. */
    for (int i = 0; i < 500; ++i) bongo_cat_vad_feed(&vad, 0.008f, 0.0f);
    CHECK(vad.threshold > 0.01f);
    BongoCatVadEvent event = BONGO_CAT_VAD_EVENT_NONE;
    feed(&vad, 0.012f, 0.0f, 10, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_NONE);
    feed(&vad, 0.2f, 0.0f, ONSET_FRAMES, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_ONSET);
}

static void test_vad_reset_keeps_floor(void) {
    BongoCatVad vad;
    bongo_cat_vad_init(&vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    for (int i = 0; i < 200; ++i) bongo_cat_vad_feed(&vad, 0.007f, 0.0f);
    float floor = vad.noise_floor;
    vad.speaking = true;
    bongo_cat_vad_reset(&vad);
    CHECK(!vad.speaking);
    CHECK(vad.noise_floor == floor);
    CHECK(vad.vadu_end_ms == BONGO_CAT_VAD_TAIL_SILENCE_MS);
}

static void test_vad_loud_room_learns_and_releases(void) {
    BongoCatVad vad;
    bongo_cat_vad_init(&vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    BongoCatVadEvent event = BONGO_CAT_VAD_EVENT_NONE;
    /* A room that never goes quiet: the floor has to learn it while the gate is
       shut, or the threshold stays on its clamp and the gate locks open. */
    feed(&vad, 0.06f, 0.0f, 200, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_NONE);
    CHECK(vad.noise_floor > 0.05f);
    event = BONGO_CAT_VAD_EVENT_NONE;
    feed(&vad, 0.3f, 0.0f, ONSET_FRAMES, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_ONSET);
    /* Back down into the room: the release level now sits above the room, so
       the segment ends instead of running to the maximum length. */
    event = BONGO_CAT_VAD_EVENT_NONE;
    feed(&vad, 0.06f, 0.0f, 30, &event);
    CHECK(event == BONGO_CAT_VAD_EVENT_OFFSET);
    CHECK(!vad.speaking);
}

static void test_ring_basic(void) {
    BongoCatVoiceRing ring;
    bongo_cat_voice_ring_init(&ring);
    CHECK(bongo_cat_voice_ring_available(&ring) == 0);
    unsigned char source[4096];
    for (size_t i = 0; i < sizeof(source); ++i)
        source[i] = (unsigned char)(i % 251);
    size_t written = bongo_cat_voice_ring_write(&ring, source, sizeof(source));
    CHECK(written == sizeof(source));
    CHECK(bongo_cat_voice_ring_available(&ring) == sizeof(source));
    unsigned char target[4096];
    size_t read = bongo_cat_voice_ring_read(&ring, target, sizeof(target));
    CHECK(read == sizeof(source));
    CHECK(memcmp(source, target, sizeof(source)) == 0);
}

static void test_ring_wrap_and_truncate(void) {
    BongoCatVoiceRing ring;
    bongo_cat_voice_ring_init(&ring);
    static unsigned char block[BONGO_CAT_VOICE_RING_CAP];
    size_t half = BONGO_CAT_VOICE_RING_CAP / 2;
    /* Advance head/tail into the middle so writes cross the wrap point. */
    CHECK(bongo_cat_voice_ring_write(&ring, block, half) == half);
    CHECK(bongo_cat_voice_ring_read(&ring, block, half) == half);
    /* One byte is sacrificed to distinguish full from empty. */
    size_t written = bongo_cat_voice_ring_write(&ring, block,
        BONGO_CAT_VOICE_RING_CAP);
    CHECK(written == BONGO_CAT_VOICE_RING_CAP - 1);
    CHECK(bongo_cat_voice_ring_available(&ring) ==
        BONGO_CAT_VOICE_RING_CAP - 1);
    CHECK(bongo_cat_voice_ring_write(&ring, block, 4) == 0);
    CHECK(bongo_cat_voice_ring_read(&ring, block,
        BONGO_CAT_VOICE_RING_CAP) == BONGO_CAT_VOICE_RING_CAP - 1);
    bongo_cat_voice_ring_reset(&ring);
    CHECK(bongo_cat_voice_ring_available(&ring) == 0);
}

static void test_frame_rms(void) {
    int16_t samples[BONGO_CAT_VAD_FRAME_SAMPLES];
    for (unsigned i = 0; i < BONGO_CAT_VAD_FRAME_SAMPLES; ++i)
        samples[i] = 16384;
    float rms = bongo_cat_vad_frame_rms(samples, BONGO_CAT_VAD_FRAME_SAMPLES);
    CHECK(rms > 0.49f && rms < 0.51f);
    CHECK(bongo_cat_vad_frame_rms(samples, 0) == 0.0f);
}

static void test_tts_envelope(void) {
    /* 24 kHz: one envelope frame is 480 samples of 20 ms. */
    CHECK(bongo_cat_tts_envelope_count(0, 24000) == 0);
    CHECK(bongo_cat_tts_envelope_count(1, 24000) == 1);
    CHECK(bongo_cat_tts_envelope_count(480, 24000) == 1);
    CHECK(bongo_cat_tts_envelope_count(481, 24000) == 2);
    CHECK(bongo_cat_tts_envelope_count(480, 0) == 0);

    int16_t pcm[960];
    memset(pcm, 0, sizeof(pcm));
    for (int i = 0; i < 480; ++i) pcm[i] = 1500; /* Half-scale first frame. */
    float env[8];
    size_t n = bongo_cat_tts_envelope_build(pcm, 960, 24000, env, 8);
    CHECK(n == 2);
    /* 1500 / 3000 = 0.5, lifted by the 0.65 gamma to about 0.64. */
    CHECK(env[0] > 0.62f && env[0] < 0.66f);
    CHECK(env[1] == 0.0f);

    /* Loud frames clamp to 1.0 and the output cap truncates. */
    for (int i = 0; i < 960; ++i) pcm[i] = 30000;
    n = bongo_cat_tts_envelope_build(pcm, 960, 24000, env, 8);
    CHECK(n == 2 && env[0] == 1.0f && env[1] == 1.0f);
    n = bongo_cat_tts_envelope_build(pcm, 960, 24000, env, 1);
    CHECK(n == 1);
    CHECK(bongo_cat_tts_envelope_build(NULL, 960, 24000, env, 8) == 2);
    CHECK(env[0] == 0.0f && env[1] == 0.0f);
}

int main(void) {
    test_vad_onset_offset();
    test_vad_transient_reports_nothing();
    test_vad_echo_floor();
    test_vad_gate_formula();
    test_vad_noise_tracking();
    test_vad_reset_keeps_floor();
    test_vad_loud_room_learns_and_releases();
    test_ring_basic();
    test_ring_wrap_and_truncate();
    test_frame_rms();
    test_tts_envelope();
    return bongo_cat_test_failures != 0;
}
