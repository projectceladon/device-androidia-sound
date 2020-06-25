/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_primary"
/*#define LOG_NDEBUG 0*/

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

#define OUT_PERIOD_SIZE 480
#define OUT_PERIOD_COUNT 2
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

//#define DEBUG_PCM_DUMP

#ifdef DEBUG_PCM_DUMP
// To enable dumps, explicitly create "/vendor/dump/" folder and reboot device
FILE *sco_call_write = NULL;
FILE *sco_call_write_remapped = NULL;
FILE *sco_call_write_bt = NULL;
FILE *sco_call_read = NULL;
FILE *sco_call_read_remapped = NULL;
FILE *sco_call_read_bt = NULL;
FILE *out_write_dump = NULL;
FILE *in_read_dump = NULL;
#endif

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

static void select_devices(struct audio_device *adev)
{
    int headphone_on;
    int speaker_on;
    int main_mic_on;
    int headset_mic_on;

    headphone_on = adev->out_device & (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                                    AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    speaker_on = adev->out_device & AUDIO_DEVICE_OUT_SPEAKER;
    main_mic_on = adev->in_device & AUDIO_DEVICE_IN_BUILTIN_MIC;
    headset_mic_on = adev->in_device & AUDIO_DEVICE_IN_WIRED_HEADSET;

    audio_route_reset(adev->ar);
    
    if (speaker_on)
        audio_route_apply_path(adev->ar, "speaker");
    if (headphone_on)
        audio_route_apply_path(adev->ar, "headphone");
    if (main_mic_on)
        audio_route_apply_path(adev->ar, "main-mic");
    if (headset_mic_on)
        audio_route_apply_path(adev->ar, "headset-mic");

    audio_route_update_mixer(adev->ar);
    
    ALOGV("%s : hp=%c speaker=%c main-mic=%c headset-mic=%c",__func__,
      headphone_on ? 'y' : 'n', speaker_on ? 'y' : 'n',
      main_mic_on ? 'y' : 'n', headset_mic_on ? 'y' : 'n' );
}

/* must be called with hw device and output stream mutexes locked */
static void do_out_standby(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        adev->active_out = NULL;
        out->standby = true;
    }
}

/* must be called with hw device and input stream mutexes locked */
static void do_in_standby(struct stream_in *in)
{
    struct audio_device *adev = in->dev;
    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        adev->active_in = NULL;
        in->standby = true;
    }
}

static int get_pcm_card(const char* name)
{
        char id_filepath[PATH_MAX] = {0};
        char number_filepath[PATH_MAX] = {0};
        ssize_t written;

        snprintf(id_filepath, sizeof(id_filepath), "/proc/asound/%s", name);

        written = readlink(id_filepath, number_filepath, sizeof(number_filepath));
        if (written < 0) {
            ALOGE("Sound card %s does not exist - setting default", name);
                return 0;
        } else if (written >= (ssize_t)sizeof(id_filepath)) {
            ALOGE("Sound card %s name is too long - setting default", name);
            return 0;
        }

        return atoi(number_filepath + 4);
}

void update_bt_card(struct audio_device *adev){
    adev->bt_card = get_pcm_card(AUDIO_BT_DRIVER_NAME); //update driver name if changed from BT side.
}

