/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "MixerCtlBuilder.h"

/* Returns all of the possible mixer ctls by combining all frontends with their
   common and direction-specific extns, and all backends with their common extns.
   The ctl_enum_lookup is also populated with the enum DAIGroup and is populated
   in the same order as the mixer ctls are created in. This way, the ctl_enum_lookup
   can be indexed by control_id.
*/
uint32_t MixerCtlBuilder::BuildAllMixerCtls(const FrontendVectors &frontends,
                                        const BackendVectors &backends,
                                        const MixerCtlExtns &ctl_extns,
                                        const DAICounts &dai_counts,
                                        std::vector<DAIGroup> &ctl_enum_lookup,
                                        std::vector<SoundCardMixerCtl> &mixer_ctls) {
    uint32_t rc = SCIU_EOK;

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Enter");

    if (frontends.unsorted_dai.empty()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "no frontend ctl prefixes to form mixer ctls with!");
        return SCIU_EBADPARAM;
    }
    if (backends.unsorted_dai.empty()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "no backend ctl prefixes to form mixer ctls with!");
        return SCIU_EBADPARAM;
    }

    unsigned int ctl_id = 0;
    // Assemble all frontend mixer ctls for common and direction-specific extns
    // 1. Mixer ctls for TX FE DAI with common FE extns
    rc = BuildMixerCtlSubset<FrontendInfo>(ctl_id, frontends.tx_dai, ctl_extns.common_frontend_extns,
                                   dai_counts, ctl_enum_lookup, mixer_ctls);
    if (rc)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "error when forming mixer ctls for TX FE DAI with common FE extns, ret = %d");
    // 2. Mixer ctls for TX FE DAI with TX FE extns
    rc = BuildMixerCtlSubset<FrontendInfo>(ctl_id, frontends.tx_dai, ctl_extns.tx_frontend_extns,
                                   dai_counts, ctl_enum_lookup, mixer_ctls);
    if (rc)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "error when forming mixer ctls for TX FE DAI with TX FE extns, ret = %d");
    // 3. Mixer ctls for RX FE DAI with common FE extns
    rc = BuildMixerCtlSubset<FrontendInfo>(ctl_id, frontends.rx_dai, ctl_extns.common_frontend_extns,
                                   dai_counts, ctl_enum_lookup, mixer_ctls);
    if (rc)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "error when forming mixer ctls for RX FE DAI with common FE extns, ret = %d");
    // 4. Mixer ctls for RX FE DAI with RX FE extns
    rc = BuildMixerCtlSubset<FrontendInfo>(ctl_id, frontends.rx_dai, ctl_extns.rx_frontend_extns,
                                   dai_counts, ctl_enum_lookup, mixer_ctls);
    if (rc)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "error when forming mixer ctls for RX FE DAI with RX FE extns, ret = %d");
    // Assemble all backend mixer ctls - all ctl_extns are common
    // 5. Mixer ctls for RX BE DAI with common BE extns
    rc = BuildMixerCtlSubset<BackendInfo>(ctl_id, backends.rx_dai, ctl_extns.backend_extns,
                                  dai_counts, ctl_enum_lookup, mixer_ctls);
    if (rc)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "error when forming mixer ctls for RX BE DAI with common BE extns, ret = %d");
    // 6. Mixer ctls for TX BE DAI with common BE extns
    rc = BuildMixerCtlSubset<BackendInfo>(ctl_id, backends.tx_dai, ctl_extns.backend_extns,
                                  dai_counts, ctl_enum_lookup, mixer_ctls);
    if (rc)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "error when forming mixer ctls for TX BE DAI with common BE extns, ret = %d");

    // At this point, all the mixer ctls are filled in
    // in mixer_ctls and all mixer ctls are mapped in ctl_enum_lookup
    return rc;
}

/* Builds one set of mixer ctls. Any and all DAIs from the dai_subset will be
   combined with any and all extns in the ctl_extns.
   This function can be used for FrontendInfo nodes and BackendInfo nodes.
*/
template <typename DAIType>
uint32_t MixerCtlBuilder::BuildMixerCtlSubset(unsigned int &ctl_id, const std::vector<DAIType> &dai_subset,
                                       const std::vector<CtlExtn> &ctl_extns,
                                       const DAICounts &dai_counts,
                                       std::vector<DAIGroup> &ctl_enum_lookup,
                                       std::vector<SoundCardMixerCtl> &mixer_ctls) {
    uint32_t rc = SCIU_EOK;
    if (ctl_extns.empty()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "no ctl extns for form mixer ctls with!");
        return SCIU_EBADPARAM;
    }

    // Assemble the given dai_subset with the given ctl_extns
    for (auto dai : dai_subset) {
        for (auto extn : ctl_extns) {
            // Set the basic mixer_ctl info that's not dependent on type or set
            // any default values
            CtlElemId elem_id = {
                .name=dai.name + " " + extn.id.name,
                .ctl_id= ctl_id,
                .iface=extn.id.iface};
            SoundCardMixerCtl new_mixer_ctl = {
                .id = elem_id,
                .dai_group = kNone,
                .type = extn.type,
                .access = extn.access,
                .count = extn.count,
                .value = extn.value,
            };
            if (std::is_same<DAIType, FrontendInfo>::value)
                new_mixer_ctl.dai_group = kAllFrontends;
            if (std::is_same<DAIType, BackendInfo>::value)
                new_mixer_ctl.dai_group = kAllBackends;
            if (extn.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
                int enum_count = dai_counts.at(extn.enum_dai_type);
                if (!enum_count) {
                    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "no enum values for this mixer ctl!");
                    rc = SCIU_EBADPARAM;
                }
                new_mixer_ctl.value.enumerated.items = enum_count;
                //REVIEW what to do for the rest of the .enumerated members?
            }
            mixer_ctls.push_back(new_mixer_ctl);
            ctl_enum_lookup.push_back(extn.enum_dai_type);
            ctl_id++;
        }
    }
    return rc;
}
