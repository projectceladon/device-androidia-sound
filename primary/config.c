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


#define LOG_TAG "audio_hal_config"
/*#define LOG_NDEBUG 0*/

#include <errno.h>
#include <expat.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <cutils/properties.h>
#include <cutils/hashmap.h>
#include <tinyalsa/asoundlib.h>

#include <log/log.h>
#include "config.h"

#define TAG_STREAM "Stream"
#define TAG_PCM "Pcm"
#define TAG_MIXER "Mixer"

#define ATTR_ADDRESS "Address"
#define ATTR_DIRECTION "Direction"
#define ATTR_CARD "Card"
#define ATTR_DEVICE "Device"
#define ATTR_MMAP "Mmap"
#define ATTR_DATADUMP "DataDump"
#define ATTR_SAMPLE_RATE "SampleRate"
#define ATTR_FORMAT "Format"
#define ATTR_CHANNELS "Channels"
#define ATTR_PERIOD_SIZE "PeriodSize"
#define ATTR_PERIOD_COUNT "PeriodCount"
#define ATTR_START_THRESHOLD "StartThreshhold"
#define ATTR_STOP_THRESHOLD "StopThreshold"
#define ATTR_ADDITIONAL_OUT_DELAY "AdditionalOutputDeviceDelay"
#define ATTR_AVAIL_MIN "AvailMin"
#define ATTR_MIXER_PATH "MixerPath"

#define HASHMAP_INIT 5
#define BUF_SIZE 1024
#define ADDRESS_LENGTH 50
#define CARD_NAME_LENGTH 50
#define MIXER_PATH_LENGTH 100

#define DIRECTION_PLAYBACK "playback"
#define DIRECTION_CAPTURE "capture"

enum Level {
    ROOT,
    STREAM,
};

struct config_parse_state {
    struct audio_hal_config *config;

    enum Level level;
    void* context;  /* temp solution, maybe request improvement */
};

struct audio_hal_config {
    Hashmap *playback;
    Hashmap *capture;
};

/* copied from libcutils/str_parms.c */
static bool str_eq(void *key_a, void *key_b) {
     return !strcmp((const char *)key_a, (const char *)key_b);
}
static int str_hash_fn(void *str) {
    uint32_t hash = 5381;
    char *p;
    for (p = str; p && *p; p++) {
         hash = ((hash << 5) + hash) + *p;
    }
    return (int)hash;
}

static bool print_stream_config(void *key __unused, void *value, void *context __unused) {
    struct stream_config* sc = (struct stream_config*)value;
    int bit = 0;

    switch (sc->pcm_config.format) {
        case PCM_FORMAT_S16_LE:
            bit = 16;
            break;
        case PCM_FORMAT_S24_LE:
            bit = 24;
            break;
        case PCM_FORMAT_S32_LE:
            bit = 32;
        default:
            break;
    }

    ALOGI("address=%s, card=%s, device=%d", sc->address, sc->card_name, sc->device_id);
    ALOGI("rate=%d, channel=%d, bit=%d", sc->pcm_config.rate, sc->pcm_config.channels, bit);
    ALOGI("period_count=%d, period_size=%d, start_threshold=%d, stop_threshold=%d, avail_min=%d",
        sc->pcm_config.period_size, sc->pcm_config.period_count,
        sc->pcm_config.start_threshold, sc->pcm_config.stop_threshold, sc->pcm_config.avail_min);
    if (sc->mixer_path) {
        ALOGI("mixer card=%s, mixer_path=%s", sc->mixer_path->card_name, sc->mixer_path->mixer_path);  
    }

    return true;
}

