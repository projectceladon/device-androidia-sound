/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef AUDIO_HAL_CONFIG_H
#define AUDIO_HAL_CONFIG_H

#if defined(__cplusplus)
extern "C" {
#endif

struct mixer_path_config {
    char* card_name;
    char* mixer_path;
};

struct stream_config {
    char *address;
    char *card_name;
    int device_id;
    bool mmap;
    bool pcm_dump;
    int additional_out_delay;
    struct pcm_config pcm_config;
    struct mixer_path_config *mixer_path;
};

void* audio_hal_config_init(void);
void audio_hal_config_free(void* handle);

int audio_hal_config_load_from_xml(void* handle, const char *xml_path);
int audio_hal_config_add(void* handle, struct stream_config *item, bool playback);
int audio_hal_config_delete(void* handle, const char* address, bool playback);

struct stream_config *audio_hal_config_get(void* handle, const char* address, bool playback);

#if defined(__cplusplus)
}  /* extern "C" */
#endif

#endif

