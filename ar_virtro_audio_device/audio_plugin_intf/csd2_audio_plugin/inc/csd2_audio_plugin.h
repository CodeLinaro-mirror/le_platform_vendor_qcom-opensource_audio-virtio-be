/**
 * @file csd2_audio_plugin.h
 * @brief Class CSD2AudioPlugin's definition
 */

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef CSD2_AUDIO_PLUGIN_H_
#define CSD2_AUDIO_PLUGIN_H_
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <chrono>

#include "audio_plugin_interface.h"
#include "csd2_types.h"
#include "csd2_ioctl.h"
#include "audio_log_util.h"

using namespace std;

struct PropData {
  uint32_t prop_id;
  uint32_t num_values;
  uint32_t *values;
};

enum CSD2StreamState {
  PCM_STATE_CLOSED = -1,
  PCM_STATE_OPEN,
  PCM_STATE_SETUP,
  PCM_STATE_PREPARED,
  PCM_STATE_RUNNING
};

struct CSD2BufPool {
  uint32_t num_buffs;
  uint32_t buf_size;
  std::mutex rw_mutex;
  std::condition_variable rw_cond;
};

struct CSD2Stream {
  string                               fe_dai_name;
  int                                  direction;
  SessModeType                         sess_mode;
  vector<string>                       be_dai_names;
  vector<string>                       supported_be_dai;
  int                                  ctl_val_ = 0;
  uint32_t                             csd2_handle;
  vector<pair<int,int>>                gkv_str;
  vector<pair<int,int>>                ckv_str;
  vector<pair<int,int>>                tkv_str;
  vector<pair<int,int>>                gkv_strdev;
  vector<pair<int,int>>                tkv_strdev;
  struct PropData                      stream_prop_id;
  struct PropData                      stream_dev_prop_id;
  enum CSD2StreamState                 stream_state;
  uint32_t                             stream_node_id = 0;
  uint32_t                             pp_mixer_node_id = 0;
  string                               loopback_id;
  std::mutex                           stream_mutex;
  std::condition_variable              stream_cond;
  bool                                 first_stream_read;
  uint32_t                             csd2_event;
  CSD2BufPool                          buf_pool;
  void*                                get_param_payload = nullptr;
  void*                                stream_params = nullptr;
  void*                                strdev_params = nullptr;
  void*                                tag_params = nullptr;
  void*                                csd2ap_instance = nullptr;
  int                                  push_pull_data_gvm_fd = -1;
  int                                  push_pull_pos_gvm_fd = -1;
  uint32_t                             rate = 0;
  uint32_t                             ch = 0;
  uint32_t                             format = 0;
  uint64_t                             timestamp = 0;  // in microseconds
};

struct CSD2Device {
  uint32_t                             rate = 0;
  uint32_t                             ch = 0;
  uint32_t                             format = 0;
  uint32_t                             data_fmt = 0;
  struct PropData                      dev_prop_id;
  vector<pair<int,int>>                gkv;
  vector<string>                       fe_dai_names;
  uint32_t                             device_node_id = 0;
  void*                                device_params = nullptr;
};

// Bit formats(same as agm_media_format)
enum AgmMediaFormat {
  kAgmFormatInvalid,
  kAgmFormatPcmS8,         // 8-bit signed
  kAgmFormatPcmS16LE,      // 16-bit signed
  kAgmFormatPcmS24LE,      // 24-bits in 4-bytes
  kAgmFormatPcmS243LE,     // 24-bits in 3-bytes
  kAgmFormatPcmS32LE,      // 32-bit signed
  kAgmFormatMP3,           // MP3 codec
  kAgmFormatAAC,           // AAC codec
  kAgmFormatFLAC,          // FLAC codec
  kAgmFormatALAC,          // ALAC codec
  kAgmFormatAPE,           // APE codec
  kAgmFormatWMAStd,        // WMA codec
  kAgmFormatWMAPro,        // WMA pro codec
  kAgmFormatVorbis,        // Vorbis codec
  kAgmFormatAMRNB,         // AMR NB codec
  kAgmFormatAMRWB,         // AMR WB codec
  kAgmFormatAMRWBPlus,     // AMR WB Plus codec
  kAgmFormatEVRC,          // EVRC codec
  kAgmFormatG711,          // G711 codec
  kAgmFormatQCELP,         // G711 codec
  kAgmFormatMax,
};

enum SetParamType {
  PARAM_TYPE_UNKNOWN,
  PARAM_TYPE_SET_PARAM,
  PARAM_TYPE_SET_PARAM_TAG,
  PARAM_TYPE_SET_PARAM_TAG_ACDB,
};

struct tag_config {
  uint32_t tag;
  uint32_t num_tkvs;
  struct csd2_key_value kv[];
};

struct acdb_param {
  bool isTKV;
  uint32_t tag;
  uint32_t num_kvs;  // number of tkv or ckv
  uint32_t blob_size; //kv size + payload size
  uint8_t blob[];     // kv + payload
};

