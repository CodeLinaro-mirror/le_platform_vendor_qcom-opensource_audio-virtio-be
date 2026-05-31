/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef ADM_SERVICE_TYPE_H_
#define ADM_SERVICE_TYPE_H_

/* ADM Service Type — shared by AudioDeviceManager, audio_plugin_wrapper,
 * sound_card_info_wrapper, etc. */
typedef enum {
    ADM_TYPE_UNINITIALIZED = -1,
    ADM_TYPE_AWE = 0,
    ADM_TYPE_AR  = 1
} ADMServiceType;

/* gvm_id passed to SoundCardInfoUtil on the HOST side.
 * The HOST is always VM 0 from the sound-card-info perspective. */
#define ADM_HOST_GVM_ID  0

#endif  // ADM_SERVICE_TYPE_H_
