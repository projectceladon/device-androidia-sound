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


#define LOG_TAG "audio_hal_utils"
/*#define LOG_NDEBUG 0*/

#include <errno.h>
#include <expat.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <linux/ioctl.h>
#include <sound/asound.h>

#include <log/log.h>
#include "utils.h"

bool check_virito_card(int card)
{
    int snd_card;
    char id_filepath[PATH_MAX] = {0};
    struct snd_ctl_card_info card_info;

    snprintf(id_filepath, sizeof(id_filepath),"/dev/snd/controlC%d", card);
    snd_card = open(id_filepath, O_RDONLY);
    if (snd_card < 0 || ioctl(snd_card, SNDRV_CTL_IOCTL_CARD_INFO, &card_info) < 0) {
        ALOGE("%s: Get info from card%d failed", __func__, card);
	return false;
    }

    if (!strcmp((char*)card_info.driver, "virtio-snd")) {
        close(snd_card);

        return true;
    }
    close(snd_card);

    return false;
}