static void parse_mixer_tag(struct stream_config* config, const XML_Char **attr)
{
    const XML_Char *attr_value = NULL;
    unsigned int i;
    char *end = NULL;
    long value = -1;
    char *card_name = NULL, *mixer_path = NULL;

    for (i = 0; attr[i]; i += 2) {
        attr_value = attr[i + 1];
        if (attr_value == NULL || strlen(attr_value) == 0) {
            ALOGE("%s: attribute value is NULL for mixer", __func__);
            free(mixer_path);
            free(card_name);
            return;
        }
        ALOGV("%s,%s: key=%s, value=%s", __func__, config->address, attr[i], attr[i + 1]);

        if (!strcmp(attr[i], ATTR_CARD)) {
            card_name = (char *)calloc(CARD_NAME_LENGTH, sizeof(char));
            if (!card_name) {
                ALOGE("%s,%s: alloc fail, no memory, %s=%s", __func__, config->address, attr[i], attr[i + 1]);
                if (mixer_path) {
                    free(mixer_path);
                    mixer_path = NULL;
                    free(card_name);
                }
                return;
            }
            memcpy(card_name, attr_value, CARD_NAME_LENGTH);
        } else if (!strcmp(attr[i], ATTR_MIXER_PATH)) {
             mixer_path = (char *)calloc(MIXER_PATH_LENGTH, sizeof(char));
            if (!mixer_path) {
                ALOGE("%s,%s: alloc fail, no memory, %s=%s", __func__, config->address, attr[i], attr[i + 1]);
                if (card_name) {
                    free(card_name);
                    card_name = NULL;
                }
                free(mixer_path);
                return;
            }
            memcpy(mixer_path, attr_value, MIXER_PATH_LENGTH);
        }
    }

    if (!card_name || !mixer_path) {
        ALOGE("%s,%s: incorrect mixer path setting", __func__, config->address);
        if (mixer_path) {
            free(mixer_path);
            mixer_path = NULL;
        }
        if (card_name) {
            free(card_name);
            card_name = NULL;
        }
        return;
    }

    config->mixer_path = (struct mixer_path_config *)calloc(MIXER_PATH_LENGTH,
            sizeof(struct mixer_path_config));
    if (!config->mixer_path) {
        ALOGE("%s,%s: alloc mixer path fail, no memory", __func__, config->address);
        free(card_name);
        free(mixer_path);
        card_name = NULL;
        mixer_path = NULL;
        return;
    }

    config->mixer_path->card_name = card_name;
    config->mixer_path->mixer_path = mixer_path;

    return;
}

static void parse_pcm_tag(struct stream_config* config, const XML_Char **attr)
{
    const XML_Char *attr_value = NULL;
    unsigned int i;
    char *end = NULL;
    long value = -1;
    char *card_name = NULL;
    int device = -1, format = -1, channels = -1, rate = -1;
    int period_size = -1, period_count = -1;
    int start_threshold = -1, stop_threshold = -1, avail_min = -1;
    int additional_out_delay = 0;

    for (i = 0; attr[i]; i += 2) {
        attr_value = attr[i + 1];
        if (attr_value == NULL || strlen(attr_value) == 0) {
            ALOGE("%s,%s: attribute value is NULL for pcm config", __func__, config->address);
            if (card_name) {
                free(card_name);
                card_name = NULL;
            }
            return;
        }
        ALOGV("%s,%s: key=%s, value=%s", __func__, config->address, attr[i], attr[i + 1]);

        if (!strcmp(attr[i], ATTR_CARD)) {
            card_name = (char *)calloc(CARD_NAME_LENGTH, sizeof(char));
            if (!card_name) {
                ALOGE("%s,%s: alloc fail, no memory, %s=%s", __func__, config->address, attr[i], attr[i + 1]);
                free(card_name);
                return;
            }
            memcpy(card_name, attr_value, CARD_NAME_LENGTH);
            continue;
        } 

        value = strtol((char *)attr_value, &end, 0);
        if (end == (char *)attr_value) {
            ALOGE("%s,%s: not correct setting for pcm config, %s", __func__, config->address, attr[i]);
            if (card_name) {
                free(card_name);
                card_name = NULL;
            }
	    return;
        }

        if (!strcmp(attr[i], ATTR_DEVICE)) {
            device = (value >= 0 ? value : -1);
        } else if (!strcmp(attr[i], ATTR_SAMPLE_RATE)) {
            if (value != 8000 && value != 16000 && value != 32000
                && value != 44100 && value != 48000) {
                ALOGE("%s,%s: not supported pcm sample rate, %ld", __func__, config->address, value);
                if (card_name) {
                    free(card_name);
                    card_name = NULL;
                }
                return;
	    }
            rate = value;
        } else if (!strcmp(attr[i], ATTR_FORMAT)) {
            switch (value)
            {
                case 16:
                    format = PCM_FORMAT_S16_LE;
                    break;
                case 24:
                    format = PCM_FORMAT_S24_LE;
                    break;
                case 32:
                    format = PCM_FORMAT_S32_LE;
                    break;
                default:
                    ALOGE("%s,%s: not supported pcm format, %ld", __func__, config->address, value);
                    if (card_name) {
                        free(card_name);
                        card_name = NULL;
                    }
                    return;
            }
	} else if (!strcmp(attr[i], ATTR_CHANNELS)) {
            if (value != 1 && value != 2 && value != 4 && value != 6
                && value != 8 && value != 12) {
                ALOGE("%s,%s: not supported pcm channels, %ld", __func__, config->address, value);
                if (card_name) {
                    free(card_name);
                    card_name = NULL;
                }
                return;
            }
            channels = value; 
	} else if (!strcmp(attr[i], ATTR_PERIOD_SIZE)) {
            period_size = value;
	} else if (!strcmp(attr[i], ATTR_PERIOD_COUNT)) {
            period_count = value;
	} else if (!strcmp(attr[i], ATTR_START_THRESHOLD)) {
            start_threshold = value;
	} else if (!strcmp(attr[i], ATTR_STOP_THRESHOLD)) {
            stop_threshold = value;
	} else if (!strcmp(attr[i], ATTR_AVAIL_MIN)) {
            avail_min = value;
	} else if (!strcmp(attr[i], ATTR_ADDITIONAL_OUT_DELAY)) {
            additional_out_delay = value;
	}

        end = NULL;
        value = -1;
    }

    if (!card_name || device < 0 || format < 0 || channels < 0 || rate < 0) {
        ALOGE("%s,%s: incorrect card, device, format, channels or rate", __func__, config->address);
        if (card_name) {
            free(card_name);
            card_name = NULL;
        }
        return;
    }

    config->card_name = card_name;
    config->device_id = device;
    config->additional_out_delay = additional_out_delay;
    config->pcm_config.format = format; 
    config->pcm_config.channels = channels;
    config->pcm_config.rate = rate;
    config->pcm_config.period_size = (period_size >= 0 ? period_size : 0);
    config->pcm_config.period_count = (period_count >= 0 ? period_count : 0);
    config->pcm_config.start_threshold = (start_threshold >= 0 ? start_threshold : 0);
    config->pcm_config.stop_threshold = (stop_threshold >= 0 ? stop_threshold : 0); 
    config->pcm_config.avail_min = avail_min;
}

