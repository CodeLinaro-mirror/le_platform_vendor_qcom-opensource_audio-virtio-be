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


#if !defined(__QNX__)
#include "sound/asound.h"
#endif
#include "virtio_snd.h"

#ifdef __QNX__
typedef int snd_ctl_elem_iface_t;
typedef int snd_ctl_elem_type_t;
#endif

// Error codes
// Success. The operation completed with no errors.
#define SCIU_EOK                                     (0)
// General failure.
#define SCIU_EFAILED                                 (1)
// Bad operation parameter.
#define SCIU_EBADPARAM                               (2)
// Unsupported routine or operation.
#define SCIU_EUNSUPPORTED                            (3)
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

enum VirtioPCMState {
    VIRTIO_PCM_STATE_INVALID = -1,
    VIRTIO_PCM_OPENED = 0,
    VIRTIO_PCM_PREPARED,
    VIRTIO_PCM_STARTED,
    VIRTIO_PCM_STOPPED,
    VIRTIO_PCM_CLOSED,
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

    enum VirtioPCMState stream_state;
};
// Used in CountDAIs to organize and easily access the number of DAIs available
// for a DAI group, e.g. TX frontends
typedef std::unordered_map<DAIGroup, uint32_t> DAICounts;

// The virtio driver requires that the enum value for a mixer ctl be sent as u8.
struct VirtioSndDCEnum {
   /* null-terminated ASCII value name */
   __u8 dai_name[VIRTIO_SND_DC_NAME_MAXLEN];
};

// Used to store and store the FE and BE DAI names as enums (VirtioSndDCEnum)
struct SortedDCEnums {
  std::vector<VirtioSndDCEnum> tx_enums;
  std::vector<VirtioSndDCEnum> rx_enums;
};

// typedef for readability - this is used in multiple XMLs to track duplicate
// entries in the XML
typedef std::unordered_map<std::string, std::string> StrToStrMap;


/*******************************************************************************
                                DAI structures
*******************************************************************************/
template <typename DAIType>
struct DAIVectors {
    std::vector<DAIType> unsorted_dai;
    std::vector<DAIType> tx_dai;
    std::vector<DAIType> rx_dai;
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

struct BackendInfo {
    std::string name;
    int direction;
};

typedef DAIVectors<FrontendInfo> FrontendVectors;
typedef DAIVectors<BackendInfo> BackendVectors;

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

struct CtlExtn {
    CtlElemId id;
    snd_ctl_elem_type_t type = SNDRV_CTL_ELEM_TYPE_NONE;
    unsigned int access = 0;
    unsigned int count = 0;
    ValueUnion value;
    DAIGroup enum_dai_type = kNone;
};

struct MixerCtlExtns {
    std::vector<CtlExtn> common_frontend_extns;
    std::vector<CtlExtn> tx_frontend_extns;
    std::vector<CtlExtn> rx_frontend_extns;
    std::vector<CtlExtn> backend_extns;
    // Any direction-specific backend extns should be added below.
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
    /* Renamed from awe_ctl_type to plugin_ctl_type to avoid leaking AWE-domain
     * naming into this shared header.  The field stores the plugin-specific
     * control type (e.g. awe_control_type_t cast to uint32_t for AWE; unused
     * for AR).
     * TODO: move this field to an AWE-only header once the full cleanup lands. */
    uint32_t plugin_ctl_type;
    MixerControlMode ctrl_mode;
    unsigned int access;
    unsigned int count;
    ValueUnion value;
    unsigned char reserved[64-4*sizeof(unsigned short)];
};


struct MixerCtlInfo {
  DAIGroup                   ctl_prefix;    // helps CSD2AudioPlugin to determine if it is a front end / backend based mixer control
  std::vector<std::string>   enum_mapping;  // contains mapping for enumerated mixer controls, EX:enum_mapping_[0] = "ZERO", 0 is enum value, ZERO is enum string
  std::string                name;
};

/*******************************************************************************
                                XML parser structures
*******************************************************************************/
typedef std::unordered_set<std::string> StrSet;

struct ExtnSet {
    StrSet common_frontend_extns;
    StrSet tx_frontend_extns;
    StrSet rx_frontend_extns;
    StrSet backend_extns;
};

// XMLData is used for all XMLs during parsing and so, contains members for each
// type of XML (FE, BE, and extns).
struct XMLData {
    char            data_buf[1024];
    size_t          offset;
    std::string     parent_tag;     // Used for FE and BE XMLs
    DAIGroup        extn_dir_tag;   // Used for extns XML
    std::unordered_set<int>   frontend_ids; // Ensures no duplicate frontends
    FrontendVectors frontends;      // All frontends with their info
    StrToStrMap     backends;       // Maps name to direction
    ExtnSet         ctl_extn_names; // Ensures no duplicate extns
    MixerCtlExtns   ctl_extns;      // All extns with their info
};

// Maps are used to aid in ctl extn XML parsing. When a tag is encountered, it is
// referenced against these maps to see if the tag is valid and whether its text
// content should be stored.
typedef std::unordered_map<std::string, DAIGroup> ExtnTagMap;
typedef std::unordered_map<std::string, snd_ctl_elem_iface_t> ExtnIFaceMap;
typedef std::unordered_map<std::string, snd_ctl_elem_type_t> ExtnTypeMap;
typedef std::unordered_map<std::string, unsigned int> ExtnAccessMap;
typedef std::unordered_map<std::string, DAIGroup> ExtnEnumMap;

#endif /* _SOUND_CARD_INFO_TYPES_H */