static unsigned int round_to_16_mult(unsigned int size)
{
    return (size + 15) & ~15;   /* 0xFFFFFFF0; */
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;

    ALOGV("%s : config : [rate %d format %d channels %d]",__func__,
            out->pcm_config->rate, out->pcm_config->format, out->pcm_config->channels);

    if (out->unavailable) {
        ALOGV("start_output_stream: output not available");
        return -ENODEV;
    }

//[BT SCO VoIP Call
    if(adev->in_sco_voip_call) {
        ALOGD("%s : sco voip call active", __func__);

        ALOGV("%s : opening pcm [%d : %d] for config : [rate %d format %d channels %d]", __func__, adev->bt_card, PCM_DEVICE,
                bt_out_config.rate, bt_out_config.format, bt_out_config.channels);

        out->pcm = pcm_open(adev->bt_card, PCM_DEVICE /*0*/, PCM_OUT, &bt_out_config);
//BT SCO VoIP Call]
    } else {
        ALOGI("PCM playback card selected = %d, \n", adev->card);
        out->pcm = pcm_open(adev->card, PCM_DEVICE, PCM_OUT | PCM_NORESTART | PCM_MONOTONIC, out->pcm_config);
    }

    if (!out->pcm) {
        ALOGE("pcm_open(out) failed: device not found");
        return -ENODEV;
    } else if (!pcm_is_ready(out->pcm)) {
        ALOGE("pcm_open(out) failed: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        out->unavailable = true;
        return -ENOMEM;
    }

    adev->active_out = out;

    /* force mixer updates */
    select_devices(adev);

    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

//[BT SCO VoIP Call
    if(adev->in_sco_voip_call) {
        ALOGD("%s : sco voip call active", __func__);

        ALOGV("%s : opening pcm [%d : %d] for config : [rate %d format %d channels %d]",__func__, adev->bt_card, PCM_DEVICE,
                bt_in_config.rate, bt_in_config.format, bt_in_config.channels);

        in->pcm = pcm_open(adev->bt_card, PCM_DEVICE, PCM_IN, &bt_in_config);
//BT SCO VoIP Call]
    } else {
        ALOGI("PCM record card selected = %d, \n", adev->card);

        ALOGV("%s : config : [rate %d format %d channels %d]",__func__,
            in->pcm_config->rate, in->pcm_config->format, in->pcm_config->channels);

        in->pcm = pcm_open(adev->cardc, PCM_DEVICE, PCM_IN, in->pcm_config);
    }

    if (!in->pcm) {
        return -ENODEV;
    } else if (!pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open(in) failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    adev->active_in = in;

    /* force mixer updates */
    select_devices(adev);

    return 0;
}

/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    ALOGV("%s : rate %d",__func__, out->req_config.sample_rate);
    return out->req_config.sample_rate;
}

static int out_set_sample_rate(struct audio_stream *stream __unused, uint32_t rate __unused)
{
    ALOGV("out_set_sample_rate: %d", rate);
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    ALOGV("out_get_buffer_size");
    return pcm_config_out.period_size *
               audio_stream_out_frame_size((struct audio_stream_out *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    ALOGV("%s : channels %d",__func__,  popcount(out->req_config.channel_mask));
    return out->req_config.channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    ALOGV("%s",__func__);
    struct stream_out *out = (struct stream_out *)stream;
    return out->req_config.format;
}

static int out_set_format(struct audio_stream *stream __unused, audio_format_t format __unused)
{
    return -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    ALOGV("out_standby");
    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    do_out_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return 0;
}

static int out_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    ALOGV("out_dump");
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("%s : kvpairs : %s",__func__, kvpairs);
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    int status = 0;
    unsigned int val;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));

    pthread_mutex_lock(&adev->lock);
    if (ret >= 0) {
        val = atoi(value);
        if ((adev->out_device != val) && (val != 0)) {
            adev->out_device = val;
            select_devices(adev);
        }

#if 0 // FIXME, does this need to replace above code ?
	pthread_mutex_lock(&out->lock);
        if (((adev->devices & AUDIO_DEVICE_OUT_ALL) != val) && (val != 0)) {
            adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
            adev->devices |= val;
        }
        pthread_mutex_unlock(&out->lock);
#endif

    }
    pthread_mutex_unlock(&adev->lock);
    
    str_parms_destroy(parms);
    return status;
}

static char *out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    ALOGV("%s : keys : %s",__func__,keys);
    struct stream_out *out = (struct stream_out *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    char *str_parm = NULL;
    char value[256];
    struct str_parms *reply = str_parms_create();
    int ret;

    if(reply == NULL || query == NULL) {
        if(reply != NULL) str_parms_destroy(reply);
        if(query != NULL) str_parms_destroy(query);
        return NULL;
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value, sizeof(value));
    if (ret >= 0) {
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_FORMATS, "AUDIO_FORMAT_PCM_16_BIT");
        str_parm = str_parms_to_str(reply);
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES, value, sizeof(value));
    if (ret >= 0) {
        str_parms_add_int(reply, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES, out->req_config.sample_rate);

        if(str_parm != NULL)
            str_parms_destroy((struct str_parms *)str_parm);

        str_parm = str_parms_to_str(reply);
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_CHANNELS,
            (out->req_config.channel_mask == AUDIO_CHANNEL_OUT_MONO ? "AUDIO_CHANNEL_OUT_MONO" : "AUDIO_CHANNEL_OUT_STEREO"));

        if(str_parm != NULL)
            str_parms_destroy((struct str_parms *)str_parm);

        str_parm = str_parms_to_str(reply);
    }

    str_parms_destroy(query);
    str_parms_destroy(reply);

    ALOGV("%s : returning keyValuePair %s",__func__, str_parm);
    return str_parm;
}

