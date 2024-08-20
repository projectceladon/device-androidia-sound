// Copyright (C) 2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#ifndef AUDIO_HAL_CONFIG_H
#define AUDIO_HAL_CONFIG_H

#if defined(__cplusplus)
extern "C" {
#endif

struct mixer_path_config {
  char *card_name;
  char *mixer_path;
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

void *audio_hal_config_init(void);
void audio_hal_config_free(void *handle);

int audio_hal_config_load_from_xml(void *handle, const char *xml_path);
int audio_hal_config_add(void *handle, struct stream_config *item,
                         bool playback);
int audio_hal_config_delete(void *handle, const char *address, bool playback);

struct stream_config *audio_hal_config_get(void *handle, const char *address,
                                           bool playback);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif
