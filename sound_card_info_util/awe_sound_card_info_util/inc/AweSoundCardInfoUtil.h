/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "SoundCardInfoTypes.h"
#include "ISoundCardInfoUtil.h"
#include <string>
#include <vector>
#include <unordered_map>

#include "awe_api.h"
#include "awe_defs.h"
#include "awe_ioctl.h"
#include "awe_status.h"
#include "awe_types.h"

class AweSoundCardInfoUtil : public ISoundCardInfoUtil {

public:
    AweSoundCardInfoUtil(const int gvm_id = 0);
    ~AweSoundCardInfoUtil();

    uint32_t BuildSoundCard();
    int GetNumPCMSubstreams();
    int GetNumMixerCtls();
    std::vector<FrontendInfo> GetPCMSubstreams();
    std::vector<SoundCardMixerCtl> GetMixerCtls();
    uint32_t GetEnumsForControl(const int control_id, std::vector<VirtioSndDCEnum> &ctl_enums);

protected:
    int gvm_id_ = 0;
    uint32_t num_pcm_devices_;
    uint32_t num_mixer_ctrls_;
    std::vector<FrontendInfo> all_pcm_devices_;
    std::vector<SoundCardMixerCtl> all_mixer_ctrls_;
    std::unordered_map<int, std::vector<VirtioSndDCEnum>> control_enum_map_;
private:
    uint32_t AddAweMixCtl(const awe_control_t *awe_ctl);
    uint32_t AddAwePcmDevice(const awe_device_t *awe_dev);
    uint32_t AddAweEventCtl(const awe_event_t *awe_event);
    void AddStaticMixerCtls();
};
