/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef AWE_AUDIO_PLUGIN_H_
#define AWE_AUDIO_PLUGIN_H_
#include <stdio.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <chrono>
#include <functional>

#include "audio_plugin_interface.h"
#include "awe_api.h"
#include "awe_defs.h"
#include "awe_ioctl.h"
#include "awe_status.h"
#include "awe_types.h"
#include "awe_manager.h"

#define AWE_STREAM_EVENT_BUF_AVAIL                      0x1
#define AWE_STREAM_EVENT_EOS                            0x2
#define	MAX_PROCESSORS                                  4
#define TLV_HEADER_SIZE                                 8
enum AweStreamState {
  PCM_STATE_CLOSED = -1,
  PCM_STATE_OPEN,
  PCM_STATE_SETUP,
  PCM_STATE_PREPARED,
  PCM_STATE_RUNNING,
  PCM_STATE_XRUN,
  PCM_STATE_DRAINING,
  PCM_STATE_SUSPENDED
};

struct AweBufPool {
  std::vector<uint8_t> buf_used;
  uint32_t free_count;
  uint32_t num_buffs;
  uint32_t buf_size;
  std::mutex rw_mutex;
  std::condition_variable rw_cond;
};

struct AweStream {
  std::string                           fe_dai_name;
  uint32_t                              direction;
  uint32_t                              device_id;
  int32_t                               handle;
  uint32_t                              sess_mode;
  bool                                  is_push_poll = false;
  struct awe_media_config               format;
  struct awe_threshold_config           threshold;
  struct awe_buffer_config              buffer_config;
  uint32_t                              data_mode;
  enum AweStreamState                   state = PCM_STATE_CLOSED;
  std::mutex                            stream_mutex; //used to protect
  std::condition_variable               stream_cond;
  uint32_t                              stream_event;
  AweBufPool                            buf_pool;
  uint32_t                              gvm_mmap_fd;
};

struct AweCtrlValue {
  //request from tinyalsa
  uint32_t                              elem_count;
  uint32_t                              elem_ctrl_id;
  struct CtlElemPayload                 elem_payload;

  //response from DSP, status, payload...
};

typedef struct AweCtrlVectorValue{
    uint16_t offset;
    uint16_t count;
    uint32_t do_set;
    union {
        uint32_t integer[0];
        uint32_t enumerated[0];
        float floating[0];
    }value;
}AweCtrlVectorValue_t;


struct GVMEventCallbackParams {
    uint32_t source_module_id;
    uint32_t event_id;
    uint32_t param_size;
    void* param;
};

struct AweShmemMapInfo{
  uint32_t data_export_id;
  uint32_t data_fd;
  uint32_t export_fd;
  uint32_t data_size;
  uint32_t mem_addr_lsw[AWE_MAX_PROCESSOR_CORES];
  uint32_t mem_addr_msw[AWE_MAX_PROCESSOR_CORES];
};

class AweAudioPlugin : public IAudioPlugin {

    public:
        AweAudioPlugin(std::vector<FrontendInfo> *all_fe_dai,
                       std::vector<SoundCardMixerCtl> *all_mixer_ctl);
        ~AweAudioPlugin();
        uint32_t DcWrite(const struct CtlElemPayload &ctl_elem_payload) override;
        int DcRead(struct CtlElemPayload &ctl_elem_payload) override;
        uint32_t StreamSetParam(string fe_dai_name,struct PCMSetParams &set_param_payload) override;
        uint32_t StreamPrepare(string fe_dai_name) override;
        uint32_t StreamRelease(string fe_dai_name) override;
        uint32_t StreamStart(string fe_dai_name) override;
        uint32_t StreamStop(string fe_dai_name) override;
        uint32_t StreamWrite(struct virtio_snd_pcm_buf_msg &msg) override;
        int StreamRead(struct virtio_snd_pcm_buf_msg &msg) override;
        uint32_t StreamMmap(string fe_dai_name, struct PCMMmapParams &msg) override;
        int RegisterSSREvent(std::function<void(enum SSRState)> ssr_cb_func) override;
        uint32_t InitClassResource() override;
        void ReleaseClassResource() override;
        int RegisterEventCallback(std::function<void(uint32_t control_id)> event_cb_func) override;
        AweCtrlValue* GetAweCtrlFromName(const char *ctrl_name);

        // function object to call after event notification
        std::function<void(uint32_t control_id)> adm_event_cb;
        std::function<void(enum SSRState)> adm_ssr_cb;
    private:
        uint32_t AweSetNamedControl(AweCtrlValue *AweCtrl, const CtlElemPayload &ctl_elem_payload, awe_control_type_t type);
        uint32_t AweGetNamedControl(AweCtrlValue *AweCtrl, const CtlElemPayload &ctl_elem_payload, awe_control_type_t type);
        int32_t AweShmemMapSMMU(AweCtrlValue *AweCtrl, awe_control_type_t type);
        int32_t AweShmemUnMapSMMU(AweCtrlValue *AweCtrl, awe_control_type_t type);
        uint32_t AweGetCoreErrorInfo(AweCtrlValue *AweCtrl);
        std::unordered_map<string, AweStream*> fe_dai_awe_map_;
        std::unordered_map<string, AweCtrlValue*> kctrl_value_map_;
        vector<SoundCardMixerCtl> *all_mixer_ctl_;
        std::vector<AweShmemMapInfo> shmem_map_info;
        uint32_t awe_snd_card_handle_;
};
#endif