static uint32_t out_get_latency(const struct audio_stream_out *stream __unused)
{
    ALOGV("out_get_latency");
    return (pcm_config_out.period_size * OUT_PERIOD_COUNT * 1000) / pcm_config_out.rate;
}

static int out_set_volume(struct audio_stream_out *stream __unused, float left __unused,
                          float right __unused)
{
     ALOGV("out_set_volume: Left:%f Right:%f", left, right);
     return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret = 0;
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t frame_size = audio_stream_out_frame_size(stream);
    int16_t *out_buffer = (int16_t *)buffer;
    unsigned int out_frames = bytes / frame_size;

    ALOGV("out_write: bytes: %zu", bytes);

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the output stream mutex - e.g.
     * executing out_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);

    if(adev->out_needs_standby) {
        do_out_standby(out);
        adev->out_needs_standby = false;
    }

    if (out->standby) {
        if(!adev->is_hfp_call_active) {
            ret = start_output_stream(out);
        } else {
            ret = -1;
        }
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = false;
    }
    pthread_mutex_unlock(&adev->lock);

//[BT SCO VoIP Call
    if(adev->in_sco_voip_call) {
        /* VoIP pcm write in celadon devices goes to bt alsa card */
        size_t frames_in = round_to_16_mult(out->pcm_config->period_size);
        size_t frames_out = round_to_16_mult(bt_out_config.period_size);
        size_t buf_size_out = bt_out_config.channels * frames_out * SAMPLE_SIZE_IN_BYTES;
        size_t buf_size_in = out->pcm_config->channels * frames_in * SAMPLE_SIZE_IN_BYTES;
        size_t buf_size_remapped = bt_out_config.channels * frames_in * SAMPLE_SIZE_IN_BYTES;
        int16_t *buf_out = (int16_t *) malloc (buf_size_out);
        int16_t *buf_in = (int16_t *) malloc (buf_size_in);
        int16_t *buf_remapped = (int16_t *) malloc (buf_size_remapped);

        if(adev->voip_out_resampler == NULL) {
            int ret = create_resampler(out->pcm_config->rate /*src rate*/, bt_out_config.rate /*dst rate*/, bt_out_config.channels/*dst channels*/,
                            RESAMPLER_QUALITY_DEFAULT, NULL, &(adev->voip_out_resampler));
            ALOGD("%s : frames_in %zu frames_out %zu",__func__, frames_in, frames_out);
            ALOGD("%s : to write bytes : %zu", __func__, bytes);
            ALOGD("%s : size_in %zu size_out %zu size_remapped %zu", __func__, buf_size_in, buf_size_out, buf_size_remapped);

            if (ret != 0) {
                adev->voip_out_resampler = NULL;
                ALOGE("%s : Failure to create resampler %d", __func__, ret);

                free(buf_in);
                free(buf_out);
                free(buf_remapped);
                goto exit;
            } else {
                ALOGD("%s : voip_out_resampler created rate : [%d -> %d]", __func__, out->pcm_config->rate, bt_out_config.rate);
            }
        }

        memset(buf_in, 0, buf_size_in);
        memset(buf_remapped, 0, buf_size_remapped);
        memset(buf_out, 0, buf_size_out);

        memcpy(buf_in, buffer, buf_size_in);

#ifdef DEBUG_PCM_DUMP
        if(sco_call_write != NULL) {
            fwrite(buf_in, 1, buf_size_in, sco_call_write);
        } else {
            ALOGD("%s : sco_call_write was NULL, no dump", __func__);
        }
#endif

        adjust_channels(buf_in, out->pcm_config->channels, buf_remapped, bt_out_config.channels, 
                                        SAMPLE_SIZE_IN_BYTES, buf_size_in);

        //ALOGV("remapping : [%d -> %d]", out->pcm_config->channels, bt_out_config.channels);

#ifdef DEBUG_PCM_DUMP
        if(sco_call_write_remapped != NULL) {
            fwrite(buf_remapped, 1, buf_size_remapped, sco_call_write_remapped);
        } else {
            ALOGD("%s : sco_call_write_remapped was NULL, no dump", __func__);
        }
#endif

        if(adev->voip_out_resampler != NULL) {
            adev->voip_out_resampler->resample_from_input(adev->voip_out_resampler, (int16_t *)buf_remapped, (size_t *)&frames_in, (int16_t *) buf_out, (size_t *)&frames_out);
            //ALOGV("%s : upsampling [%d -> %d]",__func__, out->pcm_config->rate, bt_out_config.rate);
        }

        ALOGV("%s : modified frames_in %zu frames_out %zu",__func__, frames_in, frames_out);

        buf_size_out = bt_out_config.channels * frames_out * SAMPLE_SIZE_IN_BYTES;
        bytes = out->pcm_config->channels * frames_in * SAMPLE_SIZE_IN_BYTES;

#ifdef DEBUG_PCM_DUMP
        if(sco_call_write_bt != NULL) {
            fwrite(buf_out, 1, buf_size_out, sco_call_write_bt);
        } else {
            ALOGD("%s : sco_call_write was NULL, no dump", __func__);
        }
#endif

        ret = pcm_write(out->pcm, buf_out, buf_size_out);

        free(buf_in);
        free(buf_out);
        free(buf_remapped);
//BT SCO VoIP Call]
    } else {
        /* Normal pcm out to primary card */
        ret = pcm_write(out->pcm, out_buffer, out_frames * frame_size);

#ifdef DEBUG_PCM_DUMP
        if(out_write_dump != NULL) {
            fwrite(out_buffer, 1, out_frames * frame_size, out_write_dump);
        } else {
            ALOGD("%s : out_write_dump was NULL, no dump", __func__);
        }
#endif

        if (ret == -EPIPE) {
            /* In case of underrun, don't sleep since we want to catch up asap */
            pthread_mutex_unlock(&out->lock);
            return ret;
        }
    }

    if (ret == 0) {
        out->written += out_frames;
    }

