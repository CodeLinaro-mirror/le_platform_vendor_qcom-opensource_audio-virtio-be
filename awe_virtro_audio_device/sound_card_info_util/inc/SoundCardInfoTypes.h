/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#ifndef _SOUND_CARD_INFO_TYPES_H
#define _SOUND_CARD_INFO_TYPES_H

#include <iostream>
#include <inttypes.h>
#include <stdint.h>
#include <string>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <map>
#include <unordered_map>
#include <type_traits>
#include <set>
#include <unordered_set>

#include "awe_log_util.h"
#if !defined(__QNX__)
#include "sound/asound.h"
#endif
#include "virtio_snd.h"
#include "awe_types.h"

/******************************************************************************/
/*********************************** MACROS ***********************************/
/******************************************************************************/
#define AWE_LOGE(...) AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, __VA_ARGS__)
#define AWE_LOGI(...) AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, __VA_ARGS__)
#define AWE_LOGD(...) AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, __VA_ARGS__)

#ifdef __QNX__
typedef int snd_ctl_elem_iface_t;
typedef int snd_ctl_elem_type_t;
#endif

// The session_mode is used to indicate if the FE DAI
// is in normal/hostless/non-tunnel/no config mode.
enum SessModeType {
  kSessModeDefault = 0,
  kSessModeNoHost,
  kSessModeNonTunnel,
  kSessModeNoConfig,
};

// Used to identify groups for the DAI themselves, the formed mixer ctls or
// the group that a mixer ctl uses for its enum values.
enum DAIGroup {
    kNone = -1,
    kAllFrontends = 0,
    kTXFrontends,
    kRXFrontends,
    kAllBackends,
    kTXBackends,
    kRXBackends,
};

// Used to identify the control modes
enum MixerControlMode {
    NAMED_CONTROL,
    STATIC_CONTROL_NODSP,
    STATIC_CONTROL_DSP,
    EVENT_CONTROL,
};

struct FrontendInfo {
    int id = -1;
    std::string name;
    int direction;
    SessModeType sess_mode;
    // The following structs are common for all frontend devices.
    uint32_t    features;
    uint64_t    formats;
    uint64_t    rates;
    uint8_t     channels_min;
    uint8_t     channels_max;
};

// The virtio driver requires that the enum value for a mixer ctl be sent as u8.
struct VirtioSndDCEnum {
   /* null-terminated ASCII value name */
   __u8 dai_name[VIRTIO_SND_DC_NAME_MAXLEN];
};

/*******************************************************************************
                              Ctl extn structures
*******************************************************************************/
union ValueUnion {
  struct {
    uint32_t min;
    uint32_t max;
    uint32_t step;
  } integer;
  struct {
    uint64_t min;
    uint64_t max;
    uint64_t step;
  } integer64;
  struct {
    uint32_t items;
    uint32_t item;
    char name[64];
  } enumerated;
};

// Modeled from the snd_ctl_elem_id used by Virtio
struct CtlElemId {
   std::string name;
   unsigned int ctl_id = 0;
   #if !defined(__QNX__)
   snd_ctl_elem_iface_t iface = SNDRV_CTL_ELEM_IFACE_MIXER;
   #else
   int iface = VIRTIO_CTL_ID_IFACE_MIXER;
   #endif
};


/*******************************************************************************
                                Mixer ctl structures
*******************************************************************************/
struct SoundCardMixerCtl {
    // std::string name;
    CtlElemId id;
    DAIGroup dai_group;
    #if !defined(__QNX__)
    snd_ctl_elem_type_t type;
    #else
    int type;
    #endif
    awe_control_type_t awe_ctl_type;
    MixerControlMode ctrl_mode;
    unsigned int access;
    unsigned int count;
    ValueUnion value;
    unsigned char reserved[64-4*sizeof(unsigned short)];
};

#endif /* _SOUND_CARD_INFO_TYPES_H */
