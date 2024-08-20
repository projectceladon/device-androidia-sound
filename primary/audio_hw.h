// Copyright (C) 2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

//
// All hash includes - Start
//
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <cutils/str_parms.h>
#include <log/log.h>

#include <safe_mem_lib.h>
#include <safe_str_lib.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <system/audio.h>

#include <linux/ioctl.h>
#include <sound/asound.h>
#include <tinyalsa/asoundlib.h>

#include <audio_route/audio_route.h>
#include <audio_utils/channels.h>
#include <audio_utils/resampler.h>

// All hash includes - End

//
// All hash defines - Start
//
#define PCM_CARD 0
#define PCM_CARD_DEFAULT 0
#define PCM_DEVICE 0

#define OUT_PERIOD_SIZE 1024
#define OUT_PERIOD_COUNT 4
#define OUT_SAMPLING_RATE 48000

#define IN_PERIOD_SIZE 1024 // default period size
#define IN_PERIOD_MS 10
#define IN_PERIOD_COUNT 4
#define IN_SAMPLING_RATE 48000

#define AUDIO_PARAMETER_HFP_ENABLE "hfp_enable"
#define AUDIO_PARAMETER_BT_SCO "BT_SCO"
#define AUDIO_BT_DRIVER_NAME "btaudiosource"
#define SAMPLE_SIZE_IN_BYTES 2
#define SAMPLE_SIZE_IN_BYTES_STEREO 4

#define PCM_DUMMY_DEVICE 0

// All hash defines - End

//
// All Struct defines - Start
//

struct pcm_config pcm_config_out = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = OUT_PERIOD_SIZE,
    .period_count = OUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = OUT_PERIOD_SIZE * OUT_PERIOD_COUNT,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = IN_SAMPLING_RATE,
    .period_size = IN_PERIOD_SIZE,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
    .stop_threshold = (IN_PERIOD_SIZE * IN_PERIOD_COUNT),
};

//[ BT ALSA Card config
struct pcm_config bt_out_config = {.channels = 1,
                                   .rate = 8000,
                                   .period_size = 240,
                                   .period_count = 5,
                                   .start_threshold = 0,
                                   .stop_threshold = 0,
                                   .silence_threshold = 0,
                                   .silence_size = 0,
                                   .avail_min = 0};

struct pcm_config bt_in_config = {.channels = 1,
                                  .rate = 8000,
                                  .period_size = 240,
                                  .period_count = 5,
                                  .start_threshold = 0,
                                  .stop_threshold = 0,
                                  .silence_threshold = 0,
                                  .silence_size = 0,
                                  .avail_min = 0};

struct audio_device {
  struct audio_hw_device hw_device;

  pthread_mutex_t lock; /* see note below on mutex acquisition order */
  unsigned int out_device;
  unsigned int in_device;
  bool standby;
  bool mic_mute;
  struct audio_route *ar;
  void *hal_config;

  int card;
  int cardc;
  struct stream_out *active_out;
  struct stream_in *active_in;

  //[BT-HFP Voice Call
  bool is_hfp_call_active;
  // BT-HFP Voice Call]

  bool in_needs_standby;
  bool out_needs_standby;

  //[BT SCO VoIP Call
  bool in_sco_voip_call;
  int bt_card;
  struct resampler_itfe *voip_in_resampler;
  struct resampler_itfe *voip_out_resampler;
  // BT SCO VoIP Call]
};

struct stream_out {
  struct audio_stream_out stream;

  pthread_mutex_t lock; /* see note below on mutex acquisition order */
  struct pcm *pcm;
  struct pcm_config *pcm_config;
  struct audio_config req_config;
  bool unavailable;
  bool standby;
  uint64_t written;
  struct audio_device *dev;
};

struct stream_in {
  struct audio_stream_in stream;

  pthread_mutex_t lock; /* see note below on mutex acquisition order */
  struct pcm *pcm;
  struct pcm_config *pcm_config;
  struct audio_config req_config;
  bool unavailable;
  bool standby;

  struct audio_device *dev;
};

struct pcm_config dummy_pcm_config_out = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = OUT_PERIOD_SIZE,
    .period_count = OUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = OUT_PERIOD_SIZE * OUT_PERIOD_COUNT,
};

struct pcm_config dummy_pcm_config_in = {
    .channels = 2,
    .rate = IN_SAMPLING_RATE,
    .period_size = IN_PERIOD_SIZE,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
    .stop_threshold = (IN_PERIOD_SIZE * IN_PERIOD_COUNT),
};

// All Struct defines - End

//
// All Function declarations - Start
//
static uint32_t out_get_sample_rate(const struct audio_stream *stream);
static size_t out_get_buffer_size(const struct audio_stream *stream);
static audio_format_t out_get_format(const struct audio_stream *stream);
static uint32_t in_get_sample_rate(const struct audio_stream *stream);
static size_t in_get_buffer_size(const struct audio_stream *stream);
static audio_format_t in_get_format(const struct audio_stream *stream);

