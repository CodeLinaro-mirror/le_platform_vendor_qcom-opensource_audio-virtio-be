/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#ifndef AGMAUDIOPLUGIN_H_
#define AGMAUDIOPLUGIN_H_

#include "audio_plugin_interface.h"
#include <optional>
#include "agm/agm_api.h"
#include "audio_log_util.h"

struct AGMsession {
  string                               fe_dai_name;
  string                               be_dai_name;
  int                                  ctl_val_ = 0;
  uint32_t                             agm_session_id;
  uint64_t                             agm_handle;
  uint64_t                             timestamp = 0;  // in microseconds
  struct agm_buffer_config             buffer_config;
  struct agm_media_config              media_config;
  struct agm_session_config            session_config;
  int                                  push_pull_data_gvm_fd = -1;
  int                                  push_pull_pos_gvm_fd = -1;
  void*                                agm_ap_instance;
  void*                                vaddr_dma_area;
  uint32_t                             dma_area_size;
};

enum ControlType {
    CONNECT,
    DISCONNECT,
    CONTROL,
    METADATA,
    SET_PARAM,
    SET_PARAM_TAG,
    SET_PARAM_TAG_ACDB,
    GET_TAGGED_INFO,
    EVENT,
    SET_CALIBRATION,
    GET_PARAM,
    GET_BUF_INFO,
    RATE_CH_FMT,
    SET_ACDB_TUNNEL,
    GET_ACDB_TUNNEL,
    LOOPBACK,
    ECHO_REFERENCE,
    BUF_TIMESTAMP,
    SIDETONE,
    DATAPATH_PARAMS,
    FLUSH,
    INVALID = 0xFFFF
};

class AGMAudioPlugin : public IAudioPlugin {
public:
    AGMAudioPlugin(std::vector<FrontendInfo>* all_fe_dai,
                    std::vector<MixerCtlInfo> *all_mixer_ctl_info);
    ~AGMAudioPlugin() override;

    uint32_t DcWrite(const CtlElemPayload& ctl_elem_payload) override;
    int DcRead(CtlElemPayload& ctl_elem_payload) override;
    uint32_t StreamSetParam(std::string fe_dai_name, PCMSetParams& set_param_payload) override;
    uint32_t SetParamForCapture(std::string fe_dai_name, PCMSetParams& set_param_payload) override;
    uint32_t StreamPrepare(std::string fe_dai_name) override;
    uint32_t StreamRelease(std::string fe_dai_name) override;
    uint32_t StreamStart(std::string fe_dai_name) override;
    uint32_t StreamStop(std::string fe_dai_name) override;
    uint32_t StreamWrite(struct virtio_snd_pcm_buf_msg& msg) override;
    uint32_t StreamWriteDone(struct virtio_snd_pcm_buf_msg &msg) override;
    uint32_t StreamRead(struct virtio_snd_pcm_buf_msg& msg) override;
    uint32_t StreamMmap(std::string fe_dai_name, PCMMmapParams& msg) override;
    int RegisterSSREvent(std::function<void(enum SSRState)> ssr_cb_func) override;
    int RegisterEventCallback(std::function<void(uint32_t control_id)> event_cb_func) override;
    void RegisterPcmMsgCallbackFunc(std::function<void(string &fe_dai_name,
          uint32_t msg_offset)> adm_cb_func) override;
    uint32_t InitClassResource() override;
    void ReleaseClassResource() override;
    std::function<void(string &fe_dai_name, uint32_t msg_offset)> adm_pcm_msg_cb_func;

    protected:
  // map to quickly look up agm session id
  unordered_map<string, AGMsession> fe_dai_session_id_map_;
  // map to look up agm aif id
  unordered_map<string, uint32_t> be_dai_aif_id_map_;
    // control_id is the vector index
  vector<MixerCtlInfo> *all_mixer_ctl_info_;
private:
      ControlType stringToControlType(const std::string& str);
      void debugMetaDataPrintout(std::string typeMetadata, uint32_t* metadata, uint32_t numElements);
};

#endif  // AGMAUDIOPLUGIN_H_