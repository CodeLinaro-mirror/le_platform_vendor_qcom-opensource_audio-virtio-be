/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ArSoundCardInfoUtil.h"

/* Returns an instance of the SoundCardInfoUtil for a given gvm_id.
   If no gvm_id is provided, the default of 0 is used (i.e. single GVM case).
*/
SoundCardInfoUtil::SoundCardInfoUtil(const int gvm_id)
    : gvm_id_(gvm_id),
      num_pcm_frontends_(0),
      num_mixer_ctls_(0) {
}

SoundCardInfoUtil::~SoundCardInfoUtil() {
}

/* Collects DAI information (frontends and backends) and mixer ctls.
   3 libraries are used: XMLParser, DigitalAudioIntfBuilder, and MixerCtlBuilder.
   Once the sound card is built, the Audio Device Manager may access the DAI
   and mixer ctl information via their respective getter functions.
*/
uint32_t SoundCardInfoUtil::BuildSoundCard() {
    uint32_t rc = SCIU_EOK;

    // Parse all 3 XMLs to collect the raw DAI and mixer ctl extn information.
    rc = XMLParser::ParseAllXMLs(all_frontends_, all_backends_,
                            all_ctl_extns_, gvm_id_);
    if (rc) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to parse XMLs, ret %d", rc);
        return rc;
    }

    // Build the DAI info and insert any set constraints (e.g. PCM constraints)
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Sorting DAIs");

    rc = DigitalAudioIntfBuilder::SortDAIByDir(all_frontends_, all_frontend_enums_);
    if (rc) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to sort frontends, ret %d", rc);
        return rc;
    }
    rc = DigitalAudioIntfBuilder::SortDAIByDir(all_backends_, all_backend_enums_);
    if (rc) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "Failed to sort backends, ret %d", rc);
        return rc;
    }

    // Count each DAI group for the upcoming PCM_INFO request and enum value queries
    num_pcm_frontends_ = all_frontends_.unsorted_dai.size();
    CountDAIs();

    // Combine the DAI prefixes with their respective (common and direction-specific) extns
    // Also populate the enum lookup to respond to enum queries from GVM->ADM.
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Building mixer ctls");
    rc = MixerCtlBuilder::BuildAllMixerCtls(all_frontends_, all_backends_,
                                       all_ctl_extns_, dai_counts_,
                                       mixer_ctl_enum_lookup_, all_mixer_ctls_);
    if (rc) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to build all mixer controls, ret %d", rc);
        return rc;
    }

    // Mixer ctl count is used for the DC_INFO request
    num_mixer_ctls_ = all_mixer_ctls_.size();
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Exit");

    return rc;
}

void SoundCardInfoUtil::CountDAIs() {
    // Count the FE DAIs
    dai_counts_[kAllFrontends] = all_frontends_.unsorted_dai.size();
    dai_counts_[kTXFrontends] = all_frontends_.tx_dai.size();
    dai_counts_[kRXFrontends] = all_frontends_.rx_dai.size();
    // Count the BE DAIs
    dai_counts_[kAllBackends] = all_backends_.unsorted_dai.size();
    dai_counts_[kTXBackends] = all_backends_.tx_dai.size();
    dai_counts_[kRXBackends] = all_backends_.rx_dai.size();
}

int SoundCardInfoUtil::GetNumPCMSubstreams() {
    return dai_counts_[kAllFrontends];
}

int SoundCardInfoUtil::GetNumMixerCtls() {
    return num_mixer_ctls_;
}

std::vector<FrontendInfo> SoundCardInfoUtil::GetPCMSubstreams() {
    return all_frontends_.unsorted_dai;
}

std::vector<SoundCardMixerCtl> SoundCardInfoUtil::GetMixerCtls() {
    return all_mixer_ctls_;
}

/* Returns a vector of DAI names. Each DAI is a possible enum value for the given
   mixer control with index "control_id".
   The number of DAI names returned must be equivalent to the "items" field in
   the SoundCardMixerCtl union for ENUM type mixer ctls.

   Throws an invalid_argument exception if the control_id indexes to a mixer control
   NOT of type ENUM.
*/
uint32_t SoundCardInfoUtil::GetEnumsForControl(const int control_id, std::vector<VirtioSndDCEnum> &ctl_enums) {
    uint32_t rc = SCIU_EOK;
    if (mixer_ctl_enum_lookup_.empty()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Enum lookup is empty!");
        return SCIU_EFAILED;
    }

    if ((control_id < 0) || (control_id >= mixer_ctl_enum_lookup_.size())) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "control_id %d is out of bounds!", control_id);
        return SCIU_EBADPARAM;
    }

    DAIGroup enum_type = mixer_ctl_enum_lookup_[control_id];
    switch (enum_type) {
        case kAllFrontends: {
            // FE enums are stored separately by direction
            ctl_enums = all_frontend_enums_.tx_enums;
            ctl_enums.insert(ctl_enums.end(), all_frontend_enums_.rx_enums.begin(),
                                            all_frontend_enums_.rx_enums.end());
            break;
        }
        case kTXFrontends:
            ctl_enums = all_frontend_enums_.tx_enums;
            break;
        case kRXFrontends:
            ctl_enums = all_frontend_enums_.rx_enums;
            break;
        case kAllBackends: {
            // BE enums are stored separately by direction
            ctl_enums = all_backend_enums_.tx_enums;
            ctl_enums.insert(ctl_enums.end(), all_backend_enums_.rx_enums.begin(),
                                            all_backend_enums_.rx_enums.end());
            break;
        }
        case kTXBackends:
            ctl_enums = all_backend_enums_.tx_enums;
            break;
        case kRXBackends:
            ctl_enums = all_backend_enums_.rx_enums;
            break;
        case kNone: {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "control_id %d accesses entry that is not of type ENUMs", control_id);
            rc = SCIU_EBADPARAM;
        }
    }
    return rc;
}

std::vector<MixerCtlInfo> SoundCardInfoUtil::CreateMixerCtlMap(std::vector<SoundCardMixerCtl> mixer_ctls)
{
    std::vector<MixerCtlInfo> all_mixer_ctl_info;
    int ctl_id = 0;
    int rc = 0;

    if (mixer_ctls.empty()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "CreateMixerCtlMap invalid Args");
        return all_mixer_ctl_info;
    }

    // Process the mixer ctls to be used for CSD2AudioPlugin ctor
    for (auto ctl : mixer_ctls) {
        // The CSD2AudioPlugin needs the dai_group (FE or BE) and an enum map to start
        std::vector<VirtioSndDCEnum> dc_enums;
        std::vector<std::string> str_enums;
        MixerCtlInfo ctl_info;

        if (ctl.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
            // Start the enum mappings with ZERO as the default
            str_enums.push_back("ZERO");
            // Get the rest of the enum mappings
            rc = GetEnumsForControl(ctl_id, dc_enums);
            if (rc != SCIU_EOK) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "failed to get enums for mixer control");
                return all_mixer_ctl_info;
            }
            for (auto u8_enum : dc_enums) {
                // Convert from u8 to string
                str_enums.push_back((char *)(u8_enum.dai_name));
            }
        }
        ctl_info.enum_mapping = str_enums;
        ctl_info.ctl_prefix = ctl.dai_group;
        ctl_info.name = ctl.id.name;
        all_mixer_ctl_info.push_back(ctl_info);
        ctl_id += 1;
    }

    return all_mixer_ctl_info;
}