static unsigned int round_to_16_mult(unsigned int size) {
  return (size + 15) & ~15; /* 0xFFFFFFF0; */
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream) {
  struct stream_out *out = (struct stream_out *)stream;
  ALOGV("%s : rate %d", __func__, out->req_config.sample_rate);
  return out->req_config.sample_rate;
}

static int out_set_sample_rate(struct audio_stream *stream __unused,
                               uint32_t rate __unused) {
  ALOGV("out_set_sample_rate: %d", rate);
  return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream) {
  ALOGV("out_get_buffer_size");
  return pcm_config_out.period_size *
         audio_stream_out_frame_size((struct audio_stream_out *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream) {
  struct stream_out *out = (struct stream_out *)stream;
  ALOGV("%s : channels %d", __func__, popcount(out->req_config.channel_mask));
  return out->req_config.channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream) {
  ALOGV("%s", __func__);
  struct stream_out *out = (struct stream_out *)stream;
  return out->req_config.format;
}

static int out_set_format(struct audio_stream *stream __unused,
                          audio_format_t format __unused) {
  return -ENOSYS;
}

static int out_dump(const struct audio_stream *stream __unused,
                    int fd __unused) {
  ALOGV("out_dump");
  return 0;
}

static uint32_t
out_get_latency(const struct audio_stream_out *stream __unused) {
  ALOGV("out_get_latency");
  return (pcm_config_out.period_size * OUT_PERIOD_COUNT * 1000) /
         pcm_config_out.rate;
}

static int out_set_volume(struct audio_stream_out *stream __unused,
                          float left __unused, float right __unused) {
  ALOGV("out_set_volume: Left:%f Right:%f", left, right);
  return -ENOSYS;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames) {
  struct stream_out *out = (struct stream_out *)stream;
  *dsp_frames = out->written;
  ALOGV("%s : dsp_frames: %d", __func__, *dsp_frames);
  return 0;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                         uint64_t *frames,
                                         struct timespec *timestamp) {
  struct stream_out *out = (struct stream_out *)stream;
  int ret = -1;

  if (out->pcm) {
    unsigned int avail;
    if (pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
      unsigned int kernel_buffer_size =
          out->pcm_config->period_size * out->pcm_config->period_count;
      int64_t signed_frames = out->written - kernel_buffer_size + avail;
      if (signed_frames >= 0) {
        *frames = signed_frames;
        ret = 0;
      }
    }
  }

  return ret;
}

static int out_add_audio_effect(const struct audio_stream *stream __unused,
                                effect_handle_t effect __unused) {
  ALOGV("out_add_audio_effect: %p", effect);
  return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream __unused,
                                   effect_handle_t effect __unused) {
  ALOGV("out_remove_audio_effect: %p", effect);
  return 0;
}

static int
out_get_next_write_timestamp(const struct audio_stream_out *stream __unused,
                             int64_t *timestamp __unused) {
  ALOGV("%s", __func__);
  return -ENOSYS;
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream) {
  struct stream_in *in = (struct stream_in *)stream;
  ALOGV("%s : req_config %d", __func__, in->req_config.sample_rate);
  return in->req_config.sample_rate;
}

static int in_set_sample_rate(struct audio_stream *stream __unused,
                              uint32_t rate __unused) {
  ALOGV("in_set_sample_rate: %d", rate);
  return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream) {
  struct stream_in *in = (struct stream_in *)stream;
  size_t size;

  /*
   * take resampling into account and return the closest majoring
   * multiple of 16 frames, as audioflinger expects audio buffers to
   * be a multiple of 16 frames
   */
  size = (in->pcm_config->period_size * in_get_sample_rate(stream)) /
         in->pcm_config->rate;
  size = ((size + 15) / 16) * 16;

  size *= audio_stream_in_frame_size(&in->stream);
  ALOGV("%s : buffer_size : %zu", __func__, size);
  return size;
}

static uint32_t in_get_channels(const struct audio_stream *stream) {
  struct stream_in *in = (struct stream_in *)stream;

  ALOGV("%s : channels %d", __func__, popcount(in->req_config.channel_mask));
  return in->req_config.channel_mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream) {
  struct stream_in *in = (struct stream_in *)stream;
  ALOGV("%s : req_config format %d", __func__, in->req_config.format);
  return in->req_config.format;
}

static int in_set_format(struct audio_stream *stream __unused,
                         audio_format_t format __unused) {
  return -ENOSYS;
}

static int in_dump(const struct audio_stream *stream __unused,
                   int fd __unused) {
  return 0;
}

static int in_set_gain(struct audio_stream_in *stream __unused,
                       float gain __unused) {
  return 0;
}

static uint32_t
in_get_input_frames_lost(struct audio_stream_in *stream __unused) {
  return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream __unused,
                               effect_handle_t effect __unused) {
  return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream __unused,
                                  effect_handle_t effect __unused) {
  return 0;
}

int in_read_bt(struct stream_in *in, struct audio_device *adev, void *buffer,
               size_t bytes);
int out_write_bt(struct stream_out *out, struct audio_device *adev,
                 const void *buffer, size_t bytes);

// All Function declarations - End