exit:
    pthread_mutex_unlock(&out->lock);

    if (ret != 0) {
        ALOGW("out_write error: %d, sleeping...", ret);
        usleep(bytes * 1000000 / audio_stream_out_frame_size(stream) /
               out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    struct stream_out *out = (struct stream_out *)stream;
    *dsp_frames = out->written;
    ALOGV("%s : dsp_frames: %d",__func__, *dsp_frames);
    return 0;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                   uint64_t *frames, struct timespec *timestamp)
{
    struct stream_out *out = (struct stream_out *)stream;
    int ret = -1;

    if (out->pcm) {
        unsigned int avail;
        if (pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
            unsigned int kernel_buffer_size = out->pcm_config->period_size * out->pcm_config->period_count;
            int64_t signed_frames = out->written - kernel_buffer_size + avail;
            if (signed_frames >= 0) {
            *frames = signed_frames;
            ret = 0;
            }
        }
    }

    return ret;
}

static int out_add_audio_effect(const struct audio_stream *stream __unused, effect_handle_t effect __unused)
{
    ALOGV("out_add_audio_effect: %p", effect);
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream __unused, effect_handle_t effect __unused)
{
    ALOGV("out_remove_audio_effect: %p", effect);
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream __unused,
                                        int64_t *timestamp __unused)
{
    ALOGV("%s",__func__);
    return -ENOSYS;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    ALOGV("%s : req_config %d",__func__,in->req_config.sample_rate);
    return in->req_config.sample_rate;
}

static int in_set_sample_rate(struct audio_stream *stream __unused, uint32_t rate __unused)
{
    ALOGV("in_set_sample_rate: %d", rate);
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
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
    ALOGV("%s : buffer_size : %zu",__func__, size);
    return size;
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    ALOGV("%s : channels %d",__func__, popcount(in->req_config.channel_mask));
    return in->req_config.channel_mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    ALOGV("%s : req_config format %d",__func__, in->req_config.format);
    return in->req_config.format;
}

static int in_set_format(struct audio_stream *stream __unused, audio_format_t format __unused)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    do_in_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);

    return 0;
}

static int in_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    int status = 0;
    unsigned int val;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    pthread_mutex_lock(&adev->lock);
    if (ret >= 0) {
        val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        if ((adev->in_device != val) && (val != 0)) {
            adev->in_device = val;
            select_devices(adev);
        }
    }
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return status;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    ALOGV("%s : keys : %s",__func__,keys);
    struct stream_in *in = (struct stream_in *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    char *str_parm = NULL;
    char value[256];
    struct str_parms *reply = str_parms_create();
    int ret;

    if(reply == NULL || query == NULL) {
        if(reply != NULL) str_parms_destroy(reply);
        if(query != NULL) str_parms_destroy(query);
        return NULL;
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value, sizeof(value));
    if (ret >= 0) {
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_FORMATS, "AUDIO_FORMAT_PCM_16_BIT");
        str_parm = str_parms_to_str(reply);
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES, value, sizeof(value));
    if (ret >= 0) {
        str_parms_add_int(reply, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES, in->req_config.sample_rate);

        if(str_parm != NULL)
            str_parms_destroy((struct str_parms *)str_parm);

        str_parm = str_parms_to_str(reply);
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_CHANNELS,
            (in->req_config.channel_mask == AUDIO_CHANNEL_IN_MONO ? "AUDIO_CHANNEL_IN_MONO" : "AUDIO_CHANNEL_IN_STEREO"));

        if(str_parm != NULL)
            str_parms_destroy((struct str_parms *)str_parm);

        str_parm = str_parms_to_str(reply);
    }

    str_parms_destroy(query);
    str_parms_destroy(reply);

    ALOGV("%s : returning keyValuePair %s",__func__, str_parm);
    return str_parm;
}

