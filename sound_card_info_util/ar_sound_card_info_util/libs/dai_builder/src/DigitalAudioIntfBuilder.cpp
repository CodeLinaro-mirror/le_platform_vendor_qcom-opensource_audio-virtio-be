/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "DigitalAudioIntfBuilder.h"

/* Populates the direction-specific vectors (.rx_dai, .tx_dai) in the all_dai structure.
   Each DAI is also added to a direction-specific enum list, which contains only
   the DAI name. This enum list will be retrieved on a per-mixer control basis by the ADM.
*/
template <typename VectorT>
uint32_t DigitalAudioIntfBuilder::SortDAIByDir(VectorT &all_dai, SortedDCEnums &dai_enums) {
    uint32_t rc = SCIU_EOK;

    if (all_dai.unsorted_dai.size() == 0) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "no DAIs to sort!");
        return SCIU_EBADPARAM;
    }

    if (!(dai_enums.tx_enums.empty()) ||
        !(dai_enums.rx_enums.empty())) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "dai_enums is non-empty on init");
        return SCIU_EBADPARAM;
    }

    for (auto node : all_dai.unsorted_dai) {
        switch (node.direction) {
            case SNDRV_PCM_STREAM_PLAYBACK: {
                all_dai.rx_dai.push_back(node);
                AddEnumVal(node.name, dai_enums.rx_enums);
                break;
            }
            case SNDRV_PCM_STREAM_CAPTURE: {
                all_dai.tx_dai.push_back(node);
                AddEnumVal(node.name, dai_enums.tx_enums);
                break;
            }
            default:
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "DAI is neither playback nor capture!");
                rc = SCIU_EUNSUPPORTED;
                break;
        }
    }
    return rc;
}

void DigitalAudioIntfBuilder::AddEnumVal(std::string dai_name_str,
                                         std::vector<VirtioSndDCEnum> &enum_list) {
    // Only the name is required for the enums list
    VirtioSndDCEnum enum_val = SetEnumValFromDAIName(dai_name_str);
    enum_list.insert(enum_list.end(), enum_val);
}

VirtioSndDCEnum DigitalAudioIntfBuilder::SetEnumValFromDAIName(std::string dai_name_str) {
    VirtioSndDCEnum enum_val;
    unsigned char* dai_name_usc = reinterpret_cast<unsigned char *> (const_cast<char*>(dai_name_str.c_str()));
    size_t len = strlen(dai_name_str.c_str()) + 1; // null-termination
    memcpy(enum_val.dai_name, dai_name_usc, len);
    return enum_val;
}

// Explicit instantiation for SoundCardInfoUtil to use for FEs and BEs.
template uint32_t DigitalAudioIntfBuilder::SortDAIByDir
<FrontendVectors>(FrontendVectors &all_dai, SortedDCEnums &dai_enums);

template uint32_t DigitalAudioIntfBuilder::SortDAIByDir
<BackendVectors>(BackendVectors &all_dai, SortedDCEnums &dai_enums);