static struct stream_config *parse_stream_tag(struct audio_hal_config *config, const XML_Char **attr) 
{
    const XML_Char *attr_value = NULL;
    struct stream_config* sc = NULL;
    char* address = NULL;
    Hashmap** hashmap = NULL;
    bool isMmap = false, isDataDump = false;
    unsigned int i;

    for (i = 0; attr[i]; i += 2) {
        attr_value = attr[i + 1];
        if (attr_value == NULL || strlen(attr_value) == 0) {
            ALOGE("%s: attribute value is NULL for stream", __func__);
            free((void *)attr_value);
            return NULL;
        }

        ALOGV("%s, key=%s, value=%s", __func__, attr[i], attr[i + 1]);

        if (strcmp(attr[i], ATTR_ADDRESS) == 0) {
            address = (char *)calloc(ADDRESS_LENGTH, sizeof(char));
            if (!address) {
                ALOGE("%s: address alloc fail, no memory", __func__);
                free((void *)attr_value);
                return NULL;	
	    }

            memcpy(address, attr_value, ADDRESS_LENGTH);
        } else if (strcmp(attr[i], ATTR_DIRECTION) == 0) {
            if (!strcmp((char *)attr_value, DIRECTION_PLAYBACK)) {

                hashmap = &config->playback;
            } else if (!strcmp((char *)attr_value, DIRECTION_CAPTURE)  ) {
                hashmap = &config->capture;
            } else {
                ALOGE("%s: not correct direction setting for stream", __func__);
                if (!address) {
                    free(address);
                }
                free((void *)attr_value);
                return NULL;
            }
        } else if (!strcmp(attr[i], ATTR_MMAP) || !strcmp(attr[i], ATTR_DATADUMP) ) {
           if (strcmp((char *)attr_value, "true") || strcmp((char *)attr_value, "True")) {
               if (!strcmp(attr[i], "Mmap")) {
                   isMmap = true;
               } else {
                   isDataDump = true;
               }
           }
        }
    }

    if (!address || !hashmap) {
        ALOGE("%s: incorrect stream setting", __func__);

	if (!address) {
            free(address);
        }

        free((void *)attr_value);
        return NULL;
    }

    sc = (struct stream_config *)calloc(1, sizeof(struct stream_config));
    if (!sc) {
        free((void *)attr_value);
        ALOGE("%s: no memory to alloc stream_config", __func__);
        return NULL;
    }