static int in_set_gain(struct audio_stream_in *stream __unused, float gain __unused)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;

    ALOGV("%s : bytes_requested : %zu", __func__, bytes);

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);

    if(adev->in_needs_standby) {
        do_in_standby(in);
        adev->in_needs_standby = false;
    }

    if (in->standby) {
        if(!adev->is_hfp_call_active) {
            ret = start_input_stream(in);
        } else {
            ret = -1;
        }
        if (ret == 0)
            in->standby = 0;
    }
    pthread_mutex_unlock(&adev->lock);

    if (ret < 0)
        goto exit;

//[BT SCO VoIP Call
    if(adev->in_sco_voip_call) {
        /* VoIP pcm read from bt alsa card */
        size_t frames_out = round_to_16_mult(in->pcm_config->period_size);
        size_t frames_in = round_to_16_mult(bt_in_config.period_size);
        size_t buf_size_out = in->pcm_config->channels * frames_out * SAMPLE_SIZE_IN_BYTES;
        size_t buf_size_in = bt_in_config.channels * frames_in * SAMPLE_SIZE_IN_BYTES;
        size_t buf_size_remapped = in->pcm_config->channels * frames_in * SAMPLE_SIZE_IN_BYTES;
        int16_t *buf_out = (int16_t *) malloc (buf_size_out);
        int16_t *buf_in = (int16_t *) malloc (buf_size_in);
        int16_t *buf_remapped = (int16_t *) malloc (buf_size_remapped);

        if(adev->voip_in_resampler == NULL) {
            int ret = create_resampler(bt_in_config.rate /*src rate*/, in->pcm_config->rate /*dst rate*/, in->pcm_config->channels/*dst channels*/,
                        RESAMPLER_QUALITY_DEFAULT, NULL, &(adev->voip_in_resampler));
            ALOGV("%s : bytes_requested : %zu", __func__, bytes);
            ALOGV("%s : frames_in %zu frames_out %zu",__func__, frames_in, frames_out);
            ALOGD("%s : size_in %zu size_out %zu size_remapped %zu", __func__, buf_size_in, buf_size_out, buf_size_remapped);
            if (ret != 0) {
                adev->voip_in_resampler = NULL;
                ALOGE("%s : Failure to create resampler %d", __func__, ret);

                free(buf_in);
                free(buf_out);
                free(buf_remapped);
                goto exit;
            } else {
                ALOGD("%s : voip_in_resampler created rate : [%d -> %d]", __func__, bt_in_config.rate, in->pcm_config->rate);
            }
        }

        memset(buf_in, 0, buf_size_in);
        memset(buf_remapped, 0, buf_size_remapped);
        memset(buf_out, 0, buf_size_out);

        ret = pcm_read(in->pcm, buf_in, buf_size_in);

#ifdef DEBUG_PCM_DUMP
        if(sco_call_read != NULL) {
            fwrite(buf_in, 1, buf_size_in, sco_call_read);
        } else {
            ALOGD("%s : sco_call_read was NULL, no dump", __func__);
        }
#endif
        adjust_channels(buf_in, bt_in_config.channels, buf_remapped, in->pcm_config->channels, 
                                        SAMPLE_SIZE_IN_BYTES, buf_size_in);

        //ALOGV("%s : remapping : [%d -> %d]", __func__, bt_in_config.channels, in->pcm_config->channels);

#ifdef DEBUG_PCM_DUMP
        if(sco_call_read_remapped != NULL) {
            fwrite(buf_remapped, 1, buf_size_remapped, sco_call_read_remapped);
        } else {
            ALOGD("%s : sco_call_read_remapped was NULL, no dump", __func__);
        }
#endif

        if(adev->voip_in_resampler != NULL) {
            adev->voip_in_resampler->resample_from_input(adev->voip_in_resampler, (int16_t *)buf_remapped, (size_t *)&frames_in, (int16_t *) buf_out, (size_t *)&frames_out);
            //ALOGV("%s : upsampling [%d -> %d]",__func__, bt_in_config.rate, in->pcm_config->rate);
        }

        ALOGV("%s : modified frames_in %zu frames_out %zu",__func__, frames_in, frames_out);

        buf_size_out = in->pcm_config->channels * frames_out * SAMPLE_SIZE_IN_BYTES;
        bytes = buf_size_out;

#ifdef DEBUG_PCM_DUMP
        if(sco_call_read_bt != NULL) {
            fwrite(buf_out, 1, buf_size_out, sco_call_read_bt);
        } else {
            ALOGD("%s : sco_call_read_bt was NULL, no dump", __func__);
        }
#endif

        memcpy(buffer, buf_out, buf_size_out);

        free(buf_in);
        free(buf_out);
        free(buf_remapped);
//BT SCO VoIP Call]
    } else {
        /* pcm read for primary card */
        ret = pcm_read(in->pcm, buffer, bytes);

#ifdef DEBUG_PCM_DUMP
        if(in_read_dump != NULL) {
            fwrite(buffer, 1, bytes, in_read_dump);
        } else {
            ALOGD("%s : in_read_dump was NULL, no dump", __func__);
        }
#endif
    }
    if (ret > 0)
        ret = 0;

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_in_frame_size(stream) /
               in_get_sample_rate(&stream->common));

    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream __unused)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream __unused,
                               effect_handle_t effect __unused)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream __unused,
                                  effect_handle_t effect __unused)
{
    return 0;
}


