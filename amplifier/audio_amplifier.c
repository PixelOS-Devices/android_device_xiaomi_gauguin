/*
 * SPDX-FileCopyrightText: 2020 AngeloGioacchino Del Regno <kholk11@gmail.com>
 * SPDX-FileCopyrightText: 2020-2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "audio_amplifier"
//#define LOG_NDEBUG 0

#include <dlfcn.h>
#include <errno.h>
#include <log/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <hardware/audio_amplifier.h>
#include <hardware/hardware.h>

#include <audio_hw.h>
#include <platform.h>
#include <platform_api.h>

#define UNUSED __attribute__((unused))

/* Stock persist calibration, written by factory provisioning */
#define CIRRUS_CAL_LEFT_FILE "/mnt/vendor/persist/audio/cs35l41_cal.bin"
#define CIRRUS_CAL_RIGHT_FILE "/mnt/vendor/persist/audio/cs35l41_cal_right.bin"

/* Stock valid range and defaults (see stock cirrus boot flow) */
#define CIRRUS_CAL_R_MIN 0x1e0d
#define CIRRUS_CAL_R_MAX 0x2e70
#define CIRRUS_CAL_R_DEFAULT 0x20c8

#define CIRRUS_BOOT_POLL_US 500000
#define CIRRUS_BOOT_MAX_POLLS 60

/* Not exposed via platform headers on this tree; resolved with dlsym */
const char* platform_get_snd_device_name(snd_device_t snd_device);

typedef struct cs35l41_device {
    amplifier_device_t amp_dev;
    struct audio_device* adev;
    const struct hw_module_t* module_ahal;
    pthread_t boot_thread;
    volatile bool boot_exit;
    typeof(enable_audio_route)* enable_audio_route;
    typeof(disable_audio_route)* disable_audio_route;
    typeof(platform_get_spkr_prot_snd_device)* platform_get_spkr_prot_snd_device;
    typeof(platform_get_snd_device_name)* platform_get_snd_device_name;
} cs35l41_t;

static cs35l41_t* cs_dev = NULL;

static int is_speaker(uint32_t snd_device) {
    int speaker = 0;
    switch (snd_device) {
        case SND_DEVICE_OUT_SPEAKER:
        case SND_DEVICE_OUT_SPEAKER_REVERSE:
        case SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES:
        case SND_DEVICE_OUT_SPEAKER_AND_LINE:
        case SND_DEVICE_OUT_SPEAKER_AND_HDMI:
        case SND_DEVICE_OUT_SPEAKER_AND_DISPLAY_PORT:
        case SND_DEVICE_OUT_SPEAKER_AND_ANC_HEADSET:
        case SND_DEVICE_OUT_SPEAKER_AND_ANC_FB_HEADSET:
        case SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET:
        case SND_DEVICE_OUT_SPEAKER_AND_BT_A2DP:
        case SND_DEVICE_OUT_SPEAKER_AND_BT_SCO:
        case SND_DEVICE_OUT_SPEAKER_AND_BT_SCO_WB:
        case SND_DEVICE_OUT_SPEAKER_AND_BT_SCO_SWB:
        case SND_DEVICE_OUT_VOICE_SPEAKER:
        case SND_DEVICE_OUT_VOICE_SPEAKER_2:
        case SND_DEVICE_OUT_VOICE_SPEAKER_STEREO:
        case SND_DEVICE_OUT_VOICE_SPEAKER_HFP:
        case SND_DEVICE_OUT_VOICE_SPEAKER_AND_VOICE_HEADPHONES:
        case SND_DEVICE_OUT_VOICE_SPEAKER_AND_VOICE_ANC_HEADSET:
        case SND_DEVICE_OUT_VOICE_SPEAKER_AND_VOICE_ANC_FB_HEADSET:
        case SND_DEVICE_OUT_VOICE_SPEAKER_STEREO_AND_VOICE_HEADPHONES:
        case SND_DEVICE_OUT_VOICE_SPEAKER_STEREO_AND_VOICE_ANC_HEADSET:
        case SND_DEVICE_OUT_VOICE_SPEAKER_STEREO_AND_VOICE_ANC_FB_HEADSET:
        case SND_DEVICE_OUT_VOIP_SPEAKER:
            speaker = 1;
            break;
    }

    return speaker;
}

