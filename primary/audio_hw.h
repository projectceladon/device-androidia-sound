#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include <log/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>

#include <safe_mem_lib.h>
#include <safe_str_lib.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <system/audio.h>

#include <linux/ioctl.h>
#include <sound/asound.h>
#include <tinyalsa/asoundlib.h>

#include <audio_utils/channels.h>
#include <audio_utils/resampler.h>
#include <audio_route/audio_route.h>



#define PCM_CARD 0
#define PCM_CARD_DEFAULT 0
#define PCM_DEVICE 0

#define OUT_PERIOD_SIZE 1024
#define OUT_PERIOD_COUNT 4
#define OUT_SAMPLING_RATE 48000

#define IN_PERIOD_SIZE 1024 //default period size
#define IN_PERIOD_MS 10
#define IN_PERIOD_COUNT 4
#define IN_SAMPLING_RATE 48000

#define AUDIO_PARAMETER_HFP_ENABLE   "hfp_enable"
#define AUDIO_PARAMETER_BT_SCO       "BT_SCO"
#define AUDIO_BT_DRIVER_NAME         "btaudiosource"
#define SAMPLE_SIZE_IN_BYTES          2
#define SAMPLE_SIZE_IN_BYTES_STEREO   4


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
struct pcm_config bt_out_config = {
    .channels = 1,
    .rate = 8000,
    .period_size = 240,
    .period_count = 5,
    .start_threshold = 0,
    .stop_threshold = 0,
    .silence_threshold = 0,
    .silence_size = 0,
    .avail_min = 0
};

struct pcm_config bt_in_config = {
    .channels = 1,
    .rate = 8000,
    .period_size = 240,
    .period_count = 5,
    .start_threshold = 0,
    .stop_threshold = 0,
    .silence_threshold = 0,
    .silence_size = 0,
    .avail_min = 0
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    unsigned int out_device;
    unsigned int in_device;
    bool standby;
    bool mic_mute;
    struct audio_route *ar;

    int card;
    int cardc;
    struct stream_out *active_out;
    struct stream_in *active_in;

//[BT-HFP Voice Call
    bool is_hfp_call_active;
//BT-HFP Voice Call]

    bool in_needs_standby;
    bool out_needs_standby;

//[BT SCO VoIP Call
    bool in_sco_voip_call;
    int bt_card;
    struct resampler_itfe *voip_in_resampler;
    struct resampler_itfe *voip_out_resampler;
//BT SCO VoIP Call]
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

static uint32_t out_get_sample_rate(const struct audio_stream *stream);
static size_t out_get_buffer_size(const struct audio_stream *stream);
static audio_format_t out_get_format(const struct audio_stream *stream);
static uint32_t in_get_sample_rate(const struct audio_stream *stream);
static size_t in_get_buffer_size(const struct audio_stream *stream);
static audio_format_t in_get_format(const struct audio_stream *stream);