static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle __unused,
                                   audio_devices_t devices __unused,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
    ALOGD("%s : requested config : [rate %d format %d channels %d flags %#x]",__func__,
        config->sample_rate, config->format, popcount(config->channel_mask), flags);

    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    struct pcm_params *params;

    int ret;

    params = pcm_params_get(PCM_CARD_DEFAULT, PCM_DEVICE, PCM_OUT);

    if(params != NULL) {
       adev->card = PCM_CARD_DEFAULT;
    } else {
        adev->card = get_pcm_card("Dummy");
        params = pcm_params_get(adev->card, PCM_DEVICE, PCM_OUT);
    }

    ALOGI("PCM playback card selected = %d, \n", adev->card);

    if (!params)
        return -ENOSYS;

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out) {
        free(params);
        return -ENOMEM;
    }

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    out->pcm_config = &pcm_config_out;

    out->written = 0;

// VTS : Device doesn't support mono channel or sample_rate other than 48000
//       make a copy of requested config to feed it back if requested.
    memcpy(&out->req_config, config, sizeof(struct audio_config));

    out->dev = adev;
    out->standby = true;
    out->unavailable = false;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;

    free(params);

    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev __unused,
                                     struct audio_stream_out *stream)
{
    out_standby(&stream->common);
    free(stream);
}

//called with adev lock
static void stop_existing_output_input(struct audio_device *adev){
    ALOGD("%s during call scenario", __func__);
    adev->in_needs_standby = true;
    adev->out_needs_standby = true;
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    ALOGD("%s : kvpairs: %s", __func__, kvpairs);

    struct audio_device * adev = (struct audio_device *)dev;
    char value[32];
    int ret;
    struct str_parms *parms;

    parms = str_parms_create_str(kvpairs);

    if(parms == NULL) {
        return 0;
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_HFP_ENABLE, value, sizeof(value));
    if (ret >= 0) {
        pthread_mutex_lock(&adev->lock);
        if (strcmp(value, "true") == 0){
            stop_existing_output_input(adev);
            adev->is_hfp_call_active = true;
        } else {
            adev->is_hfp_call_active = false;
        }
        pthread_mutex_unlock(&adev->lock);
    }

//[BT SCO VoIP Call
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_BT_SCO, value, sizeof(value));
    if (ret >= 0) {
        pthread_mutex_lock(&adev->lock);
        if (strcmp(value, "on") == 0){
            adev->in_sco_voip_call = true;
            stop_existing_output_input(adev);
        } else {
            adev->in_sco_voip_call = false;
            stop_existing_output_input(adev);

            release_resampler(adev->voip_in_resampler);
            adev->voip_in_resampler = NULL;
            release_resampler(adev->voip_out_resampler);
            adev->voip_out_resampler = NULL;
        }
        pthread_mutex_unlock(&adev->lock);
    }