static int32_t cirrus_read_cal_r(const char* path) {
    FILE* f = fopen(path, "rb");
    int32_t cal_r = -1;

    if (!f) {
        ALOGD("%s: no %s", __func__, path);
        return -1;
    }
    if (fread(&cal_r, sizeof(cal_r), 1, f) != 1) {
        ALOGE("%s: short read %s", __func__, path);
        cal_r = -1;
    }
    fclose(f);
    return cal_r;
}

static void* cirrus_boot_thread(UNUSED void* arg) {
    struct audio_device* adev = cs_dev->adev;
    struct mixer_ctl* ctl;
    int polls = 0;
    int32_t cal_l, cal_r;

    while ((!adev->platform || !adev->primary_output) && !cs_dev->boot_exit &&
           polls < CIRRUS_BOOT_MAX_POLLS) {
        ALOGD("%s: waiting for platform and primary output...", __func__);
        usleep(CIRRUS_BOOT_POLL_US);
        polls++;
    }
    if (!adev->platform || !adev->primary_output) return NULL;

    cal_l = cirrus_read_cal_r(CIRRUS_CAL_LEFT_FILE);
    cal_r = cirrus_read_cal_r(CIRRUS_CAL_RIGHT_FILE);

    if (cal_l < 0 || cal_r < 0 ||
        (uint32_t)(cal_l - CIRRUS_CAL_R_MIN) > (CIRRUS_CAL_R_MAX - CIRRUS_CAL_R_MIN) ||
        (uint32_t)(cal_r - CIRRUS_CAL_R_MIN) > (CIRRUS_CAL_R_MAX - CIRRUS_CAL_R_MIN)) {
        ALOGI("%s: no valid persist calibration, applying defaults", __func__);
        cal_l = CIRRUS_CAL_R_DEFAULT;
        cal_r = CIRRUS_CAL_R_DEFAULT;
    } else {
        ALOGI("%s: applying persist calibration L=%d R=%d", __func__, cal_l, cal_r);
    }

    ctl = mixer_get_ctl_by_name(adev->mixer, "RCV DSP Set CAL_Z");
    if (!ctl) {
        ALOGD("%s: no ctl RCV DSP Set CAL_Z", __func__);
    } else {
        mixer_ctl_set_value(ctl, 0, cal_l);
    }

    ctl = mixer_get_ctl_by_name(adev->mixer, "DSP Set CAL_Z");
    if (!ctl) {
        ALOGD("%s: no ctl DSP Set CAL_Z", __func__);
    } else {
        mixer_ctl_set_value(ctl, 0, cal_r);
    }

    return NULL;
}

static int amp_enable_devices(UNUSED amplifier_device_t* device, uint32_t devices, bool enable) {
    struct audio_device* adev = cs_dev->adev;
    snd_device_t prot_device;
    const char* name;

    if (!adev) {
        ALOGE("%s: Invalid params", __func__);
        return -EINVAL;
    }

    if (!is_speaker(devices)) return 0;

    prot_device = cs_dev->platform_get_spkr_prot_snd_device(devices);
    name = cs_dev->platform_get_snd_device_name(prot_device);

    if (enable) {
        ALOGD("%s: apply path: %s", __func__, name);
        audio_route_apply_and_update_path(adev->audio_route, name);
    } else {
        ALOGD("%s: reset path: %s", __func__, name);
        audio_route_reset_and_update_path(adev->audio_route, name);
    }

    return 0;
}

