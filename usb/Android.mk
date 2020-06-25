# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ifeq ($(INTEL_AUDIO_HAL),audio)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	audio_hal.c

LOCAL_SHARED_LIBRARIES := \
	liblog \
	libcutils \
	libaudioutils \
        libalsautils \
	libtinyalsa \
	libaudioroute \
	libdl

LOCAL_C_INCLUDES += \
	external/tinyalsa/include \
	$(call include-path-for, audio-utils) \
	$(call include-path-for, audio-route) \
	$(call include-path-for, audio-effects)

LOCAL_CFLAGS :=\
 -fno-strict-overflow \
 -fwrapv \
 -D_FORTIFY_SOURCE=2 \
 -fstack-protector-strong \
 -Wno-conversion-null \
 -Wnull-dereference \
 -Werror \
 -Warray-bounds \
 -Wformat -Wformat-security \
 -Werror=format-security

#Preferred paths for all vendor hals /vendor/lib/hw
LOCAL_PROPRIETARY_MODULE := true

LOCAL_HEADER_LIBRARIES += libhardware_headers

LOCAL_MODULE := audio.usb.$(TARGET_BOARD_PLATFORM)

LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

endif # INTEL_AUDIO_HAL