    sc->address = address;
    sc->card_name = NULL;
    sc->device_id = -1;
    sc->mmap = isMmap;
    sc->pcm_dump = isDataDump;
    hashmapPut(*hashmap, sc->address, sc);

    return sc;
}

static void start_tag(void *data, const XML_Char *tag_name,
                      const XML_Char **attr)
{
    struct config_parse_state *state = data;

    if (!strcmp(tag_name, TAG_STREAM)) {
        struct stream_config *sc = parse_stream_tag(state->config, attr);
        if (sc == NULL) {
            ALOGE("%s: parse stream tag wrong", __func__);
            return;
	}

        state->level = STREAM;
        state->context = (void*)sc;
    } else if (!strcmp(tag_name, TAG_PCM)) {
        struct stream_config *sc = NULL;

        if (state->level != STREAM) {
            ALOGE("%s: abnormal parse state for pcm config", __func__);
            return;
        }

        sc = state->context;
        if (sc->card_name != NULL || sc->device_id > 0) {
            ALOGE("%s: pcm config already get, %s", __func__, sc->address);
            return;
	}

        parse_pcm_tag(sc, attr);
    } else if (!strcmp(tag_name, TAG_MIXER)) {
        struct stream_config *sc = NULL;

        if (state->level != STREAM) {
            ALOGE("%s: abnormal parse state for mixer config", __func__);
            return;
        }

        sc = state->context;
        parse_mixer_tag(sc, attr); 
    }
}

static void end_tag(void *data, const XML_Char *tag_name)
{
    struct config_parse_state *state = data;

    if (!strcmp(tag_name, TAG_STREAM)) {
        state->level = ROOT;
        state->context = NULL;
    }
}

void* audio_hal_config_init(void)
{
    struct audio_hal_config *hal_config;

    hal_config = calloc(1, sizeof(struct audio_hal_config));
    if (!hal_config) {
        ALOGE("%s: memory allocate fail", __func__);
        return NULL;
    }

    hal_config->playback = hashmapCreate(HASHMAP_INIT, str_hash_fn, str_eq);
    hal_config->capture  = hashmapCreate(HASHMAP_INIT, str_hash_fn, str_eq);

    return (void*)hal_config;
}

int audio_hal_config_load_from_xml(void* handle, const char *xml_path)
{
    struct config_parse_state state;
    XML_Parser parser;
    FILE *file;
    int bytes_read;
    void *buf;
    struct audio_hal_config *hal_config = (struct audio_hal_config *)handle;

    if (!hal_config) {
        ALOGE("%s: invalid handle", __func__);
        return -1;
    }

    if (!xml_path) {
        ALOGE("%s: invalid xml path", __func__);
        return -1;
    }

    file = fopen(xml_path, "r");
    if (!file) {
        ALOGE("Failed to open %s: %s", xml_path, strerror(errno));
        return -1;
    }

    parser = XML_ParserCreate(NULL);
    if (!parser) {
        ALOGE("Failed to create XML parser");
	fclose(file);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.config = hal_config;
    state.level = ROOT;
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, start_tag, end_tag);

    for (;;) {
        buf = XML_GetBuffer(parser, BUF_SIZE);
        if (buf == NULL) {
            XML_ParserFree(parser);
            fclose(file);
            return -1;
	}

        bytes_read = fread(buf, 1, BUF_SIZE, file);
        if (bytes_read < 0) {
            XML_ParserFree(parser);
            fclose(file);
            return -1;
	}

        if (XML_ParseBuffer(parser, bytes_read,
                            bytes_read == 0) == XML_STATUS_ERROR) {
            ALOGE("Error parse audio hal config xml (%s)", xml_path);

            XML_ParserFree(parser);
            fclose(file);
            return -1;
        }

        if (bytes_read == 0) {
            break;
        }
    }

    XML_ParserFree(parser);
    fclose(file);

    char debug[PROPERTY_VALUE_MAX] = "false";
    property_get("vendor.audio_config.debug", debug, NULL);
    if (!strcmp(debug,"true")) {
        ALOGI("Playback stream config:");
        hashmapForEach(hal_config->playback, print_stream_config, NULL);

	ALOGI("Capture stream config:");
        hashmapForEach(hal_config->capture, print_stream_config, NULL);
    }

    return 0;
}

static bool free_stream_config(void *key, void *value, void *context)
{
    Hashmap *map = (Hashmap*)context;
    struct stream_config* sc = (struct stream_config*)value;

    ALOGI("%s: %s", __func__, sc->address);

    hashmapRemove(map, key);

    if (sc->mixer_path) {
        free(sc->mixer_path->card_name);
        free(sc->mixer_path->mixer_path);
    }

    free(sc->card_name);
    free(sc->address);
    free(sc);

    return true;
}