class CSD2AudioPlugin : public IAudioPlugin {
 public:
  CSD2AudioPlugin(std::vector<FrontendInfo> *all_fe_dai,
                  std::vector<MixerCtlInfo> *all_mixer_ctl_info);
  ~CSD2AudioPlugin();
  uint32_t DcWrite(const struct CtlElemPayload &ctl_elem_payload) override;
  int DcRead(struct CtlElemPayload &ctl_elem_payload) override;
  uint32_t StreamSetParam(string fe_dai_name,
                          struct PCMSetParams &set_param_payload) override;
  uint32_t SetParamForCapture(string fe_dai_name, struct PCMSetParams &set_param_payload) override;
  uint32_t StreamPrepare(string fe_dai_name) override;
  uint32_t StreamRelease(string fe_dai_name) override;
  uint32_t StreamStart(string fe_dai_name) override;
  uint32_t StreamStop(string fe_dai_name) override;

  //need to review payload
  uint32_t StreamWrite(struct virtio_snd_pcm_buf_msg &msg) override;
  uint32_t StreamRead(struct virtio_snd_pcm_buf_msg &msg) override;

  uint32_t StreamMmap(string fe_dai_name, struct PCMMmapParams &msg) override;
  uint32_t InitClassResource() override;
  void ReleaseClassResource() override;
  void ClearSessionInfo();
  int RegisterSSREvent(std::function<void(enum SSRState)> ssr_cb_func) override;
  uint32_t IsFeDaiNameInvalid(const char *dai_name);
  void RegisterPcmMsgCallbackFunc(std::function<void(string &fe_dai_name,
          uint32_t msg_offset)> adm_cb_func) override;

  // function to call after SSR notification
  std::function<void(enum SSRState)> adm_ssr_cb;

  // function to call after buffer done event
  std::function<void(string &fe_dai_name, uint32_t msg_offset)> adm_pcm_msg_cb_func;

 protected:
  uint32_t pp_session_handle_ = 0;

  // map to quickly look up CSD2 session
  unordered_map<string, CSD2Stream*> fe_dai_csd2_map_;

  // be_dai_name to CSD2Device map
  unordered_map<string, CSD2Device> csd2_devices_;

  // control_id is the vector index
  vector<MixerCtlInfo> *all_mixer_ctl_info_;
  uint32_t ssr_session_handle = 0;

 private:

  uint32_t AnalyzePropData(uint8_t *metadatabuf,
      struct PropData *prop_data_node, int start_index);
  int AnalyzeKvData(uint8_t *metadatabuf, uint32_t *kv_pairs, int kv_num,
      int start_index);
  uint32_t AnalyzeGkvAndCkvData(uint8_t *metadatabuf, string dai_name,
      uint32_t &gkv_num, int &prop_index, int metadatatype);
  void AnalyzeStreamMetadata(uint8_t *metadatabuf, string dai_name);
  void AnalyzeStrdevMetadata(uint8_t *metadatabuf, string dai_name);
  void AnalyzeDeviceMetadata(uint8_t *metadatabuf, char *be_dai_name);
  uint32_t AnalyzeMetadataCkv(uint8_t *metadatabuf, string dai_name,
      uint32_t *ckv_num);
  uint32_t AssignChannelMapping(uint32_t chs, uint8_t channel_mapping[]);
  uint32_t SwEpConfig(uint32_t session_handle, uint32_t buffer_size,
      uint32_t buffer_num, uint32_t sw_out_rate, uint32_t sw_out_bits,
      uint32_t sw_out_chs, string fe_dai_name, uint8_t is_mmap_noirq);
  uint32_t SetStreamFormat(uint32_t session_handle, uint32_t sw_in_rate,
      uint32_t sw_in_bits, uint32_t sw_in_chs, uint32_t sw_out_rate,
      uint32_t sw_out_bits, uint32_t sw_out_chs);
  uint32_t HwEpConfig(uint32_t session_handle, uint32_t device,
                      uint32_t hw_rate, uint32_t hw_bits, uint32_t hw_chs);
  uint32_t HwEpConfig(uint32_t session_handle, uint32_t devices[2],
                      uint32_t rates[2], uint32_t bits[2],
                      uint32_t chs[2]);
  uint32_t FillGraphInfo(string dai_name, string be_dai_name,
                         struct csd2_graph_info *&graph_info);
  uint32_t AddGraphInfo(string dai_name, string be_dai_name,
                         struct csd2_graph_info *&graph_info);
  int GetPcmBitsPerSample(uint32_t fmt_id);
  uint32_t SetCalData(string dai_name, uint32_t ckv_num, struct csd2_key_value *ckv);
  uint32_t AssignNodeIndices(string dai_name, string be_dai_name,
                             uint32_t &nodes_num, int &stream_node_index,
                             int &pp_node_index, int &device_node_index);
  void DeviceChConfig(const uint32_t device_id, uint32_t &hw_chs);
  uint32_t WaitTimeoutMs(string fe_dai_name);
};
#endif  // CSD2_AUDIO_PLUGIN_H_