//BT SCO VoIP Call]

    str_parms_destroy(parms);
    return 0;
}

static char * adev_get_parameters(const struct audio_hw_device *dev __unused,
                                  const char *keys)
{
    ALOGV("%s : keys : %s",__func__,keys);
    struct str_parms *query = str_parms_create_str(keys);
    char value[256];
    int ret;

    if(query == NULL) {
        return NULL;
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_HW_AV_SYNC, value, sizeof(value));
    if (ret >= 0) {
        str_parms_destroy(query);
        return NULL;
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_KEY_TTY_MODE, value, sizeof(value));
    if(ret >= 0) {
        ALOGE("%s : no support of TTY",__func__);
        str_parms_destroy(query);
        return NULL;
    }

    str_parms_destroy(query);
    return strdup(keys);
}

static int adev_init_check(const struct audio_hw_device *dev __unused)
{
     ALOGV("adev_init_check");
     return 0;
}

//Supported vol range [0:1], return OK for inrange volume request
static int adev_set_voice_volume(struct audio_hw_device *dev __unused, float volume)
{
    ALOGV("adev_set_voice_volume: %f : This platform provides no such handling", volume);

    int32_t ret = 0;

    if(volume < 0.0f){
        ret = -EINVAL;
    }

    return ret;
}

static int adev_set_master_volume(struct audio_hw_device *dev __unused, float volume __unused)
{
    ALOGV("adev_set_master_volume: %f", volume);
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev __unused, float *volume __unused)
{
    ALOGV("adev_get_master_volume:");
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev __unused, bool muted __unused)
{
    ALOGV("adev_set_master_mute: %d", muted);
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev __unused, bool *muted __unused)
{
    ALOGV("adev_get_master_mute: %d", *muted);
    return -ENOSYS;
}
static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    ALOGD("%s : mode : %d", __func__, mode);
    struct audio_device *adev = (struct audio_device *)dev;

    pthread_mutex_lock(&adev->lock);
    stop_existing_output_input(adev);
    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    ALOGV("adev_set_mic_mute: %d",state);
    struct audio_device *adev = (struct audio_device *)dev;
    adev->mic_mute = state;
    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    ALOGV("adev_get_mic_mute");
    struct audio_device *adev = (struct audio_device *)dev;
    *state = adev->mic_mute;
    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev __unused,
                                         const struct audio_config *config)
{
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (pcm_config_in.period_size * config->sample_rate) / pcm_config_in.rate;
    size = ((size + 15) / 16) * 16;

    return (size * popcount(config->channel_mask) *
                audio_bytes_per_sample(config->format));
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle __unused,
                                  audio_devices_t devices __unused,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags __unused,
                                  const char *address __unused,
                                  audio_source_t source __unused)

{
    ALOGD("%s : requested config : [rate %d format %d channels %d flags %#x]",__func__,
        config->sample_rate, config->format, popcount(config->channel_mask), flags);

    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    struct pcm_params *params;

    *stream_in = NULL;

    params = pcm_params_get(PCM_CARD_DEFAULT, PCM_DEVICE, PCM_IN);

    if(params != NULL) {
        adev->cardc = PCM_CARD_DEFAULT;
    } else {
        adev->cardc = get_pcm_card("Dummy");
        params = pcm_params_get(adev->cardc, PCM_DEVICE, PCM_IN);
    }

    ALOGI("PCM capture card selected = %d, \n", adev->card);

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (!in) {
        free(params);
        return -ENOMEM;
    }

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->dev = adev;
    in->standby = true;

    in->pcm_config = &pcm_config_in; /* default PCM config */

// VTS : Device doesn't support mono channel or sample_rate other than 48000
//       make a copy of requested config to feed it back if requested.
    memcpy(&in->req_config, config, sizeof(struct audio_config));

    *stream_in = &in->stream;

    free(params);
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev __unused,
                                   struct audio_stream_in *stream)
{
    ALOGV("adev_close_input_stream...");

    in_standby(&stream->common);
    free(stream);
}

static int adev_dump(const audio_hw_device_t *device __unused, int fd __unused)
{
    ALOGV("adev_dump");
    return 0;
}

static int adev_get_microphones(const audio_hw_device_t *device __unused, struct audio_microphone_characteristic_t *mic_array, size_t *actual_mics)
{
    ALOGV("%s",__func__);
    int32_t ret = 0;
    *actual_mics = 1;
    memset(&mic_array[0], 0, sizeof(mic_array[0]));

    return ret;
}

