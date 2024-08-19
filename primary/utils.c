//Copyright (C) 2024 Intel Corporation
//SPDX-License-Identifier: Apache-2.0

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

