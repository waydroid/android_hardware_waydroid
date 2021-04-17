#
# Copyright 2015 The Android Open-Source Project
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
#

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_VENDOR_MODULE := true
spurv_hwcomposer_shared_libraries := \
    liblog \
    libutils \
    libcutils \
    libEGL \
    libutils \
    libhardware \
    libsync \
    libui \
    libdrm \
    libdrm_etnaviv \

spurv_hwcomposer_src_files := \
    hwcomposer.cpp \
    simple-dmabuf-drm.c

spurv_hwcomposer_cflags += \
    -DLOG_TAG=\"hwcomposer\" \
    -Iout/soong/.intermediates/external/wayland-protocols/wayland_extension_client_protocol_headers/gen

spurv_hwcomposer_c_includes += \
    system/core \
    system/core/libsync \
    system/core/libsync/include

spurv_hwcomposer_relative_path := hw

LOCAL_SHARED_LIBRARIES := $(spurv_hwcomposer_shared_libraries)
LOCAL_STATIC_LIBRARIES := libwayland_client libffi libwayland_extension_client_protocols
LOCAL_HEADER_LIBRARIES := libsystem_headers
LOCAL_SRC_FILES := $(spurv_hwcomposer_src_files)
LOCAL_CFLAGS := $(spurv_hwcomposer_cflags)
LOCAL_C_INCLUDES := $(spurv_hwcomposer_c_includes)
LOCAL_MODULE_RELATIVE_PATH := $(spurv_hwcomposer_relative_path)

LOCAL_MODULE := hwcomposer.anbox
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

