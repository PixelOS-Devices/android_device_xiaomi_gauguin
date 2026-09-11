/*
 * Copyright (C) 2023 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * gauguin port: stock veux shim used android.hardware.audio@6.0, but gauguin
 * serves android.hardware.audio@7.0 (see device.mk), so IDevicesFactory 6.0
 * getService() returned null and openPrimaryDevice segfaulted sensors@1.0
 * (tombstone in elliptic_notify_audio_hal+252 -> bootloop on first
 * proximity activation). Use the 7.0 API with null guards instead.
 */

#define LOG_TAG "libnotifyaudiohal"

#include <android/hardware/audio/7.0/IDevicesFactory.h>
#include <android/hardware/audio/7.0/IPrimaryDevice.h>
#include <android/hardware/audio/7.0/types.h>
#include <log/log.h>
#include <string>
#include <string.h>

using ::android::hardware::hidl_vec;

using android::sp;
using android::hardware::audio::V7_0::IDevicesFactory;
using android::hardware::audio::V7_0::IPrimaryDevice;
using android::hardware::audio::V7_0::ParameterValue;
using android::hardware::audio::V7_0::Result;

static void ultrasound_enable(int enable) {
    ALOGD("ultrasound_enable: %d", enable);
    auto factory = IDevicesFactory::getService();
    if (factory == nullptr) {
        ALOGE("ultrasound_enable: no 7.0 IDevicesFactory");
        return;
    }
    factory->openPrimaryDevice([&](Result retval, const sp<IPrimaryDevice>& result) {
        if (retval == Result::OK && result != nullptr) {
            result->setParameters({} /* context */,
                                  {
                                          {"ultrasound-sensor", std::to_string(enable)},
                                  },
                                  [&](Result, const hidl_vec<ParameterValue>&) {});
        } else {
            ALOGE("ultrasound_enable: openPrimaryDevice failed");
        }
    });
}

extern "C" int elliptic_notify_audio_hal(char* param) {
    ALOGD("elliptic_notify_audio_hal: %s", param);
    if (!strcmp(param, "ultrasound-proximity=1")) {
        ultrasound_enable(1);
    } else if (!strcmp(param, "ultrasound-proximity=0")) {
        ultrasound_enable(0);
    }

    return 0;
}

extern "C" bool elliptic_ultrasound_supported() {
    return true;
}