void audio_hal_config_free(void* handle)
{
    struct audio_hal_config* config = (struct audio_hal_config*)handle;

    if (config != NULL) {
        hashmapForEach(config->playback, free_stream_config, config->playback);
        hashmapFree(config->playback);

        hashmapForEach(config->capture, free_stream_config, config->capture);
        hashmapFree(config->capture);

        free(config);
        config = NULL;
    }
}

struct stream_config *audio_hal_config_get(void* handle, const char* address, bool playback)
{
    struct audio_hal_config* config = NULL;
    Hashmap *map = NULL;

    if (handle == NULL || address == NULL) {
        ALOGE("%s: incorrect argument", __func__);
        return 0;
    }

    config = (struct audio_hal_config*)handle;
    map = playback ? config->playback : config->capture;

    return hashmapGet(map, (void*)address);
}

int audio_hal_config_add(void* handle, struct stream_config *item, bool playback)
{
    struct audio_hal_config* config = NULL;
    struct stream_config *sc = NULL;
    Hashmap *map = NULL;

    if (!handle || !item->address || !item->card_name) {
        ALOGE("%s: incorrect argument", __func__);
        return -1;
    }

    config = (struct audio_hal_config*)handle;
    map = playback ? config->playback : config->capture;

    sc = hashmapGet(map, (void*)item->address);
    if (sc) {
        ALOGE("%s: config item already exist", __func__);
        return 0;
    }

    sc = (struct stream_config *)calloc(1, sizeof(struct stream_config));
    if (!sc) {
        ALOGE("%s: no memory to alloc stream_config", __func__);
        return -1;
    }
    sc->address = (char *)calloc(1, strlen(item->address)+1);
    if (!sc->address) {
       ALOGE("%s: no memory to alloc stream address", __func__);
       free(sc);
       return -1;
    }
    sc->card_name = (char *)calloc(1, strlen(item->card_name)+1);
    if (!sc->card_name) {
       ALOGE("%s: no memory to alloc stream card name", __func__);
       free(sc->address);
       free(sc);
       return -1;
    }

    memcpy(sc, item, sizeof(struct stream_config));
    memcpy(sc->address, item->address, strlen(item->address));
    memcpy(sc->card_name, item->card_name, strlen(item->card_name));

    if (item->mixer_path) {
        sc->mixer_path = (struct mixer_path_config *)calloc(1, sizeof(struct mixer_path_config));
        if (!sc->mixer_path) {
            ALOGE("%s: no memory to alloc mixer", __func__);
            free(sc->card_name);
            free(sc->address);
            free(sc);
            return -1;
        }
        sc->mixer_path->card_name = (char *)calloc(1, strlen(item->mixer_path->card_name)+1);
        if (!sc->mixer_path->card_name) {
            ALOGE("%s: no memory to alloc mixer card name", __func__);
            free(sc->mixer_path);
            free(sc->card_name);
            free(sc->address);
            free(sc);
            return -1;
        }
        sc->mixer_path->mixer_path = (char *)calloc(1, strlen(item->mixer_path->mixer_path)+1);
        if (!sc->mixer_path->mixer_path) {
            ALOGE("%s: no memory to alloc mixer path", __func__);
            free(sc->mixer_path->card_name);
            free(sc->mixer_path);
            free(sc->card_name);
            free(sc->address);
            free(sc);
            return -1;
        }
        memcpy(sc->mixer_path->card_name, item->mixer_path->card_name, strlen(item->mixer_path->card_name));
        memcpy(sc->mixer_path->mixer_path, item->mixer_path->mixer_path, strlen(item->mixer_path->mixer_path));
    }
    strcat(sc->address, "\0");
    hashmapPut(map, sc->address, sc);

    return 0;
}

int audio_hal_config_delete(void* handle, const char* address, bool playback)
{
    struct audio_hal_config* config = NULL;
    struct stream_config *item = NULL;
    Hashmap *map = NULL;

    if (handle == NULL || address == NULL) {
        ALOGE("%s: incorrect argument", __func__);
        return -1;
    }

    config = (struct audio_hal_config*)handle;
    map = playback ? config->playback : config->capture;

    item = hashmapGet(map, (void*)address);
    if (item != NULL) {
        hashmapRemove(map, (void*)address);
        return -1;
    }

    return 0;
}