static int adev_close(hw_device_t *device)
{
    ALOGV("adev_close");

    struct audio_device *adev = (struct audio_device *)device;

    audio_route_free(adev->ar);

#ifdef DEBUG_PCM_DUMP
    if(sco_call_write != NULL) {
        fclose(sco_call_write);
    }
    if(sco_call_write_remapped != NULL) {
        fclose(sco_call_write_remapped);
    }
    if(sco_call_write_bt != NULL) {
        fclose(sco_call_write_bt);
    }
    if(sco_call_read != NULL) {
        fclose(sco_call_read);
    }
    if(sco_call_read_remapped != NULL) {
        fclose(sco_call_read_remapped);
    }
    if(sco_call_read_bt != NULL) {
        fclose(sco_call_read_bt);
    }
    if(out_write_dump != NULL) {
        fclose(out_write_dump);
    }
    if(in_read_dump != NULL) {
        fclose(in_read_dump);
    }
#endif

    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    ALOGV("adev_open: %s", name);

    struct audio_device *adev;
    int card = 0;
    char mixer_path[PATH_MAX];

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;
    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.get_master_volume = adev_get_master_volume;
    adev->hw_device.set_master_mute = adev_set_master_mute;
    adev->hw_device.get_master_mute = adev_get_master_mute;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;
    adev->hw_device.get_microphones = adev_get_microphones;

    sprintf(mixer_path, "/vendor/etc/mixer_paths_%d.xml", card);
    adev->ar = audio_route_init(card, mixer_path);
    if (!adev->ar) {
        ALOGE("%s: Failed to init audio route controls for card %d, aborting.",
            __func__, card);
        goto error;
    }
    
    adev->out_device = AUDIO_DEVICE_OUT_SPEAKER;
    adev->in_device = AUDIO_DEVICE_IN_BUILTIN_MIC & ~AUDIO_DEVICE_BIT_IN;

    *device = &adev->hw_device.common;

// CLK target codec only works with sample_rate 48000, identify the target and update default pcm_config.rate if needed.
    char product[PROPERTY_VALUE_MAX] = "cel_kbl";
    if(property_get("ro.hardware", product, NULL) <= 0) {
        ALOGE("%s : failed to read ro.hardware", __func__);
    } else {
        if(strcmp(product, "clk") == 0) {
            pcm_config_in.rate = 48000;
        }
    }

//Update period_size based on sample rate and period_ms
    size_t size = (pcm_config_in.rate * IN_PERIOD_MS * SAMPLE_SIZE_IN_BYTES_STEREO) / 1000;
    pcm_config_in.period_size = size;

    ALOGI("%s : will use input [rate : period] as [%d : %u] for %s variants", __func__, pcm_config_in.rate, pcm_config_in.period_size, product);

//[BT SCO VoIP Call
    update_bt_card(adev);

    adev->in_sco_voip_call = false;
    adev->is_hfp_call_active = false;
    adev->voip_in_resampler = NULL;
    adev->voip_out_resampler = NULL;
//BT SCO VoIP Call]

    adev->in_needs_standby = false;
    adev->out_needs_standby = false;

#ifdef DEBUG_PCM_DUMP
    sco_call_write = fopen("/vendor/dump/sco_call_write.pcm", "a");
    sco_call_write_remapped = fopen("/vendor/dump/sco_call_write_remapped.pcm", "a");
    sco_call_write_bt = fopen("/vendor/dump/sco_call_write_bt.pcm", "a");
    sco_call_read = fopen("/vendor/dump/sco_call_read.pcm", "a");
    sco_call_read_remapped = fopen("/vendor/dump/sco_call_read_remapped.pcm", "a");
    sco_call_read_bt = fopen("/vendor/dump/sco_call_read_bt.pcm", "a");
    out_write_dump = fopen("/vendor/dump/out_write_dump.pcm", "a");
    in_read_dump = fopen("/vendor/dump/in_read_dump.pcm", "a");

    if(sco_call_write == NULL || sco_call_write_remapped == NULL || sco_call_write_bt == NULL || sco_call_read == NULL || 
            sco_call_read_bt == NULL || sco_call_read_remapped == NULL || out_write_dump == NULL || in_read_dump == NULL)
        ALOGD("%s : failed to open dump files",__func__);
    else
        ALOGD("%s : success in opening dump files",__func__);
#endif

    return 0;

 error:
    free(adev);
    return -ENODEV;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Android IA minimal HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
