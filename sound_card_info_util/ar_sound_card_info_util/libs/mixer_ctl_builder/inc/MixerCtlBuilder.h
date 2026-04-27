/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "SoundCardInfoTypes.h"
#include "audio_log_util.h"
class MixerCtlBuilder {
public:
    static uint32_t BuildAllMixerCtls(const FrontendVectors &frontends,
                                  const BackendVectors &backends,
                                  const MixerCtlExtns &ctl_extns,
                                  const DAICounts &dai_counts_,
                                  std::vector<DAIGroup> &ctl_enum_lookup,
                                  std::vector<SoundCardMixerCtl> &mixer_ctls);
protected:
    template <typename DAIType>
    static uint32_t BuildMixerCtlSubset(unsigned int &ctl_id,
                                 const std::vector<DAIType> &dai_subset,
                                 const std::vector<CtlExtn> &ctl_extns,
                                 const DAICounts &dai_counts_,
                                 std::vector<DAIGroup> &ctl_enum_lookup,
                                 std::vector<SoundCardMixerCtl> &mixer_ctls);
};