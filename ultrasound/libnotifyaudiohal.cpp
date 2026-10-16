/*
 * Copyright (C) 2023 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "libnotifyaudiohal"

#include <android/hardware/audio/7.0/IDevicesFactory.h>
#include <log/log.h>
#include <string.h>

using android::sp;
using android::hardware::audio::V7_0::IDevicesFactory;
using android::hardware::audio::V7_0::IPrimaryDevice;
using android::hardware::audio::V7_0::Result;

static void ultrasound_enable(int enable) {
    ALOGD("ultrasound_enable: %d", enable);
    auto factory = IDevicesFactory::getService();
    factory->openPrimaryDevice([&](Result retval, const sp<IPrimaryDevice>& result) {
        if (retval == Result::OK) {
            result->setParameters({} /* context */,
                                  {
                                          {"ultrasound-sensor", std::to_string(enable)},
                                  });
        }
    });
}

extern "C" int elliptic_notify_audio_hal(char* param) {
    int enable;

    ALOGD("elliptic_notify_audio_hal : %s ", param);

    if (!strcmp(param, "ultrasound-proximity=1") ||
        !strcmp(param, "ultrasound-calibration=1") ||
        !strcmp(param, "ultrasound-quickmute=1") ||
        !strcmp(param, "ultrasound-quicksnap=1") ||
        !strcmp(param, "ultrasound-quickzoom=1") ||
        !strcmp(param, "ultrasound_enable=1")) {
        enable = 1;
    } else if (!strcmp(param, "ultrasound-proximity=0") ||
               !strcmp(param, "ultrasound-calibration=0") ||
               !strcmp(param, "ultrasound-quickmute=0") ||
               !strcmp(param, "ultrasound-quicksnap=0") ||
               !strcmp(param, "ultrasound-quickzoom=0") ||
               !strcmp(param, "ultrasound_enable=0")) {
        enable = 0;
    } else {
        return 0;
    }

    ALOGD("elliptic_notify_audio_hal, enable %d", enable);
    ultrasound_enable(enable);

    return 0;
}

extern "C" bool elliptic_ultrasound_supported() {
    return true;
}
