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