static int amp_dev_close(hw_device_t* device) {
    cs35l41_t* dev = (cs35l41_t*)device;

    if (cs_dev) cs_dev->boot_exit = true;
    pthread_join(cs_dev->boot_thread, NULL);

    if (dev) free(dev);
    cs_dev = NULL;
    return 0;
}

static int amp_calib(UNUSED amplifier_device_t* device, void* adev) {
    if (!adev) {
        ALOGE("%s: adev is NULL!", __func__);
        return -EINVAL;
    }

    cs_dev->adev = adev;

    ALOGI("%s: Initialize Cirrus Logic Playback module", __func__);
    if (pthread_create(&cs_dev->boot_thread, NULL, cirrus_boot_thread, NULL)) {
        ALOGE("%s: boot thread creation failed", __func__);
        return -EINVAL;
    }

    return 0;
}

static int amp_module_open(const hw_module_t* module, const char* name, hw_device_t** device) {
    if (strcmp(name, AMPLIFIER_HARDWARE_INTERFACE)) {
        ALOGE("%s:%d: %s does not match amplifier hardware interface name\n", __func__, __LINE__,
              name);
        return -ENODEV;
    }

    cs_dev = calloc(1, sizeof(cs35l41_t));
    if (!cs_dev) {
        ALOGE("%s:%d: Unable to allocate memory for amplifier device\n", __func__, __LINE__);
        return -ENOMEM;
    }

    cs_dev->amp_dev.common.tag = HARDWARE_DEVICE_TAG;
    cs_dev->amp_dev.common.module = (hw_module_t*)module;
    cs_dev->amp_dev.common.version = HARDWARE_DEVICE_API_VERSION(1, 0);
    cs_dev->amp_dev.common.close = amp_dev_close;

    cs_dev->amp_dev.set_input_devices = NULL;
    cs_dev->amp_dev.set_output_devices = NULL;
    cs_dev->amp_dev.enable_input_devices = NULL;
    cs_dev->amp_dev.enable_output_devices = amp_enable_devices;
    cs_dev->amp_dev.input_stream_standby = NULL;
    cs_dev->amp_dev.output_stream_standby = NULL;
    cs_dev->amp_dev.set_mode = NULL;
    cs_dev->amp_dev.input_stream_start = NULL;
    cs_dev->amp_dev.output_stream_start = NULL;
    cs_dev->amp_dev.set_parameters = NULL;
    cs_dev->amp_dev.in_set_parameters = NULL;
    cs_dev->amp_dev.out_set_parameters = NULL;
    cs_dev->amp_dev.set_feedback = NULL;
    cs_dev->amp_dev.calibrate = amp_calib;

    if (hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, AUDIO_HARDWARE_MODULE_ID_PRIMARY,
                               &cs_dev->module_ahal)) {
        ALOGW("%s: Failed to load audio.primary", __func__);
        return -ENODEV;
    }

#define LOAD_AHAL_SYMBOL(symbol)                                          \
    do {                                                                  \
        cs_dev->symbol = dlsym(cs_dev->module_ahal->dso, #symbol);        \
        if (cs_dev->symbol == NULL) {                                     \
            ALOGW("%s: %s not found (%s)", __func__, #symbol, dlerror()); \
            free(cs_dev);                                                 \
            return -ENODEV;                                               \
        }                                                                 \
    } while (0)

    LOAD_AHAL_SYMBOL(enable_audio_route);
    LOAD_AHAL_SYMBOL(disable_audio_route);
    LOAD_AHAL_SYMBOL(platform_get_spkr_prot_snd_device);
    LOAD_AHAL_SYMBOL(platform_get_snd_device_name);

#undef LOAD_AHAL_SYMBOL

    *device = (hw_device_t*)cs_dev;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
        .open = amp_module_open,
};

/* clang-format off */
amplifier_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AMPLIFIER_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AMPLIFIER_HARDWARE_MODULE_ID,
        .name = "Cirrus Logic CS35L41 amplifier HAL",
        .author = "The LineageOS Project",
        .methods = &hal_module_methods,
    },
};
