/* Tests for audio/speaker diaphragm generation
 *
 * Copyright (C) 2026
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "keyboard.h"
#include "z80.h"

/* Audio state constants matching main.c */
#define AUDIO_SAMPLE_RATE 44100
#define CYCLES_PER_FRAME 65000
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 50) /* 882 */

unsigned long tstates = 0, tsmax = CYCLES_PER_FRAME;
static int speaker_diaphragm_pos = 0;
static unsigned long last_speaker_tstates = 0;
static float audio_buffer[SAMPLES_PER_FRAME];

/* Mock UI audio queue for testing */
static int mock_queued_samples = 0;
static float last_queued_buffer[SAMPLES_PER_FRAME];

void ui_audio_init(int sample_rate) { (void)sample_rate; }
void ui_audio_queue(float *buffer, int samples) {
  mock_queued_samples = samples;
  memcpy(last_queued_buffer, buffer, samples * sizeof(float));
}
void ui_sync_frame(void) {}

static void set_speaker_diaphragm(int pos) {
  if (tsmax != CYCLES_PER_FRAME) {
    speaker_diaphragm_pos = pos;
    return;
  }

  unsigned long current_t = tstates;
  if (current_t < last_speaker_tstates) {
    last_speaker_tstates = 0;
  }
  if (current_t > CYCLES_PER_FRAME)
    current_t = CYCLES_PER_FRAME;

  unsigned int last_s =
      (last_speaker_tstates * SAMPLES_PER_FRAME) / CYCLES_PER_FRAME;
  unsigned int current_s = (current_t * SAMPLES_PER_FRAME) / CYCLES_PER_FRAME;

  if (current_s > SAMPLES_PER_FRAME)
    current_s = SAMPLES_PER_FRAME;

  float val = speaker_diaphragm_pos ? 0.15f : -0.15f;
  for (unsigned int i = last_s; i < current_s; i++) {
    audio_buffer[i] = val;
  }

  speaker_diaphragm_pos = pos;
  last_speaker_tstates = current_t;
}

static unsigned int test_in(int h, int l) {
  (void)h;
  if ((l & 1) == 0) {
    set_speaker_diaphragm(0);
    return 255;
  }
  return 255;
}

static unsigned int test_out(int h, int l, int a) {
  (void)h; (void)a;
  if ((l & 1) == 0) {
    set_speaker_diaphragm(1);
  }
  return 0;
}

static void test_even_port_diaphragm_states() {
  tsmax = CYCLES_PER_FRAME;
  tstates = 0;
  last_speaker_tstates = 0;
  speaker_diaphragm_pos = 0;

  /* OUT to even port sets speaker diaphragm to 1 (ON) */
  tstates = 100;
  test_out(0x00, 0xfe, 0);
  assert(speaker_diaphragm_pos == 1);

  /* IN to even port sets speaker diaphragm to 0 (OFF) */
  tstates = 500;
  test_in(0x00, 0xfe);
  assert(speaker_diaphragm_pos == 0);
}

static void test_keyboard_scan_no_buzzing() {
  tsmax = CYCLES_PER_FRAME;
  tstates = 0;
  last_speaker_tstates = 0;
  speaker_diaphragm_pos = 0;

  /* Simulate continuous keyboard IN polling (scanning 8 keyports) */
  for (int step = 1; step <= 8; step++) {
    tstates = step * 100;
    test_in(0xFE >> (step - 1), 0xFE);
  }

  /* Fill remaining frame */
  tstates = CYCLES_PER_FRAME;
  set_speaker_diaphragm(speaker_diaphragm_pos);

  /* Verify no sound transitions occurred during idle keyboard polling */
  int transitions = 0;
  for (int i = 1; i < SAMPLES_PER_FRAME; i++) {
    if (audio_buffer[i] != audio_buffer[i - 1]) {
      transitions++;
    }
  }
  assert(transitions == 0);
}

static void test_beep_square_wave_generation() {
  tsmax = CYCLES_PER_FRAME;
  tstates = 0;
  last_speaker_tstates = 0;
  speaker_diaphragm_pos = 0;

  /* Simulate Jupiter Ace ROM BEEP loop: IN (OFF) and OUT (ON) alternating */
  for (int step = 1; step <= 10; step++) {
    tstates = step * 369;
    if (step % 2 == 1) {
      test_in(0x7F, 0xFE);
    } else {
      test_out(0x00, 0xFE, 0);
    }
  }

  /* Fill remaining frame */
  tstates = CYCLES_PER_FRAME;
  set_speaker_diaphragm(speaker_diaphragm_pos);

  /* Verify buffer contains alternating square wave values */
  int transitions = 0;
  for (int i = 1; i < SAMPLES_PER_FRAME; i++) {
    if (audio_buffer[i] != audio_buffer[i - 1]) {
      transitions++;
    }
  }
  assert(transitions >= 8);
}

int main() {
  test_even_port_diaphragm_states();
  test_keyboard_scan_no_buzzing();
  test_beep_square_wave_generation();
  printf("audio_test: ALL TESTS PASSED\n");
  return 0;
}
