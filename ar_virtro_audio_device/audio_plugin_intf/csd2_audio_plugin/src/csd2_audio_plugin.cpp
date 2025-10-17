/**
 * @file csd2_audio_plugin.cpp
 * @brief Class CSD2AudioPlugin's definition
 */

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "csd2_audio_plugin.h"

#include "csd2.h"
#include "csd2_media_format.h"
#include "csd2_oem_types.h"
#include "csd2_platform_info.h"
#include "csd2_internal_api.h"
#include "csd2_status.h"

#define IAUD_EOK                                  0
#define IAUD_EFAILED                              1
#define IAUD_ENORESOURCE                          2
#define IAUD_EGKVERROR                            3
#define IAUD_ECKVERROR                            4
#define IAUD_EINVAL                               5
#define PERIOD_SIZE                               3072
#define ARRAY_SIZE(a) \
  (sizeof(a)/sizeof(a[0]))
#define TAG_RIFF                                  0x46464952
#define TAG_WAVE                                  0x45564157
#define TAG_FMT                                   0x20746d66
#define TAG_DATA                                  0x61746164
#define DATA_EVENT_TAG_RD_SH_MEM_EP_MEDIA_FORMAT  0x06001000
#define DATA_EVENT_TAG_RD_SH_MEM_EP_EOS           0x06001001
#define STREAM_METADATA                           1
#define STREAM_DEVICE_METADATA                    2
#define DEVICE_METADATA                           3

#define CSD2_EOS_EVENT                            0x1
#define PARAM_ID_PCM_ENCODER_FRAME_SIZE           0x0800136B

#define CSD2_AP_ALIGN_8BYTE(x)                    (((x) + 7) & (~7))

struct param_id_pcm_encoder_frame_size_t {
  uint32_t frame_size_type;
  uint32_t frame_size_in_samples;
  uint32_t frame_size_in_us;
};

struct pcm_push_pull_position_buffer {
  volatile uint32_t frame_counter;
  volatile uint32_t index;
  volatile uint32_t timestamp_us_lsw;
  volatile uint32_t timestamp_us_msw;
};

// based on supported PCM frame rates defined in virtio_snd.h
const std::map<uint32_t, uint32_t> v2csd_rate_map = {
  {VIRTIO_SND_PCM_RATE_5512, 5512},
  {VIRTIO_SND_PCM_RATE_8000, 8000},
  {VIRTIO_SND_PCM_RATE_11025, 11025},
  {VIRTIO_SND_PCM_RATE_12000, 12000},
  {VIRTIO_SND_PCM_RATE_16000, 16000},
  {VIRTIO_SND_PCM_RATE_22050, 22050},
  {VIRTIO_SND_PCM_RATE_24000, 24000},
  {VIRTIO_SND_PCM_RATE_32000, 32000},
  {VIRTIO_SND_PCM_RATE_44100, 44100},
  {VIRTIO_SND_PCM_RATE_48000, 48000},
  {VIRTIO_SND_PCM_RATE_64000, 64000},
  {VIRTIO_SND_PCM_RATE_88200, 88200},
  {VIRTIO_SND_PCM_RATE_96000, 96000},
  {VIRTIO_SND_PCM_RATE_176400, 176400},
  {VIRTIO_SND_PCM_RATE_192000, 192000},
  {VIRTIO_SND_PCM_RATE_384000, 384000}
};

// based on PCM sample formats defined in virtio_snd.h
const std::map<uint32_t, uint32_t> v2csd_fmt_map = {
  {VIRTIO_SND_PCM_FMT_IMA_ADPCM, 4},
  {VIRTIO_SND_PCM_FMT_MU_LAW, 8},
  {VIRTIO_SND_PCM_FMT_A_LAW, 8},
  {VIRTIO_SND_PCM_FMT_S8, 8},
  {VIRTIO_SND_PCM_FMT_U8, 8},
  {VIRTIO_SND_PCM_FMT_S16, 16},
  {VIRTIO_SND_PCM_FMT_U16, 16},
  {VIRTIO_SND_PCM_FMT_S18_3, 24},
  {VIRTIO_SND_PCM_FMT_U18_3, 24},
  {VIRTIO_SND_PCM_FMT_S20_3, 24},
  {VIRTIO_SND_PCM_FMT_U20_3, 24},
  {VIRTIO_SND_PCM_FMT_S24_3, 24},
  {VIRTIO_SND_PCM_FMT_U24_3, 24},
  {VIRTIO_SND_PCM_FMT_S20, 32},
  {VIRTIO_SND_PCM_FMT_U20, 32},
  {VIRTIO_SND_PCM_FMT_S24, 32},
  {VIRTIO_SND_PCM_FMT_U24, 32},
  {VIRTIO_SND_PCM_FMT_S32, 32},
  {VIRTIO_SND_PCM_FMT_U32, 32},
  {VIRTIO_SND_PCM_FMT_FLOAT, 32},
  {VIRTIO_SND_PCM_FMT_FLOAT64, 64},
  {VIRTIO_SND_PCM_FMT_DSD_U8, 8},
  {VIRTIO_SND_PCM_FMT_DSD_U16, 16},
  {VIRTIO_SND_PCM_FMT_DSD_U32, 32},
  {VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME, 32}
};

CSD2AudioPlugin::CSD2AudioPlugin(vector<FrontendInfo> *all_fe_dai,
    vector<struct MixerCtlInfo> *all_mixer_ctl_info)
    : IAudioPlugin(all_fe_dai), all_mixer_ctl_info_(all_mixer_ctl_info) {
    if (all_fe_dai != nullptr &&
        all_mixer_ctl_info != nullptr) {
        InitClassResource();
    }
}

CSD2AudioPlugin::~CSD2AudioPlugin() {
    ClearSessionInfo();
    ReleaseClassResource();
    csd2_close(ssr_session_handle);
}

static inline int32_t csd2ap_mem_cpy(void *dst, size_t dst_size, const void *src, size_t src_size)
{
  if (src == nullptr ||
      dst == nullptr ||
      dst_size == 0  ||
      src_size == 0) {
    return -IAUD_EINVAL;
  }

  size_t copy_size = (dst_size <= src_size) ? dst_size : src_size;
  memcpy(dst, src, copy_size);

  return copy_size;
}

static void ReplaceValueForKey(std::vector<std::pair<int,int>> &vec, int keyToFind, int newVal) {
  for (auto &it : vec) {
    if (it.first == keyToFind) {
      it.second = newVal;
      return;
    }
  }

  AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "key not found");
  return;
}

// map bit width and sample rate from virtio to csd
static uint32_t MapHwParamsBwSr(uint32_t &sw_in_bits, uint32_t &sw_in_rate,
    struct PCMSetParams &set_param_payload) {

  if (v2csd_fmt_map.find(set_param_payload.format) != v2csd_fmt_map.end())
    sw_in_bits = v2csd_fmt_map.at(set_param_payload.format);

  if (v2csd_rate_map.find(set_param_payload.rate) != v2csd_rate_map.end())
    sw_in_rate = v2csd_rate_map.at(set_param_payload.rate);

  if (sw_in_rate == numeric_limits<uint32_t>::max() ||
      sw_in_bits == numeric_limits<uint32_t>::max())
    return -EINVAL;

  return IAUD_EOK;
}

// check the validity of the fe dai names
uint32_t CSD2AudioPlugin::IsFeDaiNameInvalid(const char *dai_name) {
  // PCM_DUMMY-TX-0 is intf for echo-ref-record, not fe dai name, check it first.
  if (dai_name != nullptr &&
      strncmp(dai_name, "PCM_DUMMY-TX-0", strlen("PCM_DUMMY-TX-0")) == 0)
    return IAUD_EINVAL;

  if(dai_name != nullptr &&
     (strstr(dai_name, "PCM") != nullptr ||
     strstr(dai_name, "COMPRESS") != nullptr ||
     strstr(dai_name, "VOICE") != nullptr ||
     strstr(dai_name, "ExtEC") != nullptr) &&
     fe_dai_csd2_map_.find(dai_name) != fe_dai_csd2_map_.end())
     return IAUD_EOK;

  return IAUD_EINVAL;
}

// call to set calibration data
uint32_t CSD2AudioPlugin::SetCalData(string dai_name, uint32_t ckv_num,
    struct csd2_key_value *ckv) {
  uint32_t rc = IAUD_EOK;
  struct csd2_cal_config cal_config;
  struct csd2_graph_info graph_info = {0};
  uint32_t session_handle;

  if (ckv == nullptr ||
      dai_name.empty() ||
      fe_dai_csd2_map_.find(dai_name) == fe_dai_csd2_map_.end() ||
      !fe_dai_csd2_map_.at(dai_name)->csd2_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "SetCalData invalid args for dai_name: %s", dai_name.c_str());
    return IAUD_EINVAL;
  }

  cal_config.num_kvs = ckv_num;
  cal_config.cal_key_vector = ckv;
  cal_config.graph_info = &graph_info;
  session_handle = fe_dai_csd2_map_.at(dai_name)->csd2_handle;

  rc = csd2_ioctl(session_handle, CSD2_CMD_SET_CAL, &cal_config, sizeof(cal_config));
  if (rc != IAUD_EOK)
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_ioctl set calibration failed");

  return rc;
}

static void RemoveDuplicateGkv(vector<pair<int,int>> &gkv)
{
  set<pair<int,int>> seen;

  gkv.erase(remove_if(gkv.begin(), gkv.end(), [&seen](pair<int,int> gkv_pair){
            return !seen.insert(gkv_pair).second;}), gkv.end());
}

static void FillMetadata(struct csd2_key_value* output_kv,
            vector<pair<int,int>> input_gkv) {

  int i = 0;
  for (auto iter : input_gkv) {
    output_kv[i].key = iter.first;
    output_kv[i].value = iter.second;
    i++;
  }
}

static int SetParamToCSD2(uint32_t pp_session_handle, vector<pair<int,int>> &gkv,
                          struct csd2_pp_control_spf_custom_param* payload) {
  int rc = IAUD_EOK;
  struct csd2_pp_param_custom_config pp_param;

  pp_param.gkv.num_kvs = gkv.size();
  pp_param.gkv.kv = static_cast<struct csd2_key_value*>(calloc(1, gkv.size() * sizeof(struct csd2_key_value)));
  if (!pp_param.gkv.kv) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for gkv");
    return IAUD_ENORESOURCE;
  }

  FillMetadata(pp_param.gkv.kv, gkv);
  pp_param.custom_param = payload;

  rc = csd2_ioctl(pp_session_handle, CSD2_CMD_PP_CONTROL_SET_CUSTOM_CONFIG,
                  &pp_param, sizeof(struct csd2_pp_param_custom_config));
  if (rc != IAUD_EOK)
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_ioctl pp control set custom config failed");

  free(pp_param.gkv.kv);

  return rc;
}

static int SetTaggedParamToCSD2(uint32_t pp_session_handle, vector<pair<int,int>> &gkv, struct tag_config* payload) {
  int rc = IAUD_EOK;
  struct csd2_pp_param_config tagged_param;

  tagged_param.gkv.num_kvs = gkv.size();
  tagged_param.gkv.kv = static_cast<struct csd2_key_value*>(calloc(1, gkv.size() * sizeof(struct csd2_key_value)));
  if (!tagged_param.gkv.kv) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for gkv");
    return IAUD_ENORESOURCE;
  }

  FillMetadata(tagged_param.gkv.kv, gkv);
  tagged_param.is_persist = false;
  tagged_param.mode = CSD2_PP_CFG_MODE_TKV;
  tagged_param.tag = payload->tag;
  tagged_param.u.tkv.num_kvs = payload->num_tkvs;
  tagged_param.u.tkv.kv = payload->kv;

  rc = csd2_ioctl(pp_session_handle, CSD2_CMD_PP_CONTROL_SET, &tagged_param, sizeof(struct csd2_pp_param_config));
  if (rc != IAUD_EOK)
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_ioctl pp control set tagged param failed");

  free(tagged_param.gkv.kv);

  return rc;
}

static int SetTaggedParamToACDB(uint32_t pp_session_handle, vector<pair<int,int>> &gkv, struct acdb_param* payload) {
  int rc = IAUD_EOK;
  struct csd2_cmd_pp_set_data_to_acdb_info pp_param;

  pp_param.gkv.num_kvs = gkv.size();
  pp_param.gkv.kv = static_cast<struct csd2_key_value*>(calloc(1, gkv.size() * sizeof(struct csd2_key_value)));
  if (!pp_param.gkv.kv) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for gkv");
    return IAUD_ENORESOURCE;
  }

  FillMetadata(pp_param.gkv.kv, gkv);
  pp_param.isTKV = payload->isTKV;
  pp_param.tag = payload->tag;
  pp_param.tckv.num_kvs = payload->num_kvs;
  pp_param.tckv.kv = static_cast<struct csd2_key_value*>(calloc(1, payload->num_kvs * sizeof(struct csd2_key_value)));
  if (!pp_param.tckv.kv) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for tkv/ckv");
    free(pp_param.gkv.kv);
    return IAUD_ENORESOURCE;
  }

  rc = csd2ap_mem_cpy((uint8_t*)pp_param.tckv.kv, sizeof(pp_param.tckv.kv),
                  payload->blob, sizeof(pp_param.tckv.kv));
  if (rc < 0 || rc < sizeof(pp_param.tckv.kv)) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "csd2ap_mem_cpy failed");
    goto end;
  }

  rc = IAUD_EOK;

  pp_param.custom_param = (struct csd2_pp_control_spf_custom_param*)
                          (payload->blob + payload->num_kvs * sizeof(struct csd2_key_value));

  rc = csd2_ioctl(pp_session_handle, CSD2_CMD_PP_SET_TAG_TO_ACDB,
                  &pp_param, sizeof(struct csd2_cmd_pp_set_data_to_acdb_info));
  if (rc != IAUD_EOK)
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "csd2_ioctl pp set tag to acdb failed");

end:
  free(pp_param.tckv.kv);
  free(pp_param.gkv.kv);

  return rc;
}

uint32_t CSD2AudioPlugin::WaitTimeoutMs(string fe_dai_name) {

  // Timeout := (1000 * 2 * buffer_size [bytes] * 8 [bits/byte]) /
  //            (bit_rate [bits/sec])
  // where bit_rate := (sample_rate [sample/sec] * num_ch * bit_format [bits/sample])
  // and the multiplier 2 is used as a margin, and 1000 is to convert to ms

  uint32_t min_timeout = 60;
  uint32_t calc_timeout = 0;

  calc_timeout =
      (1000 * 2 * fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.buf_size * fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.num_buffs * 8) /
      (fe_dai_csd2_map_.at(fe_dai_name)->rate * fe_dai_csd2_map_.at(fe_dai_name)->ch * fe_dai_csd2_map_.at(fe_dai_name)->format);

  return max(min_timeout, calc_timeout);
}

static uint32_t SSRCallbackFunc(enum csd2_global_event_ids event_id, void *event_payload,
                            size_t event_payload_sz, void *client_data) {
  enum SSRState ssr_state = SSR_STATE_UNKNOWN;
  CSD2AudioPlugin *csd2_plug  = (CSD2AudioPlugin*)client_data;

  switch(event_id)
  {
  case CSD2_GLOBAL_EVENT_AUDIO_SVC_DN:
    ssr_state = SSR_STATE_DOWN;
    break;
  case CSD2_GLOBAL_EVENT_AUDIO_SVC_UP:
    ssr_state = SSR_STATE_UP;
    break;
  default:
    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Unsupported event_id %d", event_id);
    return 0;
  }

  csd2_plug->ClearSessionInfo();
  csd2_plug->adm_ssr_cb(ssr_state);
  return 0;
}

// This func should be called after constructor, returns an integer to
// indicate whether initialization was successful. If initialization
// fails, should call the destructor after this func.
uint32_t CSD2AudioPlugin::InitClassResource() {
  uint32_t rc;
  struct csd2_init_data init_data = {0};
  struct csd2_open_config open_cfg = {0};

  // init fe_dai_csd2_map_
  for (auto iter : *all_fe_dai_) {
    CSD2Stream *stream_node = static_cast<struct CSD2Stream*>(calloc(1, sizeof(CSD2Stream)));
    if (stream_node == nullptr) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "alloc CSD2Stream for fe_dai_csd2_map err");
      goto fail;
    }
    stream_node->fe_dai_name = iter.name;
    stream_node->direction = iter.direction;
    stream_node->sess_mode = iter.sess_mode;
    stream_node->push_pull_data_gvm_fd = -1;
    stream_node->push_pull_pos_gvm_fd = -1;
    pair<string, CSD2Stream*> element(iter.name, stream_node);
    fe_dai_csd2_map_.insert(element);
  }

  /* for PCM control mixer controls, cache the enum_mapping in the corresponding
     CSD2Stream struct. This is later used to find the device name based on the ctl_val_
     and access the csd2_devices_ struct for the device gkv.
  */
  for (auto iter : *all_mixer_ctl_info_) {
    // Extract the dai_name and control name
    char *saveptr = nullptr;
    char *dai_name = strtok_r((char *)iter.name.c_str(), " ", &saveptr);
    char *pcontrol = strtok_r(nullptr, " ", &saveptr);

    if ((dai_name == nullptr) || (pcontrol == nullptr)) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "DAI name or pcontrol is empty");
      return IAUD_EFAILED;
    }

    if (strncmp(pcontrol, "control", strlen("control")) == 0)
    {
      if (IsFeDaiNameInvalid(dai_name)) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Invalid fe dai_name %s, failing", dai_name);
        goto fail;
      }

      fe_dai_csd2_map_.at(dai_name)->supported_be_dai = iter.enum_mapping;
    }
  }
  // call csd2_init
  rc = csd2_init(&init_data);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2 init failure");
    goto fail;
  }

  rc = csd2_open(CSD2_MODULE_TYPE_PP_CONTROL, &open_cfg, sizeof(open_cfg), &pp_session_handle_);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_pp_open failure");
    goto fail;
  }

  return rc;

fail:
  for (auto iter : fe_dai_csd2_map_) {
    if (iter.second != nullptr)
      free(iter.second);
  }
  fe_dai_csd2_map_.clear();

  rc = IAUD_EFAILED;
  return rc;
}

// This func should be called before destructor, no return value.
void CSD2AudioPlugin::ReleaseClassResource() {
  // call csd2_deinit
  csd2_deinit();

  // clear fe_dai_csd2_map_
  for (auto iter : fe_dai_csd2_map_) {
    if (iter.second->stream_prop_id.values != nullptr)
      free(iter.second->stream_prop_id.values);

    if (iter.second->stream_dev_prop_id.values != nullptr)
      free(iter.second->stream_dev_prop_id.values);

    if (iter.second->get_param_payload != nullptr)
      free(iter.second->get_param_payload);

    if (iter.second->stream_params != nullptr)
      free(iter.second->stream_params);

    if (iter.second->strdev_params != nullptr)
      free(iter.second->strdev_params);

    if (iter.second->tag_params != nullptr)
      free(iter.second->tag_params);

    if (iter.second != nullptr)
      free(iter.second);

    if (iter.second->csd2ap_instance != nullptr)
      iter.second->csd2ap_instance = nullptr;
  }

  fe_dai_csd2_map_.clear();

  // clear csd2_devices_
  for (auto iter : csd2_devices_) {
    if (iter.second.dev_prop_id.values != nullptr)
      free(iter.second.dev_prop_id.values);
  }

  csd2_devices_.clear();
}

void CSD2AudioPlugin::RegisterPcmMsgCallbackFunc(std::function<void(string &fe_dai_name,
        uint32_t msg_offset)> adm_cb_func) {

  adm_pcm_msg_cb_func = adm_cb_func;
}

int CSD2AudioPlugin::RegisterSSREvent(std::function<void(enum SSRState)> ssr_func) {
  struct csd2_reg_global_event reg_event = {0};
  struct csd2_ssr_client_config ssr_cfg = {"csd2ap", 0};
  uint32_t rc = IAUD_EOK;

  rc = csd2_open(CSD2_MODULE_TYPE_SSR, (void*)&ssr_cfg, sizeof(ssr_cfg), &ssr_session_handle);
  if (rc) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_open failed for SSR module");
    return rc;
  }

  reg_event.global_event_cb = SSRCallbackFunc;
  reg_event.client_data = (void*)this;
  rc = csd2_ioctl(ssr_session_handle, CSD2_CMD_REG_GLOBAL_EVENT, &reg_event, sizeof(reg_event));
  if (rc) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_ioctl SSR register failed");
    return rc;
  }

  adm_ssr_cb = ssr_func;
  return IAUD_EOK;
}


// We get propdata from 'metadatabuf', data behind 'start_index' is propdata.
// The data format of propdata is : prop_id + num_values + values[num_values].
// example 1: 0x10 0x00 0x00 0x08 0x01 0x00 0x00 0x00 0x01 0x00 0x00 0x00
// example 2: 0x10 0x00 0x00 0x08 0x02 0x00 0x00 0x00 0x02 0x00 0x00 0x00 0x05 0x00 0x00 0x00
// For example 1 prop_id = 0x08000010, num_values = 0x01, values[0] = 0x01.
// For example 2 prop_id = 0x08000010, num_values = 0x02, values[0] = 0x02, values[1] = 0x05.
uint32_t CSD2AudioPlugin::AnalyzePropData(uint8_t *metadatabuf,
    struct PropData *prop_data_node, int start_index) {
  uint32_t rc = IAUD_EOK;
  int begin = start_index;

  if (metadatabuf == nullptr ||
      prop_data_node == nullptr ||
      start_index < 0) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "AnalyzePropData Invalid args");
    return IAUD_EINVAL;
  }

  uint32_t prop_id = ((uint32_t)metadatabuf[begin + 3] << 24) |
                      ((uint32_t)metadatabuf[begin + 2] << 16) |
                      ((uint32_t)metadatabuf[begin + 1] << 8) |
                      (uint32_t)metadatabuf[begin];
  uint32_t num_values = ((uint32_t)metadatabuf[begin + 7] << 24) |
                         ((uint32_t)metadatabuf[begin + 6] << 16) |
                         ((uint32_t)metadatabuf[begin + 5] << 8) |
                         (uint32_t)metadatabuf[begin + 4];
  begin += 8;  // 8 is the size of prop_id and num_values
  prop_data_node->prop_id = prop_id;
  prop_data_node->num_values = num_values;
  prop_data_node->values = static_cast<uint32_t*>(calloc(num_values, sizeof(uint32_t)));
  if (prop_data_node->values == nullptr) {
    rc = IAUD_ENORESOURCE;
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "allocate value array for PropData fail");
    return rc;
  }

  for (int i = 0; i < num_values; i++) {
    prop_data_node->values[i] = ((uint32_t)metadatabuf[begin + 3] << 24) |
                                ((uint32_t)metadatabuf[begin + 2] << 16) |
                                ((uint32_t)metadatabuf[begin + 1] << 8) |
                                 (uint32_t)metadatabuf[begin];
    begin += 4;  // 4 is the size of value[i]
  }

  return rc;
}

// We get 'kv_pairs' from 'metadatabuf', data behind 'start_index' is kv data,
// 'kv_num' indicates the number of kv pairs. Returns an integer, the return
// value is equal to end index of kv data plus one. The data format of kv data
// is: key1 + value1 + key2 + value2 + ...
// example: 0x00 0x00 0x00 0xa1 0x15 0xf0 0x00 0xa1
// For the example key = 0xa1000000, value = 0xa100f015,
// so the kv_pairs[0] = 0xa1000000, kv_pairs[1] = 0xa100f015.
int CSD2AudioPlugin::AnalyzeKvData(uint8_t *metadatabuf, uint32_t *kv_pairs,
                                   int kv_num, int start_index) {
  int begin = start_index;
  uint32_t key;
  uint32_t value;

  if (metadatabuf == nullptr ||
      kv_pairs == nullptr ||
      start_index < 0 ||
      kv_num < 0) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "AnalyzeKvData Invalid args");
    return IAUD_EINVAL;
  }

  for (int i = 0; i < kv_num; i++) {
    key = ((uint32_t)metadatabuf[begin + 3] << 24) |
          ((uint32_t)metadatabuf[begin + 2] << 16) |
          ((uint32_t)metadatabuf[begin + 1] << 8) |
          (uint32_t)metadatabuf[begin];
    value = ((uint32_t)metadatabuf[begin + 7] << 24) |
            ((uint32_t)metadatabuf[begin + 6] << 16) |
            ((uint32_t)metadatabuf[begin + 5] << 8) |
            (uint32_t)metadatabuf[begin + 4];

    begin += 8;  // 8 is the size of key and value
    kv_pairs[i * 2] = key;
    kv_pairs[i * 2 + 1] = value;
  }

  return begin;
}

uint32_t CSD2AudioPlugin::AnalyzeGkvAndCkvData(uint8_t *metadatabuf,
    string dai_name, uint32_t &gkv_num, int &prop_index,
    int metadatatype) {
  int i;

  if (metadatabuf == nullptr ||
      dai_name.empty() ||
      (metadatatype == DEVICE_METADATA &&
      csd2_devices_.find(dai_name) == csd2_devices_.end()) ||
     (metadatatype != DEVICE_METADATA &&
      fe_dai_csd2_map_.find(dai_name) == fe_dai_csd2_map_.end())) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "AnalyzeGkvAndCkvData invalid args for dai_name: %s", dai_name.c_str());
    return IAUD_EINVAL;
  }

  // gkv analysis
  gkv_num = ((uint32_t)metadatabuf[3] << 24) |
             ((uint32_t)metadatabuf[2] << 16) |
             ((uint32_t)metadatabuf[1] << 8) |
             (uint32_t)metadatabuf[0];
  if (gkv_num <= 0) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "no gkv data");
    return IAUD_EFAILED;
  }
  uint32_t *gkv_pairs = static_cast<uint32_t*>(calloc(gkv_num * 2, sizeof(uint32_t)));
  if (gkv_pairs == nullptr) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "AnalyzeGkvAndCkvData calloc failure");
    return IAUD_EFAILED;
  }
  int ckv_index = AnalyzeKvData(metadatabuf, gkv_pairs, gkv_num, 4);
  if (metadatatype == STREAM_METADATA) {
    for (i = 0; i < gkv_num; i++) {
      // not to create a new pair for stream device KVs if they are part of stream meta data
      if (!fe_dai_csd2_map_.at(dai_name)->gkv_strdev.empty()
        && (gkv_pairs[i*2] == DEVICEPP_RX || gkv_pairs[i*2] == DEVICEPP_TX)) {
        ReplaceValueForKey(fe_dai_csd2_map_.at(dai_name)->gkv_strdev,
            gkv_pairs[i*2], gkv_pairs[i*2+1]);
        continue;
      }
      fe_dai_csd2_map_.at(dai_name)->gkv_str.push_back(
          make_pair(gkv_pairs[i * 2], gkv_pairs[i * 2 + 1]));
    }
    fe_dai_csd2_map_.at(dai_name)->gkv_str.push_back(
        make_pair(0xff000000, 0x00000001)); // VMID, LA_GVM_1
    gkv_num += 1;
  }

  if (metadatatype == STREAM_DEVICE_METADATA) {
    for (i = 0; i < gkv_num; i++) {
      // not to create a new pair for stream KVs if they are part of stream device meta data
      if (!fe_dai_csd2_map_.at(dai_name)->gkv_str.empty()
        && (gkv_pairs[i*2] == STREAMRX || gkv_pairs[i*2] == STREAMTX)) {
        ReplaceValueForKey(fe_dai_csd2_map_.at(dai_name)->gkv_str,
            gkv_pairs[i*2], gkv_pairs[i*2+1]);
        continue;
      }
      fe_dai_csd2_map_.at(dai_name)->gkv_strdev.push_back(
          make_pair(gkv_pairs[i * 2], gkv_pairs[i * 2 + 1]));
    }
  }

  if (metadatatype == DEVICE_METADATA) {
    for (i = 0; i < gkv_num; i++) {
      csd2_devices_.at(dai_name).gkv.push_back(
          make_pair(gkv_pairs[i * 2], gkv_pairs[i * 2 + 1]));
    }
  }

  // ckv analysis
  uint32_t ckv_num = ((uint32_t)metadatabuf[ckv_index + 3] << 24) |
                      ((uint32_t)metadatabuf[ckv_index + 2] << 16) |
                      ((uint32_t)metadatabuf[ckv_index + 1] << 8) |
                      (uint32_t)metadatabuf[ckv_index];
  if (ckv_num > 0) {
    uint32_t ckv_pairs[ckv_num * 2];
    prop_index = AnalyzeKvData(metadatabuf, &ckv_pairs[0],
                               ckv_num, ckv_index + 4);
      for (i = 0; i < ckv_num; i++) {
        fe_dai_csd2_map_.at(dai_name)->ckv_str.push_back(
            make_pair(ckv_pairs[i * 2], ckv_pairs[i * 2 + 1]));
      }
  } else {  // no ckv data
    prop_index = ckv_index + 4;
  }

  free(gkv_pairs);

  return IAUD_EOK;
}

// Analyze stream metadata to get gkv/ckv/propdata, and get stream_node_id.
// The data format of stream metadata('metadatabuf') is:
// gkv_num + key[i] + value[i] + (ckv_num + key[i] +
// value[i]) + prop_id + num_value + values[num_value].
// example 1: 0x01 0x00 0x00 0x00 0x00 0x00 0x00 0xa1 0x15 0xf0 0x00 0xa1
//            0x01 0x00 0x00 0x00 0x00 0x00 0x00 0xa4 0x0f 0x00 0x00 0x00
//            0x10 0x00 0x00 0x08 0x01 0x00 0x00 0x00 0x01 0x00 0x00 0x00
// example 2: 0x01 0x00 0x00 0x00 0x00 0x00 0x00 0xa1 0x15 0xf0 0x00 0xa1
//            0x00 0x00 0x00 0x00 0x10 0x00 0x00 0x08 0x01 0x00 0x00 0x00
//            0x01 0x00 0x00 0x00
// For example 1 gkv_num = 1, gkv_pairs[0] = 0xa1000000, gkv_pairs[1] = 0xa100f015,
// ckv_num = 1, ckv_pairs[0] = 0xa4000000, ckv_pairs[1] = 0x0000000f,
// prop_id = 0x08000010, num_value = 1, values[0] = 1.
// For example 2 gkv_num = 1, gkv_pairs[0] = 0xa1000000, gkv_pairs[1] = 0xa100f015,
// prop_id = 0x08000010, num_value = 1, values[0] = 1.
void CSD2AudioPlugin::AnalyzeStreamMetadata(uint8_t *metadatabuf,
    string dai_name) {
  // gkv and ckv analysis
  int i;
  int j;
  uint32_t rc;
  uint32_t gkv_num;
  int prop_index;

  if (metadatabuf == nullptr ||
      dai_name.empty()) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "Invalid argument for dai_name %s", dai_name.c_str());
    return;
  }

  rc = AnalyzeGkvAndCkvData(metadatabuf, dai_name, gkv_num, prop_index, STREAM_METADATA);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "AnalyzeGkvAndCkvData for stream metadata fail");
    return;
  }

  // prop data analysis
  rc = AnalyzePropData(metadatabuf,
      &fe_dai_csd2_map_.at(dai_name)->stream_prop_id, prop_index);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "propdata analysis for stream metadata fail");
    return;
  }

  // get stream_node_id
  // stream_nodes defined in csd2_platform_info.h,
  // loop through stream_nodes to find the correct node by comparing kv data.
  for (i = 0; i < ARRAY_SIZE(stream_nodes); i++) {
    if ((stream_nodes[i].sgkv.num_kvps) == gkv_num) {
      for (j = 0; j < gkv_num; j++) {
        if ((stream_nodes[i].sgkv.kvp[j].key != fe_dai_csd2_map_.at(dai_name)->gkv_str[j].first) ||
            (stream_nodes[i].sgkv.kvp[j].value != fe_dai_csd2_map_.at(dai_name)->gkv_str[j].second)) {
          break;
        }
      }
      if (j == gkv_num) {
        fe_dai_csd2_map_.at(dai_name)->stream_node_id = stream_nodes[i].id;
        break;
      }
    }
  }

  // can't find corresponding stream_node_id in stream_nodes
  if (i == ARRAY_SIZE(stream_nodes)) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "Couldn't find the correct stream_node_id for the given KVs");
  }

  return;
}

// Analyze stream device metadata to get gkv/ckv/propdata, and get device_node_id
// and pp_mixer_node_id. The data format of strdev_metadata
// ('metadatabuf') is: gkv_num + key[i] + value[i] + (ckv_num + key[i] +
// value[i]) + prop_id + num_value + values[num_value].
// example:0x02 0x00 0x00 0x00 0x00 0x00 0x00 0xa2 0x01 0x00 0x00 0xa2
//         0x00 0x00 0x00 0xac 0x06 0x00 0x00 0xac 0x01 0x00 0x00 0x00
//         0x00 0x00 0x0 0xd2 0x00 0x00 0x00 0x00 0x10 0x00 0x00 0x08
//         0x01 0x00 0x00 0x00 0x03 0x00 0x00 0x00
// For the example gkv_num = 2, gkv_pairs[0] = 0xa2000000, gkv_pairs[1] = 0xa2000001,
// gkv_pairs[3] = 0xac000000, gkv_pairs[4] = 0xac000006,
// ckv_num = 1, ckv_pairs[0] = 0xd2000000, ckv_pairs[1] = 0,
// prop_id = 0x08000010, num_value = 1, values[0] = 3.
void CSD2AudioPlugin::AnalyzeStrdevMetadata(uint8_t *metadatabuf,
    string dai_name) {
  // gkv and ckv analysis
  int i;
  uint32_t rc;
  uint32_t gkv_num;
  int prop_index;

  if (metadatabuf == nullptr ||
      dai_name.empty() ||
      fe_dai_csd2_map_.find(dai_name) == fe_dai_csd2_map_.end()) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "AnalyzeStrdevMetadata invalid args for dai_name %s", dai_name.c_str());
    return;
  }

  rc = AnalyzeGkvAndCkvData(metadatabuf, dai_name, gkv_num, prop_index, STREAM_DEVICE_METADATA);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "AnalyzeGkvAndCkvData for strdev metadata fail");
    return;
  }

  // prop data analysis
  rc = AnalyzePropData(metadatabuf,
      &fe_dai_csd2_map_.at(dai_name)->stream_dev_prop_id, prop_index);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "propdata analysis for strdev metadata fail");
    return;
  }

  // get pp_mixer_node_id from strdev_metadata
  // pp_mixer_nodes defined in csd2_platform_info.h,
  // loop through stream device kv pairs then compare each within pp_mixer_nodes
  // to find the correct node
  for(const auto &it : fe_dai_csd2_map_.at(dai_name)->gkv_strdev) {
    for (i = 0; i < ARRAY_SIZE(pp_mixer_nodes); i++) {
      if ((pp_mixer_nodes[i].sgkv.kvp[0].key == it.first) &&
          (pp_mixer_nodes[i].sgkv.kvp[0].value == it.second)) {
          fe_dai_csd2_map_.at(dai_name)->pp_mixer_node_id = pp_mixer_nodes[i].id;
          return;
      }
    }
  }

  // can't find corresponding pp_mixer_node_id in pp_mixer_nodes
  AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "Couldn't find the correct pp_mixer_node_id for the given KVs");

  return;
}

// Analyze device metadata to get gkv/propdata, and fill 'CSD2Device'for
// csd2_devices_ with these info. The data format of dev_metadata
// ('metadatabuf') is: gkv_num + key[i] + value[i] + prop_id + num_value + values[num_value].
// example:0x01 0x00 0x00 0x00 0x00 0x00 0x00 0xa2 0x01 0x00 0x00 0xa2
//         0x00 0x00 0x00 0x00 0x10 0x00 0x00 0x08 0x02 0x00 0x00 0x00
//         0x02 0x00 0x00 0x00 0x05 0x00 0x00 0x00
// For the example gkv_num = 1, gkv_pairs[0] = 0xa2000000, gkv_pairs[1] = 0xa2000001,
// prop_id = 0x08000010, num_value = 2, values[0] = 2, values[1] = 5.
void CSD2AudioPlugin::AnalyzeDeviceMetadata(uint8_t *metadatabuf,
                                                char *be_dai_name) {
  // gkv and ckv analysis
  int i;
  uint32_t rc;
  uint32_t gkv_num;
  int prop_index;
  CSD2Device dev_node;

  if (metadatabuf == nullptr || be_dai_name == nullptr) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "AnalyzeDeviceMetadata invalid args for be_dai_name %s", be_dai_name);
    return;
  }

  if (csd2_devices_.find(be_dai_name) == csd2_devices_.end()) {
    pair<string, CSD2Device> element(be_dai_name, dev_node);
    csd2_devices_.insert(element);
  }

  // provide a be_dai_name, need fill gkv for CSD2Device
  rc = AnalyzeGkvAndCkvData(metadatabuf, be_dai_name, gkv_num, prop_index, DEVICE_METADATA);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "AnalyzeGkvAndCkvData for device metadata fail");
    return;
  }

  // fill PropData
  rc = AnalyzePropData(metadatabuf, &csd2_devices_.at(be_dai_name).dev_prop_id, prop_index);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "propdata analysis for device metadata fail");
    return;
  }

  // get device_node_id
  // device_nodes defined in csd2_platform_info.h,
  // loop through device_nodes to find the correct node by comparing kv data.
  for (i = 0; i < ARRAY_SIZE(device_nodes); i++) {
    if (device_nodes[i].sgkv.num_kvps == 1 &&
        (device_nodes[i].sgkv.kvp[0].key == csd2_devices_.at(be_dai_name).gkv[0].first) &&
        (device_nodes[i].sgkv.kvp[0].value == csd2_devices_.at(be_dai_name).gkv[0].second)) {
      csd2_devices_.at(be_dai_name).device_node_id = device_nodes[i].id;
      break;
    }
  }

  // can't find corresponding device_node_id in device_nodes
  if (i == ARRAY_SIZE(device_nodes)) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "Couldn't find the correct device_node_id for the given KVs");
  }

  return;
}

// We get ckv data from 'metadatabuf'
// example 1: 0x10 0x00 0x00 0x00 0x00 0x00 0x00 0xa4 0x0d 0x00 0x00 0x00
// example 2: 0x10 0x00 0x00 0x00 0x00 0x00 0x00 0xa4 0x0f 0x00 0x00 0x00
// For example 1 num_values = 0x01, ckv key = 0x00 0x00 0x00 0xa4, ckv value = 0x0d 0x00 0x00 0x00
// For example 2 num_values = 0x01, ckv key = 0x00 0x00 0x00 0xa4, ckv value = 0x0f 0x00 0x00 0x00
uint32_t CSD2AudioPlugin::AnalyzeMetadataCkv(uint8_t *metadatabuf,
        string dai_name, uint32_t *ckv_num) {
  int ckv_index = 4;

  if (metadatabuf == nullptr ||
      ckv_num == nullptr ||
      fe_dai_csd2_map_.find(dai_name) == fe_dai_csd2_map_.end()) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "AnalyzeMetadataCkv invalid args for dai_name: %s", dai_name.c_str());
    return IAUD_EINVAL;
  }

  *ckv_num = ((uint32_t)metadatabuf[3] << 24) |
             ((uint32_t)metadatabuf[2] << 16) |
             ((uint32_t)metadatabuf[1] << 8) |
             (uint32_t)metadatabuf[0];

  if (*ckv_num <= 0) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "Invalid ckv number");
    return -IAUD_ECKVERROR;
  }

  uint32_t ckv_pairs[*ckv_num * 2];
  (void)AnalyzeKvData(metadatabuf, &ckv_pairs[0],
          *ckv_num, ckv_index);
    for (uint32_t i = 0; i < *ckv_num; i++) {
      if (!fe_dai_csd2_map_.at(dai_name)->ckv_str.empty())
        fe_dai_csd2_map_.at(dai_name)->ckv_str.clear();
      fe_dai_csd2_map_.at(dai_name)->ckv_str.push_back(
        make_pair(ckv_pairs[i * 2], ckv_pairs[i * 2 + 1]));
    }

  return IAUD_EOK;
}

uint32_t CSD2AudioPlugin::AssignChannelMapping(uint32_t chs,
                                               uint8_t channel_mapping[]) {
  if (chs == 1) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_C;
  } else if (chs == 2) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
  } else if (chs == 3) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_C;
  } else if (chs == 4) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_LB;
    channel_mapping[3] = CSD2_CHMAP_CHANNEL_RB;
  } else if (chs == 5) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_C;
    channel_mapping[3] = CSD2_CHMAP_CHANNEL_LB;
    channel_mapping[4] = CSD2_CHMAP_CHANNEL_RB;
  } else if (chs == 6) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_C;
    channel_mapping[3] = CSD2_CHMAP_CHANNEL_LB;
    channel_mapping[4] = CSD2_CHMAP_CHANNEL_RB;
    channel_mapping[5] = CSD2_CHMAP_CHANNEL_LFE;
  } else if (chs == 7) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_C;
    channel_mapping[3] = CSD2_CHMAP_CHANNEL_LS;
    channel_mapping[4] = CSD2_CHMAP_CHANNEL_RS;
    channel_mapping[5] = CSD2_CHMAP_CHANNEL_LB;
    channel_mapping[6] = CSD2_CHMAP_CHANNEL_RB;
  } else if (chs == 8) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_LFE;
    channel_mapping[3] = CSD2_CHMAP_CHANNEL_C;
    channel_mapping[4] = CSD2_CHMAP_CHANNEL_LS;
    channel_mapping[5] = CSD2_CHMAP_CHANNEL_RS;
    channel_mapping[6] = CSD2_CHMAP_CHANNEL_LB;
    channel_mapping[7] = CSD2_CHMAP_CHANNEL_RB;
  } else if (chs == 12) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_RS;
    channel_mapping[3] = CSD2_CHMAP_CHANNEL_LFE;
    channel_mapping[4] = CSD2_CHMAP_CHANNEL_RB;
    channel_mapping[5] = CSD2_CHMAP_CHANNEL_TS;
    channel_mapping[6] = CSD2_CHMAP_CHANNEL_FLC;
    channel_mapping[7] = CSD2_CHMAP_CHANNEL_FRC;
    channel_mapping[8] = CSD2_CHMAP_CHANNEL_LFE2;
    channel_mapping[9] = CSD2_CHMAP_CHANNEL_SL;
    channel_mapping[10] = CSD2_CHMAP_CHANNEL_TFR;
    channel_mapping[11] = CSD2_CHMAP_CHANNEL_TC;
  } else if (chs == 16) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_C;
    channel_mapping[3] = CSD2_CHMAP_CHANNEL_LS;
    channel_mapping[4] = CSD2_CHMAP_CHANNEL_RS;
    channel_mapping[5] = CSD2_CHMAP_CHANNEL_LFE;
    channel_mapping[6] = CSD2_CHMAP_CHANNEL_LB;
    channel_mapping[7] = CSD2_CHMAP_CHANNEL_RB;
    channel_mapping[8] = CSD2_CHMAP_CHANNEL_TS;
    channel_mapping[9] = CSD2_CHMAP_CHANNEL_TFC;
    channel_mapping[10] = CSD2_CHMAP_CHANNEL_MS;
    channel_mapping[11] = CSD2_CHMAP_CHANNEL_FLC;
    channel_mapping[12] = CSD2_CHMAP_CHANNEL_FRC;
    channel_mapping[13] = CSD2_CHMAP_CHANNEL_RLC;
    channel_mapping[14] = CSD2_CHMAP_CHANNEL_RRC;
    channel_mapping[15] = CSD2_CHMAP_CHANNEL_LFE2;
  } else if (chs == 32) {
    channel_mapping[0] = CSD2_CHMAP_CHANNEL_FL;
    channel_mapping[1] = CSD2_CHMAP_CHANNEL_FR;
    channel_mapping[2] = CSD2_CHMAP_CHANNEL_C;
    channel_mapping[3] = CSD2_CHMAP_CHANNEL_LS;
    channel_mapping[4] = CSD2_CHMAP_CHANNEL_RS;
    channel_mapping[5] = CSD2_CHMAP_CHANNEL_LFE;
    channel_mapping[6] = CSD2_CHMAP_CHANNEL_LB;
    channel_mapping[7] = CSD2_CHMAP_CHANNEL_RB;
    channel_mapping[8] = CSD2_CHMAP_CHANNEL_TS;
    channel_mapping[9] = CSD2_CHMAP_CHANNEL_TFC;
    channel_mapping[10] = CSD2_CHMAP_CHANNEL_MS;
    channel_mapping[11] = CSD2_CHMAP_CHANNEL_FLC;
    channel_mapping[12] = CSD2_CHMAP_CHANNEL_FRC;
    channel_mapping[13] = CSD2_CHMAP_CHANNEL_RLC;
    channel_mapping[14] = CSD2_CHMAP_CHANNEL_RRC;
    channel_mapping[15] = CSD2_CHMAP_CHANNEL_LFE2;
    channel_mapping[16] = CSD2_CHMAP_CHANNEL_SL;
    channel_mapping[17] = CSD2_CHMAP_CHANNEL_SR;
    channel_mapping[18] = CSD2_CHMAP_CHANNEL_TFL;
    channel_mapping[19] = CSD2_CHMAP_CHANNEL_TFR;
    channel_mapping[20] = CSD2_CHMAP_CHANNEL_TC;
    channel_mapping[21] = CSD2_CHMAP_CHANNEL_TBL;
    channel_mapping[22] = CSD2_CHMAP_CHANNEL_TBR;
    channel_mapping[23] = CSD2_CHMAP_CHANNEL_TSL;
    channel_mapping[24] = CSD2_CHMAP_CHANNEL_TSR;
    channel_mapping[25] = CSD2_CHMAP_CHANNEL_TBC;
    channel_mapping[26] = CSD2_CHMAP_CHANNEL_BFC;
    channel_mapping[27] = CSD2_CHMAP_CHANNEL_BFL;
    channel_mapping[28] = CSD2_CHMAP_CHANNEL_BFR;
    channel_mapping[29] = CSD2_CHMAP_CHANNEL_LW;
    channel_mapping[30] = CSD2_CHMAP_CHANNEL_RW;
    channel_mapping[31] = CSD2_CHMAP_CHANNEL_LSD;
  } else {
    return IAUD_EFAILED;
  }

  return IAUD_EOK;
}

uint32_t CSD2AudioPlugin::SetStreamFormat(uint32_t session_handle,
    uint32_t sw_in_rate, uint32_t sw_in_bits, uint32_t sw_in_chs,
    uint32_t sw_out_rate, uint32_t sw_out_bits, uint32_t sw_out_chs) {
  struct csd2_stream_fmt_config stream_fmt_config;
  memset(&stream_fmt_config, 0, sizeof(stream_fmt_config));
  stream_fmt_config.u.rx_fmt.fmt_cfg.aud_fmt_id = CSD2_MEDIA_FORMAT_PCM;
  stream_fmt_config.u.rx_fmt.fmt_cfg.data_format = CSD2_DATA_FORMAT_FIXED_POINT;
  stream_fmt_config.u.rx_fmt.fmt_cfg.fmt_blk_size = sizeof(struct csd2_media_fmt_input_pcm);
  stream_fmt_config.u.rx_fmt.fmt_cfg.u.pcm.sample_rate = sw_in_rate;
  stream_fmt_config.u.rx_fmt.fmt_cfg.u.pcm.bit_width = sw_in_bits;
  stream_fmt_config.u.rx_fmt.fmt_cfg.u.pcm.num_channels = sw_in_chs;
  uint32_t rc = IAUD_EOK;

  rc = AssignChannelMapping(sw_in_chs,
      &stream_fmt_config.u.rx_fmt.fmt_cfg.u.pcm.channel_mapping[0]);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "assign channel mapping failure");
    return rc;
  }

  stream_fmt_config.u.rx_fmt.pcm_output_cfg.sample_rate = sw_out_rate;  // ignored by driver
  stream_fmt_config.u.rx_fmt.pcm_output_cfg.bit_width = sw_out_bits;
  stream_fmt_config.u.rx_fmt.pcm_output_cfg.num_channels = sw_out_chs;
  rc = AssignChannelMapping(sw_out_chs,
      &stream_fmt_config.u.rx_fmt.pcm_output_cfg.channel_mapping[0]);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "assign channel mapping failure");
    return rc;
  }

  rc = csd2_ioctl(session_handle, CSD2_CMD_SET_STREAM_FORMAT,
                  &stream_fmt_config, sizeof(stream_fmt_config));
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "csd2_ioctl set stream in out media format failure");
    return rc;
  }

  return IAUD_EOK;
}

uint32_t CSD2AudioPlugin::HwEpConfig(uint32_t session_handle, uint32_t device,
    uint32_t hw_rate, uint32_t hw_bits, uint32_t hw_chs) {
  uint32_t rc = IAUD_EOK;
  struct csd2_device_config* device_config;
  device_config = static_cast<struct csd2_device_config*>(calloc(1,
      sizeof(struct csd2_device_config) + sizeof(struct csd2_device_config_entry)));
  if (device_config == nullptr) {
    rc = IAUD_ENORESOURCE;
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "alloc memory for csd2_device_config fail");
    return rc;
  }

  device_config->num_of_devices = 1;
  device_config->entries[0].bits = hw_bits;
  device_config->entries[0].channels = hw_chs;
  device_config->entries[0].sample_rate = hw_rate;
  device_config->entries[0].device_id = device;

  rc = csd2_ioctl(session_handle, CSD2_CMD_DEVICE_CONFIG,
      device_config, sizeof(struct csd2_device_config) +
      device_config->num_of_devices * sizeof(struct csd2_device_config_entry));
  if (rc != IAUD_EOK)
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2_ioctl set device format failure");

  free(device_config);

  return rc;
}

// Used for loopback with two devices. Index 0 = RX, index 1 = TX
uint32_t CSD2AudioPlugin::HwEpConfig(uint32_t session_handle, uint32_t devices[2],
  uint32_t rates[2], uint32_t bits[2], uint32_t chs[2]) {
  uint32_t rc = IAUD_EOK;
  struct csd2_device_config* device_config;
  device_config = static_cast<struct csd2_device_config*>(calloc(1,
      sizeof(struct csd2_device_config) + 2 * sizeof(struct csd2_device_config_entry)));
  if (device_config == nullptr) {
    rc = IAUD_ENORESOURCE;
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "alloc memory for csd2_device_config fail");
    return rc;
  }

  device_config->num_of_devices = 2;
  // RX
  device_config->entries[0] = {
    .device_id = devices[0],
    .sample_rate = rates[0],
    .bits = bits[0],
    .channels = chs[0],
  };
  // TX
  device_config->entries[1] = {
    .device_id = devices[1],
    .sample_rate = rates[1],
    .bits = bits[1],
    .channels = chs[1],
  };

  rc = csd2_ioctl(session_handle, CSD2_CMD_DEVICE_CONFIG,
      device_config, sizeof(struct csd2_device_config) +
      device_config->num_of_devices * sizeof(struct csd2_device_config_entry));
  if (rc != IAUD_EOK)
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2_ioctl set device format failure");

  free(device_config);

  return rc;
}

static void PlaybackCaptureCallback(struct csd2_event_cb_params* event_params,
    void* client_data) {
  CSD2Stream *stream = (CSD2Stream*)client_data;

  if (event_params == nullptr ||
      client_data == nullptr ||
      stream == nullptr) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "PlaybackCaptureCallback invalid arg");
    return;
  }

  CSD2AudioPlugin *csd2_plug = (CSD2AudioPlugin*)stream->csd2ap_instance;

  AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV,
    "received from module %x event_id %d payload size %d",
    event_params->source_module_id, event_params->event_id,
    event_params->event_payload_size);

  switch (event_params->event_id) {
    case CSD2_EVENT_ID_WRITE_DONE_V2:
    case CSD2_EVENT_ID_READ_DONE_V2: {
      struct csd2_event_read_write_done_payload_v2* payload =
          (struct csd2_event_read_write_done_payload_v2 *)event_params->event_payload;
      if (payload) {  // it will be nullptr for non blocking mode, and valid only for external memory mode
        AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV,
            "addr %p count %lu timestamp %lx", payload->addr,
            payload->count, payload->timestamp);
        stream->timestamp = payload->timestamp;
        csd2_plug->adm_pcm_msg_cb_func(stream->fe_dai_name, payload->offset);
      }
      break;
    }
    case CSD2_EVENT_ID_EOS: {
      AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "receive str eos event");
      {
        std::lock_guard<std::mutex> rw_lock(stream->stream_mutex);
        stream->csd2_event |= CSD2_EOS_EVENT;
      }
      stream->stream_cond.notify_all();
      break;
    }
    default: {
      break;
    }
  }
}

uint32_t CSD2AudioPlugin::AssignNodeIndices(string dai_name, string be_dai_name, uint32_t &nodes_num,
                      int &stream_node_index, int &pp_node_index, int &device_node_index) {

  if (fe_dai_csd2_map_.find(dai_name) == fe_dai_csd2_map_.end() ||
      (csd2_devices_.find(be_dai_name) == csd2_devices_.end())) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "FillGraphInfo invalid args dai_name: %s, be_dai_name: %s", dai_name.c_str(), be_dai_name.c_str());
    return IAUD_EINVAL;
  }

  if (fe_dai_csd2_map_.at(dai_name)->stream_node_id > CSD2_STREAM_TYPE_BEGIN &&
      fe_dai_csd2_map_.at(dai_name)->stream_node_id < CSD2_STREAM_TYPE_END) {
    stream_node_index = nodes_num;
    nodes_num++;
  }

  if (fe_dai_csd2_map_.at(dai_name)->pp_mixer_node_id > CSD2_DEVICE_PP_TYPE_BEGIN &&
      fe_dai_csd2_map_.at(dai_name)->pp_mixer_node_id < CSD2_DEVICE_PP_TYPE_END) {
    pp_node_index = nodes_num;
    nodes_num++;
  }

  if (csd2_devices_.at(be_dai_name).device_node_id > CSD2_DEVICE_TYPE_BEGIN &&
      csd2_devices_.at(be_dai_name).device_node_id < CSD2_DEVICE_TYPE_END) {
    device_node_index = nodes_num;
    nodes_num++;
  }

  if (nodes_num == 0) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "graphinfo is nullptr");
    return IAUD_EFAILED;
  }

  return IAUD_EOK;
}

// The elements of array 'node_id' in graph_info are sorted in a specific order:
// stream/devicepp/device. The 'number_of_nodes' in graph_info is the array size
// of 'node_id', the value may be 1/2/3.
uint32_t CSD2AudioPlugin::FillGraphInfo(string dai_name, string be_dai_name, struct csd2_graph_info *&graph_info) {
  uint32_t nodes_num = 0;
  int stream_node_index = -1;
  int pp_node_index = -1;
  int device_node_index = -1;
  int rc = 0;

  rc = AssignNodeIndices(dai_name, be_dai_name, nodes_num, stream_node_index, pp_node_index, device_node_index);
  if (rc) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to determine number of nodes in graph");
    return IAUD_EFAILED;
  }

  graph_info = static_cast<struct csd2_graph_info*>(realloc(graph_info, sizeof(uint32_t) + (sizeof(csd2_node_id_t) * nodes_num)));
  if (graph_info == nullptr) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "realloc memory for csd2_graph_info failed");
    return IAUD_ENORESOURCE;
  }

  graph_info->number_of_nodes = nodes_num;

  if (stream_node_index != -1) {
    graph_info->node_id[stream_node_index] = fe_dai_csd2_map_.at(dai_name)->stream_node_id;
  }
  if (pp_node_index != -1) {
    graph_info->node_id[pp_node_index] = fe_dai_csd2_map_.at(dai_name)->pp_mixer_node_id;
  }
  if (device_node_index != -1) {
    graph_info->node_id[device_node_index] = csd2_devices_.at(be_dai_name).device_node_id;
  }

  return IAUD_EOK;
}

// For cases such as loopback, there may be more than 3 nodes in the graph
// This function allows for an existing graph to be extended so that additional
// nodes can be added to the graph
uint32_t CSD2AudioPlugin::AddGraphInfo(string dai_name, string be_dai_name, struct csd2_graph_info *&graph_info) {
  uint32_t nodes_num = 0;
  int stream_node_index = -1;
  int pp_node_index = -1;
  int device_node_index = -1;
  int rc = 0;

  if (graph_info == nullptr) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "original graph is empty, nothing to append to");
    return IAUD_EINVAL;
  }

  nodes_num = graph_info->number_of_nodes;
  rc = AssignNodeIndices(dai_name, be_dai_name, nodes_num, stream_node_index, pp_node_index, device_node_index);
  if (rc) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to determine number of nodes in graph");
    return IAUD_EFAILED;
  }

  // Reallocate the graph global to accomodate all existing + new nodes
  graph_info = static_cast<struct csd2_graph_info*>(realloc(
      graph_info, sizeof(uint32_t) + (sizeof(csd2_node_id_t) * nodes_num)));
  if (!graph_info) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "realloc memory for csd2_graph_info failed");
    return IAUD_ENORESOURCE;
  }

  // Fill the reallocated graph global with the new nodes
  graph_info->number_of_nodes = nodes_num;

  if (stream_node_index != -1) {
    graph_info->node_id[stream_node_index] = fe_dai_csd2_map_.at(dai_name)->stream_node_id;
  }
  if (pp_node_index != -1) {
    graph_info->node_id[pp_node_index] = fe_dai_csd2_map_.at(dai_name)->pp_mixer_node_id;
  }
  if (device_node_index != -1) {
    graph_info->node_id[device_node_index] = csd2_devices_.at(be_dai_name).device_node_id;
  }

  return IAUD_EOK;
}

int CSD2AudioPlugin::GetPcmBitsPerSample(uint32_t fmt_id) {
  int bits_per_sample = 16;

  switch(fmt_id) {
    case kAgmFormatPcmS8:      // 8-bit signed
      bits_per_sample = 8;
      break;
    case kAgmFormatPcmS24LE:   // 24-bits in 4-bytes, 8_24 form
      bits_per_sample = 32;
      break;
    case kAgmFormatPcmS243LE:  // 24-bits in 3-bytes
      bits_per_sample = 24;
      break;
    case kAgmFormatPcmS32LE:   // 32-bit signed
      bits_per_sample = 32;
      break;
    case kAgmFormatPcmS16LE:   // 16-bit signed
    default:
      bits_per_sample = 16;
      break;
  }
  return bits_per_sample;
}

uint32_t CSD2AudioPlugin::DcWrite(const struct CtlElemPayload &ctl_elem_payload) {
  char *dai_name, *pcontrol;
  char *saveptr = nullptr;
  struct csd2_graph_info *graph_info = nullptr;
  CSD2Device dev_node;

  AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "enter");
  // Extract the dai_name and control name
  dai_name = strtok_r((char *)ctl_elem_payload.name.c_str(), " ", &saveptr);
  pcontrol = strtok_r(nullptr, " ", &saveptr);
  if ((dai_name == nullptr) || (pcontrol == nullptr)) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "DcWrite dai_name or pcontrol is empty");
    return IAUD_EINVAL;
  }

  // create csd2 device node in case we are receiving mixer ctl with intf name first
  if ((strstr(dai_name, "TDM-")) && (csd2_devices_.find(dai_name) == csd2_devices_.end())) {
    pair<string, CSD2Device> element(dai_name, dev_node);
    csd2_devices_.insert(element);
  }

  // deal with enum cmd
  string enum_str;
  unsigned int enum_val;
  int32_t rc = IAUD_EOK;
  uint32_t session_handle = 0;
  struct csd2_open_config open_cfg = {0};

  // Handle all byte type controls: 'setCalibration'
  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES
      && strncmp(pcontrol, "setCalibration", strlen("setCalibration")) == 0) {
      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "received setCalibration mixer ctl");
      struct csd2_key_value *ckv = nullptr;
      uint32_t ckv_num = 0;

      if (fe_dai_csd2_map_.find(dai_name) == fe_dai_csd2_map_.end()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "setCalibration invalid args for dai_name: %s", dai_name);
        return IAUD_EINVAL;
      }

      rc = AnalyzeMetadataCkv((uint8_t *)ctl_elem_payload.value.bytes.data, dai_name, &ckv_num);
      if (rc != IAUD_EOK) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "AnalyzeMetadataCkv for stream metadata fail");
        return rc;
      }

      ckv = static_cast<struct csd2_key_value*>(calloc(ckv_num, sizeof(struct csd2_key_value)));
      if (ckv == nullptr) {
        rc = IAUD_ENORESOURCE;
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "alloc memory for ckv failed");
        return rc;
      }

      for (uint32_t i=0;i<ckv_num;++i) {
        ckv[i].key = fe_dai_csd2_map_.at(dai_name)->ckv_str[i].first,
        ckv[i].value = fe_dai_csd2_map_.at(dai_name)->ckv_str[i].second;
      }

      rc = SetCalData(dai_name, ckv_num, ckv);
      if (rc != IAUD_EOK) {
        // Setting CKV in CSD2 is for all subgraphs and it'll return
        // CSD2_EALREADY if a subgraph has been started.
        // Although the return value is CSD2_EALREADY, setting ckv
        // for all subgraphs is successful.
        if (rc == CSD2_EALREADY) {
          AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
            "A subgraph is in started state, ignore EALREADY");
          rc = IAUD_EOK;
        } else {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "set calibration fail");
          rc = IAUD_EFAILED;
          free(ckv);
          return rc;
        }
      }

      free(ckv);
  }
  // Handle all enum type controls: 'control', 'connect', 'loopback'
  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
    enum_val = ctl_elem_payload.value.enumerated.item[0];
    enum_str = all_mixer_ctl_info_->at(ctl_elem_payload.control_id).
        enum_mapping.at(enum_val);
    if (strncmp(pcontrol, "control", strlen("control")) == 0) {
      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
          "Received control ctl for dai_name %s", dai_name);

      if (fe_dai_csd2_map_.find(dai_name) != fe_dai_csd2_map_.end())
        fe_dai_csd2_map_.at(dai_name)->ctl_val_ = enum_val;
      if (!strncmp(enum_str.c_str(), "ZERO", strlen("ZERO")) == 0) {
        // fill be_dai_names
        if (fe_dai_csd2_map_.find(dai_name) != fe_dai_csd2_map_.end()) {
          fe_dai_csd2_map_.at(dai_name)->be_dai_names.push_back(enum_str);
        }
        // fill fe_dai_names
        if (csd2_devices_.find(enum_str) != csd2_devices_.end()) {
          csd2_devices_.at(enum_str).fe_dai_names.push_back(dai_name);
        }
    }
  }

    if (strncmp(pcontrol, "connect", strlen("connect")) == 0) {
      // Skip connect calls for loopback type PCM ids, which will be handled
      // in the 'loopback' mixer ctl logic instead
      if (fe_dai_csd2_map_.at(dai_name)->sess_mode == kSessModeNoHost) {
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
          "Ignore connect call for hostless PCM ids - wait until loopback mixer ctl");
        return IAUD_EOK;
      }
      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
        "Received connect ctl for dai_name %s", dai_name);

      if (fe_dai_csd2_map_.find(dai_name) != fe_dai_csd2_map_.end() &&
          fe_dai_csd2_map_.at(dai_name)->csd2_handle &&
          fe_dai_csd2_map_.at(dai_name)->stream_state == PCM_STATE_OPEN)
      {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
              "found an existing csd2_handle: %u , dai_name: %s",
              fe_dai_csd2_map_.at(dai_name)->csd2_handle, dai_name);

        rc = csd2_close(fe_dai_csd2_map_.at(dai_name)->csd2_handle);
        if (rc != IAUD_EOK) {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "close session failure");
          rc = IAUD_EFAILED;
          return rc;
        }
      }
      // fill graph with stream_node_id/pp_mixer_node_id/device_node_id
      if (graph_info == nullptr) {
        graph_info = static_cast<struct csd2_graph_info*>(calloc(sizeof(uint32_t) + (sizeof(csd2_node_id_t)), 1));
      }
      rc = FillGraphInfo(dai_name, enum_str, graph_info);
      if (rc != IAUD_EOK) {
        if (graph_info != nullptr) {
          free(graph_info);
          graph_info = nullptr;
        }
        return rc;
      }

      // do csd2_open, get csd2_handle
      switch (fe_dai_csd2_map_.at(dai_name)->direction) {
        case SNDRV_PCM_STREAM_PLAYBACK: {
          open_cfg.op_code = CSD2_OPEN_OP_W;
          break;
        }
        case SNDRV_PCM_STREAM_CAPTURE: {
          open_cfg.op_code = CSD2_OPEN_OP_R;
          break;
        }
        default: {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "Unknown FE DAI direction");
          break;
        }
      }

      open_cfg.graph_info = graph_info;
      AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
        "Calling csd2 open for session with dai_name %s", dai_name);
      rc = csd2_open(CSD2_MODULE_TYPE_AUDIO_SESSION, &open_cfg,
          sizeof(open_cfg), &session_handle);
      if (rc != IAUD_EOK) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2 open failure");
        goto graph_cleanup;
      }
      else {
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "csd2 open successful, caching handle and state");
        fe_dai_csd2_map_.at(dai_name)->csd2_handle = session_handle;
        fe_dai_csd2_map_.at(dai_name)->stream_state = PCM_STATE_OPEN;

        // collect gkv for the opened session
        vector<pair<int,int>> gkv_vec(fe_dai_csd2_map_.at(dai_name)->gkv_str.begin(), fe_dai_csd2_map_.at(dai_name)->gkv_str.end());
        gkv_vec.insert(gkv_vec.end(), fe_dai_csd2_map_.at(dai_name)->gkv_strdev.begin(), fe_dai_csd2_map_.at(dai_name)->gkv_strdev.end());
        gkv_vec.insert(gkv_vec.end(), csd2_devices_.at(enum_str).gkv.begin(), csd2_devices_.at(enum_str).gkv.end());
        RemoveDuplicateGkv(gkv_vec);

        // set cached stream params, if any
        if (fe_dai_csd2_map_.at(dai_name)->stream_params != nullptr) {
          rc = SetParamToCSD2(pp_session_handle_, gkv_vec, (struct csd2_pp_control_spf_custom_param*)
                                       fe_dai_csd2_map_.at(dai_name)->stream_params);
          if (rc != IAUD_EOK) {
            goto graph_cleanup;
          }
        }

        free(fe_dai_csd2_map_.at(dai_name)->stream_params);
        fe_dai_csd2_map_.at(dai_name)->stream_params = nullptr;

        // set cached streamdevice params, if any
        if (fe_dai_csd2_map_.at(dai_name)->strdev_params != nullptr) {
          rc = SetParamToCSD2(pp_session_handle_, gkv_vec,
               (struct csd2_pp_control_spf_custom_param*) fe_dai_csd2_map_.at(dai_name)->strdev_params);
          if (rc != IAUD_EOK) {
            goto graph_cleanup;
          }
        }

        free(fe_dai_csd2_map_.at(dai_name)->strdev_params);
        fe_dai_csd2_map_.at(dai_name)->strdev_params = nullptr;

        // set cached devices params, if any
        if (csd2_devices_.at(enum_str).device_params != nullptr) {
          rc = SetParamToCSD2(pp_session_handle_, gkv_vec,
               (struct csd2_pp_control_spf_custom_param*)csd2_devices_.at(enum_str).device_params);
          if (rc != IAUD_EOK) {
            goto graph_cleanup;
          }
        }

        free(csd2_devices_.at(enum_str).device_params);
        csd2_devices_.at(enum_str).device_params = nullptr;

        // set cached streamdevice tagged params, if any
        if (fe_dai_csd2_map_.at(dai_name)->tag_params != nullptr) {
          rc = SetTaggedParamToCSD2(pp_session_handle_, gkv_vec,
              (struct tag_config*)fe_dai_csd2_map_.at(dai_name)->tag_params);
          if (rc != IAUD_EOK)
            goto graph_cleanup;
        }

        free(fe_dai_csd2_map_.at(dai_name)->tag_params);
        fe_dai_csd2_map_.at(dai_name)->tag_params = nullptr;

        goto done;
      }

graph_cleanup:
    // clean up the csd2_devices_ info and fe_dai_csd2_map_ info
    (void)StreamRelease(dai_name);
    rc = IAUD_EFAILED;

done:
  free(graph_info);
  graph_info = nullptr;
    }

    if (strncmp(pcontrol, "loopback", strlen("loopback")) == 0) {
      // Determine the RX PCM id, from the enum payload, that completes the loopback
      enum_val = ctl_elem_payload.value.enumerated.item[0];
      enum_str = all_mixer_ctl_info_->at(ctl_elem_payload.control_id).enum_mapping.at(enum_val);
      if (strncmp(enum_str.c_str(), "ZERO", strlen("ZERO")) == 0) {
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Continuing with teardown of loopback");
        return IAUD_EOK;
      }

      if (IsFeDaiNameInvalid(enum_str.c_str())) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "Invalid loopback ctl for %s to %s", dai_name, enum_str.c_str());
        return IAUD_EINVAL;
      }

      string loopback_id_str = enum_str;
      fe_dai_csd2_map_.at(dai_name)->loopback_id = loopback_id_str;
      fe_dai_csd2_map_.at(loopback_id_str)->loopback_id = dai_name;

      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
        "Received loopback ctl for %s to %s", dai_name, enum_str.c_str());

      // Merge the TX and RX graphs by filling the graph_info with the TX graph,
      // then adding the RX graph
      //TODO what about multiple BE names?
      string be_dai_name = fe_dai_csd2_map_.at(dai_name)->be_dai_names[0];
      if (graph_info == nullptr) {
        graph_info = static_cast<struct csd2_graph_info*>(calloc(sizeof(uint32_t) + (sizeof(csd2_node_id_t)), 1));
      }
      rc = FillGraphInfo(dai_name, be_dai_name, graph_info);
      if (rc != IAUD_EOK) {
        if (graph_info != nullptr) {
          free(graph_info);
          graph_info = nullptr;
        }
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to fill graph info for loopback");
        return rc;
      }
      //TODO what about multiple BE names?
      be_dai_name = fe_dai_csd2_map_.at(loopback_id_str)->be_dai_names[0];
      rc = AddGraphInfo(loopback_id_str, be_dai_name, graph_info);
      if (rc != IAUD_EOK) {
        if (graph_info != nullptr) {
          free(graph_info);
          graph_info = nullptr;
        }
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Failed to add graph info for loopback");
        return rc;
      }

      // Open a loopback graph
      open_cfg.op_code = CSD2_OPEN_OP_LOOPBACK;
      open_cfg.graph_info = graph_info;
      AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "Calling csd2 open for loopback session");
      rc = csd2_open(CSD2_MODULE_TYPE_AUDIO_SESSION, &open_cfg,
          sizeof(open_cfg), &session_handle);
      if (rc != IAUD_EOK) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2 open loopback failure");
        rc = IAUD_EFAILED;
      }
      else {
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "csd2 open loopback successful, caching handle and state");
        fe_dai_csd2_map_.at(dai_name)->csd2_handle = session_handle;
        fe_dai_csd2_map_.at(dai_name)->stream_state = PCM_STATE_OPEN;
        fe_dai_csd2_map_.at(loopback_id_str)->csd2_handle = session_handle;
        fe_dai_csd2_map_.at(loopback_id_str)->stream_state = PCM_STATE_OPEN;
      }

      free(graph_info);
      graph_info = nullptr;
    }
    else
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Received an unsupported ENUM type command, no-op-ing ctl %s %s", dai_name, pcontrol);
  }

  // Handle all tlv byte type controls: 'metadata', 'setParam', 'getParam'
  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES &&
      ctl_elem_payload.access_control == tlv_bytes) {
    if (strncmp(pcontrol, "metadata", strlen("metadata")) == 0) {
      if (!IsFeDaiNameInvalid(dai_name)) {
        if (!fe_dai_csd2_map_.at(dai_name)->ctl_val_) {  // stream metadata
          AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
            "Received stream metadata ctl for %s", dai_name);
          AnalyzeStreamMetadata(ctl_elem_payload.value.tlv.tlv, dai_name);
        } else {
            AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
                  "Received stream device metadata ctl for %s", dai_name);
            AnalyzeStrdevMetadata(ctl_elem_payload.value.tlv.tlv, dai_name); // streamdevice metadata
        }
      } else { //device metadata
          AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
            "Received device metadata ctl for %s", dai_name);
          AnalyzeDeviceMetadata(ctl_elem_payload.value.tlv.tlv, dai_name);
      }
    }
    else if (strstr(pcontrol, "setParam") != nullptr) {
      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Received %s ctl for %s", pcontrol,  dai_name);
      if (!IsFeDaiNameInvalid(dai_name)) {
        enum SetParamType param_type = PARAM_TYPE_UNKNOWN;
        uint32_t session_handle = fe_dai_csd2_map_.at(dai_name)->csd2_handle;
        int control_val = fe_dai_csd2_map_.at(dai_name)->ctl_val_;

        if (strncmp(pcontrol, "setParam", strlen(pcontrol)) == 0)
          param_type = PARAM_TYPE_SET_PARAM;
        else if (strncmp(pcontrol, "setParamTag", strlen(pcontrol)) == 0)
          param_type = PARAM_TYPE_SET_PARAM_TAG;
        else if (strncmp(pcontrol, "setParamTagACDB", strlen(pcontrol)) == 0)
          param_type = PARAM_TYPE_SET_PARAM_TAG_ACDB;

        if (!session_handle) {
          switch(param_type) {
            case PARAM_TYPE_SET_PARAM: {
              struct csd2_pp_control_spf_custom_param* param = (struct csd2_pp_control_spf_custom_param*)
                                                                ctl_elem_payload.value.tlv.tlv;
              uint32_t payload_size = param->size + sizeof(struct csd2_pp_control_spf_custom_param);
              if (!control_val) {
                if (fe_dai_csd2_map_.at(dai_name)->stream_params != nullptr)
                  free(fe_dai_csd2_map_.at(dai_name)->stream_params);
                fe_dai_csd2_map_.at(dai_name)->stream_params = static_cast<void*>(calloc(1, payload_size));
                if (!fe_dai_csd2_map_.at(dai_name)->stream_params) {
                  AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for stream params cache");
                  rc = IAUD_ENORESOURCE;
                  goto end;
                }

                rc = csd2ap_mem_cpy(fe_dai_csd2_map_.at(dai_name)->stream_params, payload_size,
                        param, payload_size);
                if (rc < 0 || rc < payload_size) {
                  AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                    "csd2ap_mem_cpy failed");
                  goto end;
                }

                rc = IAUD_EOK;
              }
              else {
                if (fe_dai_csd2_map_.at(dai_name)->strdev_params != nullptr)
                  free(fe_dai_csd2_map_.at(dai_name)->strdev_params);
                fe_dai_csd2_map_.at(dai_name)->strdev_params = static_cast<void*>(calloc(1, payload_size));
                if (!fe_dai_csd2_map_.at(dai_name)->strdev_params) {
                  AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for strdev params cache");
                  rc = IAUD_ENORESOURCE;
                  goto end;
                }

                rc = csd2ap_mem_cpy(fe_dai_csd2_map_.at(dai_name)->strdev_params, payload_size,
                        param, payload_size);
                if (rc < 0 || rc < payload_size) {
                  AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                    "csd2ap_mem_cpy failed");
                  goto end;
                }

                rc = IAUD_EOK;
              }

              goto end;
            }
            case PARAM_TYPE_SET_PARAM_TAG: {
              if (!control_val) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "setParamTag not supported for stream in closed state");
                rc = IAUD_EINVAL;
              }
              else {
                struct tag_config *payload = (struct tag_config*)ctl_elem_payload.value.tlv.tlv;
                uint32_t payload_size = sizeof(struct tag_config) + payload->num_tkvs * sizeof(struct csd2_key_value);
                if (fe_dai_csd2_map_.at(dai_name)->tag_params != nullptr)
                  free(fe_dai_csd2_map_.at(dai_name)->tag_params);
                fe_dai_csd2_map_.at(dai_name)->tag_params = static_cast<void*>(calloc(1, payload_size));
                if (!fe_dai_csd2_map_.at(dai_name)->tag_params) {
                  AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for tag_params cache");
                  rc = IAUD_ENORESOURCE;
                  goto end;
                }

                rc = csd2ap_mem_cpy(fe_dai_csd2_map_.at(dai_name)->tag_params, payload_size,
                        payload, payload_size);
                if (rc < 0 || rc < payload_size) {
                  AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                    "csd2ap_mem_cpy failed");
                  goto end;
                }

                rc = IAUD_EOK;
              }

              goto end;
            }

            default:
              break;
          }
        }

        if (!fe_dai_csd2_map_.at(dai_name)->gkv_str.size()) {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "no metadata set yet, failing");
          rc = IAUD_EFAILED;
          goto end;
        }

        vector<pair<int,int>> gkv_vec(fe_dai_csd2_map_.at(dai_name)->gkv_str.begin(), fe_dai_csd2_map_.at(dai_name)->gkv_str.end());
        // if control_val is ZERO, only use stream gkv when calling csd2 pp control
        // if control_val is set to a backend, use stream + stream_dev + device gkv when calling csd2 pp control
        if (control_val) {
          std::string be_dai = fe_dai_csd2_map_.at(dai_name)->supported_be_dai[control_val];
          gkv_vec.insert(gkv_vec.end(), fe_dai_csd2_map_.at(dai_name)->gkv_strdev.begin(), fe_dai_csd2_map_.at(dai_name)->gkv_strdev.end());
          gkv_vec.insert(gkv_vec.end(), csd2_devices_.at(be_dai).gkv.begin(), csd2_devices_.at(be_dai).gkv.end());
        }

        RemoveDuplicateGkv(gkv_vec);

        switch (param_type) {
          case PARAM_TYPE_SET_PARAM: {
            struct csd2_pp_control_spf_custom_param* payload = (struct csd2_pp_control_spf_custom_param*)
                                                                ctl_elem_payload.value.tlv.tlv;
            rc = SetParamToCSD2(pp_session_handle_, gkv_vec, payload);
            break;
          }
          case PARAM_TYPE_SET_PARAM_TAG: {
            struct tag_config *payload = (struct tag_config*)ctl_elem_payload.value.tlv.tlv;
            rc = SetTaggedParamToCSD2(pp_session_handle_, gkv_vec, payload);
            break;
          }
          case PARAM_TYPE_SET_PARAM_TAG_ACDB: {
            struct acdb_param *acdb_pp_params = (struct acdb_param*)ctl_elem_payload.value.tlv.tlv;
            rc = SetTaggedParamToACDB(pp_session_handle_, gkv_vec, acdb_pp_params);
            break;
          }
          default:
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Unsupported control %s for FE dai %s", pcontrol, dai_name);
            rc = IAUD_EFAILED;
            break;
        }
      }
      else if (strstr(dai_name, "TDM-") != nullptr ||
               strncmp(dai_name, "PCM_DUMMY-TX-0", strlen("PCM_DUMMY-TX-0")) == 0) {
        struct csd2_pp_control_spf_custom_param *payload = (struct csd2_pp_control_spf_custom_param*)
                                                           ctl_elem_payload.value.tlv.tlv;
        uint32_t payload_size = payload->size + sizeof(struct csd2_pp_control_spf_custom_param);

        if (csd2_devices_.at(dai_name).device_params) {
          free(csd2_devices_.at(dai_name).device_params);
        }

        csd2_devices_.at(dai_name).device_params = static_cast<char*>(calloc(1, payload_size));
        if (!csd2_devices_.at(dai_name).device_params) {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for device params");
          rc = IAUD_ENORESOURCE;
          goto end;
        }

        rc = csd2ap_mem_cpy(csd2_devices_.at(dai_name).device_params, payload_size,
                payload, payload_size);
        if (rc < 0 || rc < payload_size) {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "csd2ap_mem_cpy failed");
          goto end;
        }

        rc = IAUD_EOK;
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Cached payload of size %d for device setParam ctl", payload->size);
      }
      else
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Unsupported control %s for backend dai %s", pcontrol, dai_name);
    }
    else if ((strncmp(pcontrol, "getParam", strlen(pcontrol)) == 0) &&
             !IsFeDaiNameInvalid(dai_name)) {
      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Received getParam ctl for %s", dai_name);
      struct csd2_pp_control_spf_custom_param *payload = (struct csd2_pp_control_spf_custom_param*)
                                                           ctl_elem_payload.value.tlv.tlv;
      // this is to pass ar_mem_cpy boundary check
      // when dest_size < size at gsl_apm_get_config_inband
      payload->size = CSD2_AP_ALIGN_8BYTE(payload->size);
      uint32_t payload_size = payload->size + sizeof(struct csd2_pp_control_spf_custom_param);
      if (fe_dai_csd2_map_.at(dai_name)->get_param_payload != nullptr)
        free(fe_dai_csd2_map_.at(dai_name)->get_param_payload);

      fe_dai_csd2_map_.at(dai_name)->get_param_payload = static_cast<char*>(calloc(1, payload_size));
      if (!fe_dai_csd2_map_.at(dai_name)->get_param_payload) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for get_param_payload");
        rc = IAUD_ENORESOURCE;
        goto end;
      }

      rc = csd2ap_mem_cpy(fe_dai_csd2_map_.at(dai_name)->get_param_payload, payload_size,
              payload, payload_size);
      if (rc < 0 || rc < payload_size) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "csd2ap_mem_cpy failed");
        goto end;
      }

      rc = IAUD_EOK;
      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Cached payload of size %d for getParam ctl", payload->size);
    }
    else {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Received an unsupported TLV command, no-op-ing ctl %s for dai_name %s", pcontrol, dai_name);
    }
  }

  // Handle all integer type controls: 'rate'
  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER &&
      csd2_devices_.find(dai_name) != csd2_devices_.end()) {
    if (strncmp(pcontrol, "rate", strlen("rate")) == 0) {
      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Received device rate ch fmt ctl for %s", dai_name);
      if (!csd2_devices_.at(dai_name).rate)
        csd2_devices_.at(dai_name).rate = ctl_elem_payload.value.integer.value[0];
      if (!csd2_devices_.at(dai_name).ch)
        csd2_devices_.at(dai_name).ch = ctl_elem_payload.value.integer.value[1];
      if (!csd2_devices_.at(dai_name).format)
        csd2_devices_.at(dai_name).format = ctl_elem_payload.value.integer.value[2];
      if (!csd2_devices_.at(dai_name).data_fmt)
        csd2_devices_.at(dai_name).data_fmt = ctl_elem_payload.value.integer.value[3];
    }
    else
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Received an unsupported INT type command, no-op-ing ctl %s %s", dai_name, pcontrol);
  }

end:
  return rc;
}

int CSD2AudioPlugin::DcRead(struct CtlElemPayload &ctl_elem_payload) {
  char *dai_name, *pcontrol;
  char *saveptr = nullptr;
  uint32_t session_handle;
  char type[1];
  string enum_str;
  string loopback_id;
  string loopback_be_dai;
  unsigned int enum_val;
  int32_t rc = IAUD_EOK;

  dai_name = strtok_r((char *)ctl_elem_payload.name.c_str(), " ", &saveptr);
  pcontrol = strtok_r(nullptr, " ", &saveptr);

  if ((dai_name == nullptr) || (pcontrol == nullptr)) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "DcRead dai_name or pcontrol is empty");
    return IAUD_EINVAL;
  }



  // Handle all enum type controls: <to be filled>
  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Received an unsupported ENUM type command, no-op-ing ctl %s %s", dai_name, pcontrol);
  }

  // Handle all tlv byte type controls: 'getTaggedInfo', 'getParam', 'getBufInfo'
  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES &&
      ctl_elem_payload.access_control == tlv_bytes) {
      if ((strstr(pcontrol, "get") != nullptr) &&
           !IsFeDaiNameInvalid(dai_name)) {
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "got %s control for dai_name: %s", pcontrol, dai_name);
        if (!fe_dai_csd2_map_.at(dai_name)->gkv_str.size()) {
           AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "no metadata set yet, failing");
           rc = IAUD_EFAILED;
           goto end;
         }

         // if control_val is ZERO, only use stream gkv when calling csd2 pp control
        // if control_val is set to a backend, use stream + stream_dev + device gkv when calling csd2 pp control
         int control_val = fe_dai_csd2_map_.at(dai_name)->ctl_val_;
         vector<pair<int,int>> gkv_vec(fe_dai_csd2_map_.at(dai_name)->gkv_str.begin(), fe_dai_csd2_map_.at(dai_name)->gkv_str.end());
         //Loopback session has two streams, add the 2nd stream's gkv
         if (fe_dai_csd2_map_.at(dai_name)->sess_mode == kSessModeNoHost) {
             loopback_id = fe_dai_csd2_map_.at(dai_name)->loopback_id;
             gkv_vec.insert(gkv_vec.end(), fe_dai_csd2_map_.at(loopback_id)->gkv_str.begin(), fe_dai_csd2_map_.at(loopback_id)->gkv_str.end());
         }

         if (control_val) {
           std::string be_dai = fe_dai_csd2_map_.at(dai_name)->supported_be_dai[control_val];
           gkv_vec.insert(gkv_vec.end(), fe_dai_csd2_map_.at(dai_name)->gkv_strdev.begin(), fe_dai_csd2_map_.at(dai_name)->gkv_strdev.end());
           gkv_vec.insert(gkv_vec.end(), csd2_devices_.at(be_dai).gkv.begin(), csd2_devices_.at(be_dai).gkv.end());
           //Loopback session has two devices, add the 2nd device's gkv
           if (fe_dai_csd2_map_.at(dai_name)->sess_mode == kSessModeNoHost) {
               loopback_be_dai = fe_dai_csd2_map_.at(loopback_id)->be_dai_names[0];
               gkv_vec.insert(gkv_vec.end(), csd2_devices_.at(loopback_be_dai).gkv.begin(), csd2_devices_.at(loopback_be_dai).gkv.end());
           }
         }

         RemoveDuplicateGkv(gkv_vec);
         if (strncmp(pcontrol, "getTaggedInfo", strlen(pcontrol)) == 0) {
           struct csd2_get_tags_with_module_info module_info_param;
           module_info_param.gkv.num_kvs = gkv_vec.size();
           module_info_param.gkv.kv = static_cast<struct csd2_key_value*>(calloc(1, (gkv_vec.size() * sizeof(struct csd2_key_value))));
           if (!module_info_param.gkv.kv) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for gkv");
            rc = IAUD_ENORESOURCE;
            goto end;
          }

           FillMetadata(module_info_param.gkv.kv, gkv_vec);
           // call csd2_ioctl with nullptr payload and zero size first. When this call is done, csd2 will update the size
           // to reflect the module_info size to be returned.
           module_info_param.size = 0;
           module_info_param.module_info = nullptr;
           rc = csd2_ioctl(pp_session_handle_, CSD2_CMD_PP_GET_TAG_WITH_MODULE_INFO, &module_info_param, sizeof(module_info_param));
           if (rc != IAUD_EOK)
             AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2_ioctl pp get tag with module info failed");
           else {
             // call csd2_ioctl again with a valid payload pointer and size to receive the actual module_info payload
             module_info_param.module_info = (struct csd2_tag_module_info*)ctl_elem_payload.value.tlv.tlv;
             rc = csd2_ioctl(pp_session_handle_, CSD2_CMD_PP_GET_TAG_WITH_MODULE_INFO, &module_info_param, sizeof(module_info_param));
             if (rc != IAUD_EOK)
               AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2_ioctl pp get tag with module info failed");
           }

           free(module_info_param.gkv.kv);
         }
         else if (strncmp(pcontrol, "getParam", strlen(pcontrol)) == 0) {
          struct csd2_pp_param_custom_config pp_param;
          pp_param.gkv.num_kvs = gkv_vec.size();
          pp_param.gkv.kv = static_cast<struct csd2_key_value*>(calloc(1, gkv_vec.size() * sizeof(struct csd2_key_value)));
          if (!pp_param.gkv.kv) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory allocation failed for gkv");
            rc = IAUD_ENORESOURCE;
            goto end;
          }
          FillMetadata(pp_param.gkv.kv, gkv_vec);
          pp_param.custom_param = (struct csd2_pp_control_spf_custom_param*)fe_dai_csd2_map_.at(dai_name)->get_param_payload;
          rc = csd2_ioctl(pp_session_handle_, CSD2_CMD_PP_CONTROL_GET_CUSTOM_CONFIG, &pp_param, sizeof(struct csd2_pp_param_custom_config));
          if (rc != IAUD_EOK)
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2_ioctl pp control get custom config failed");
          else {
            uint32_t src_size = sizeof(struct csd2_pp_control_spf_custom_param) + pp_param.custom_param->size;
            rc = csd2ap_mem_cpy(ctl_elem_payload.value.tlv.tlv,
                    src_size,
                    pp_param.custom_param,
                    src_size);
            if (rc < 0 || rc < src_size)
              AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "csd2ap_mem_cpy failed");
            else
              rc = IAUD_EOK;
          }

          free(fe_dai_csd2_map_.at(dai_name)->get_param_payload);
          fe_dai_csd2_map_.at(dai_name)->get_param_payload = nullptr;

          free(pp_param.gkv.kv);
         }
        else if ((strncmp(pcontrol, "getBufInfo", strlen(pcontrol)) == 0) &&
                 (strstr(dai_name, "PCM") != nullptr)) {
            AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Received read for getBufInfo ctl for %s", dai_name);
            struct MmapBufInfo *buf_info = (struct MmapBufInfo*)ctl_elem_payload.value.tlv.tlv;

            buf_info->data_buf_fd = fe_dai_csd2_map_.at(dai_name)->push_pull_data_gvm_fd;
            buf_info->pos_buf_fd = fe_dai_csd2_map_.at(dai_name)->push_pull_pos_gvm_fd;
            buf_info->data_buf_size = fe_dai_csd2_map_.at(dai_name)->buf_pool.buf_size *
                                      fe_dai_csd2_map_.at(dai_name)->buf_pool.num_buffs;
            buf_info->pos_buf_size = sizeof(struct pcm_push_pull_position_buffer);
        }
        else
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Unsupported control %s for FE dai %s", pcontrol, dai_name);
      }
      else
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Invalid control %s for FE dai %s", pcontrol, dai_name);
  }

  // Handle all byte type controls: 'bufTimestamp'
  if ((ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_BYTES) &&
      (!IsFeDaiNameInvalid(dai_name))) {
    if (strncmp(pcontrol, "bufTimestamp", strlen("bufTimestamp")) == 0) {
      AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "received bufTimestamp mixer ctl");
      if (fe_dai_csd2_map_.at(dai_name)->timestamp > 0) {
        uint64_t timestamp = fe_dai_csd2_map_.at(dai_name)->timestamp;
        csd2ap_mem_cpy((uint64_t *)ctl_elem_payload.value.bytes.data, sizeof(uint64_t), &timestamp, sizeof(timestamp));
      }
    }
  }
  // Handle all integer type controls: <to be filled>
  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
    AUDIO_BASE_LOG(BASE_LOG_VERBOSE, MODULE_VIO_AUD_DEV, "Received an unsupported INT type command, no-op-ing ctl %s %s", dai_name, pcontrol);
  }

end:
  return rc;
}

uint32_t CSD2AudioPlugin::SwEpConfig(uint32_t session_handle,
    uint32_t buffer_size, uint32_t buffer_num, uint32_t sw_out_rate,
    uint32_t sw_out_bits, uint32_t sw_out_chs, string fe_dai_name, uint8_t is_mmap_noirq) {
  uint32_t rc = IAUD_EOK;
  struct csd2_stream_buffer_config buff_config;
  memset(&buff_config, 0, sizeof(buff_config));
  struct csd2_stream_fmt_config stream_fmt_config;
  memset(&stream_fmt_config, 0, sizeof(stream_fmt_config));
  struct csd2_pp_param_config pp_param_payload;
  memset(&pp_param_payload, 0, sizeof(csd2_pp_param_config));
  struct param_id_pcm_encoder_frame_size_t frame_size_config;
  uint32_t sample_per_msec = sw_out_rate / 1000;
  uint32_t frame_size = (buffer_size * 8) / (sw_out_chs * sw_out_bits);
  string dev_name;

  if (fe_dai_csd2_map_.find(fe_dai_name) == fe_dai_csd2_map_.end() ||
      !session_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "SetParamForCapture invalid args for fe_dai_name: %s", fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  // collect gkv for the opened session
  dev_name = fe_dai_csd2_map_.at(fe_dai_name)->be_dai_names[0];
  vector<pair<int,int>> gkv_vec(fe_dai_csd2_map_.at(fe_dai_name)->gkv_str.begin(), fe_dai_csd2_map_.at(fe_dai_name)->gkv_str.end());
  gkv_vec.insert(gkv_vec.end(), fe_dai_csd2_map_.at(fe_dai_name)->gkv_strdev.begin(), fe_dai_csd2_map_.at(fe_dai_name)->gkv_strdev.end());
  gkv_vec.insert(gkv_vec.end(), csd2_devices_.at(dev_name).gkv.begin(), csd2_devices_.at(dev_name).gkv.end());
  RemoveDuplicateGkv(gkv_vec);

  frame_size_config.frame_size_type = 1;
  frame_size_config.frame_size_in_samples = frame_size;

  pp_param_payload.is_persist = false;
  pp_param_payload.mode = CSD2_PP_CFG_MODE_TAGGED_PARAM;
  pp_param_payload.tag = STREAM_PCM_ENCODER;
  pp_param_payload.u.tagged_param.data = &frame_size_config;
  pp_param_payload.u.tagged_param.data_sz = sizeof(frame_size_config);
  pp_param_payload.u.tagged_param.param_id = PARAM_ID_PCM_ENCODER_FRAME_SIZE;
  pp_param_payload.gkv.num_kvs = gkv_vec.size();
  pp_param_payload.gkv.kv = static_cast<struct csd2_key_value*>(calloc(1, gkv_vec.size() * sizeof(struct csd2_key_value)));
  if (!pp_param_payload.gkv.kv) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "Memory alloc err for pp_param_payload");
    return IAUD_ENORESOURCE;
  }

  FillMetadata(pp_param_payload.gkv.kv, gkv_vec);

  rc = csd2_ioctl(pp_session_handle_, CSD2_CMD_PP_CONTROL_SET, &pp_param_payload, sizeof(pp_param_payload));

  free(pp_param_payload.gkv.kv);

  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_ioctl set encoder frame size config failed");
    return rc;
  }

  if (!is_mmap_noirq) {
    buff_config.buff_size = buffer_size;
    buff_config.num_buffs = buffer_num;
    buff_config.buffer_mode = CSD2_DATA_MODE_EXTERN_MEM;
    rc = csd2_ioctl(session_handle, CSD2_CMD_BUFFER_CONFIG, &buff_config,
                    sizeof(buff_config));
    if (rc != IAUD_EOK) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "csd2_ioctl set buffer config for capture failed");
      return rc;
    }
  }

  stream_fmt_config.u.tx_fmt.fmt_cfg.aud_fmt_id = CSD2_MEDIA_FORMAT_PCM;
  stream_fmt_config.u.tx_fmt.pcm_output_cfg.sample_rate = sw_out_rate;
  stream_fmt_config.u.tx_fmt.pcm_output_cfg.bit_width = sw_out_bits;
  stream_fmt_config.u.tx_fmt.pcm_output_cfg.num_channels = sw_out_chs;
  rc = AssignChannelMapping(sw_out_chs,
      &stream_fmt_config.u.tx_fmt.pcm_output_cfg.channel_mapping[0]);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "AssignChannelMapping failure");
    return rc;
  }

  rc = csd2_ioctl(session_handle, CSD2_CMD_SET_STREAM_FORMAT,
                  &stream_fmt_config, sizeof(stream_fmt_config));
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "csd2_ioctl set stream format for capture failed");
  }

  return rc;
}

// This function is for capture.
uint32_t CSD2AudioPlugin::SetParamForCapture(string fe_dai_name,
         struct PCMSetParams &set_param_payload) {
  uint32_t rc = IAUD_EOK;
  uint32_t session_handle;
  uint32_t ckv_num = 0;
  struct csd2_key_value *ckv = nullptr;
  uint32_t sw_in_rate = numeric_limits<uint32_t>::max();
  uint32_t sw_in_bits = numeric_limits<uint32_t>::max();
  uint32_t sw_in_chs;
  uint32_t device_id;
  uint32_t hw_chs;
  string dev_name;
  uint8_t is_mmap_noirq = set_param_payload.is_mmap_noirq;

  AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "SetParamForCapture: enter");

  if (fe_dai_csd2_map_.find(fe_dai_name) == fe_dai_csd2_map_.end() ||
      !fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "SetParamForCapture invalid args for fe_dai_name: %s", fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  // Skip SetParamForCapture for loopback cases as params have been handled in StreamSetParam already
  if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode == kSessModeNoHost) {
    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Skipping for loopback sessions");
    return IAUD_EOK;
  }

  session_handle = fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle;

  ckv_num = fe_dai_csd2_map_.at(fe_dai_name)->ckv_str.size();
  if (ckv_num) {
    ckv = static_cast<struct csd2_key_value*>(calloc(ckv_num, sizeof(struct csd2_key_value)));
    if (ckv == nullptr) {
        rc = IAUD_ENORESOURCE;
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "alloc memory for ckv failed");
        return rc;
    }

    for (uint32_t i=0;i<ckv_num;++i) {
      ckv[i].key = fe_dai_csd2_map_.at(fe_dai_name)->ckv_str[i].first,
      ckv[i].value = fe_dai_csd2_map_.at(fe_dai_name)->ckv_str[i].second;
    }

    // calling set cal before set stream format to avoid
    // acdb modules overwritten by the default values
    rc = SetCalData(fe_dai_name, ckv_num, ckv);
    if (rc != IAUD_EOK) {
      // SetCalData will also set ckv for device subgraph, will return with
      // CSD2_EALREADY if device has been started in concurrent recording
      if (rc == CSD2_EALREADY) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "Shared subgraph is in started state, can't set cal, ignore EALREADY");
        rc = IAUD_EOK;
      } else {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "set calibration fail");
        rc = IAUD_EFAILED;
        return rc;
      }
    }
  }

  // one fe_dai may map to many be_dais, so loop through all be_dais to do
  // SwEpConfig and HwEpConfig
  // TODO: commenting out for loop until multiple BE's are supported
  // for (auto iter : fe_dai_csd2_map_.at(fe_dai_name)->be_dai_names) {
  rc = MapHwParamsBwSr(sw_in_bits, sw_in_rate, set_param_payload);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "MapHwParamsBwSr for capture failure");
    return rc;
  }

  sw_in_chs = set_param_payload.channels;
  dev_name = fe_dai_csd2_map_.at(fe_dai_name)->be_dai_names[0];
  device_id = csd2_devices_.at(dev_name).device_node_id;

  uint32_t buffer_size_ch = set_param_payload.period_bytes;
  uint32_t num_buffs = set_param_payload.buffer_bytes / set_param_payload.period_bytes;
  rc = SwEpConfig(session_handle,
                  buffer_size_ch,  // buff_size
                  num_buffs,               // buff_num
                  sw_in_rate, sw_in_bits, sw_in_chs, fe_dai_name,
                  is_mmap_noirq);
  if (rc != IAUD_EOK) {
    return rc;
  }

  // For echo_reference usecase, skip DeviceChConfig and HwEpConfig
  // due to the lack of a real input device
  if (device_id != CSD2_DEVICE_ECHO_REF_TX) {
    DeviceChConfig(device_id, hw_chs);
    rc = HwEpConfig(session_handle, device_id, 48000, 16, hw_chs);

    if (rc != IAUD_EOK) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "config hw ep for capture failure");
      return rc;
    }
  }

  fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.num_buffs = num_buffs;
  fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.buf_size = buffer_size_ch;

  return rc;
}

// Determine the number of hw channels that a device should use
void CSD2AudioPlugin::DeviceChConfig(const uint32_t device_id, uint32_t &hw_chs) {

  switch (device_id) {
    case CSD2_DEVICE_RECORD_MIC:
    case CSD2_DEVICE_HANDSET_MIC:
    case CSD2_DEVICE_SEN_SOURCE:
    case CSD2_DEVICE_SEP_SOURCE:
    case CSD2_DEVICE_OCT_SOURCE:
    case CSD2_DEVICE_HS_IF0_SOURCE:
    case CSD2_DEVICE_HS_IF1_SOURCE:
    case CSD2_DEVICE_HS_IF2_SOURCE:
    case CSD2_DEVICE_HS_IF3_SOURCE:
    case CSD2_DEVICE_HS_IF4_SOURCE:
        hw_chs = 8;
        break;
    case CSD2_DEVICE_A2B_MIC:
        hw_chs = 32;
        break;
    case CSD2_DEVICE_A2B2_MIC:
        hw_chs = 16;
        break;
    default:
        hw_chs = 8;
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "device id not found, setting chs to %d by default", hw_chs);
  }
}

// This function is for playback and loopback.
uint32_t CSD2AudioPlugin::StreamSetParam(string fe_dai_name,
    struct PCMSetParams &set_param_payload) {
  uint32_t rc = IAUD_EOK;
  uint32_t session_handle;

  uint32_t sw_in_rate = numeric_limits<uint32_t>::max();
  uint32_t sw_in_bits = numeric_limits<uint32_t>::max();
  uint32_t sw_in_chs;
  uint32_t device_id;
  uint8_t is_mmap_noirq = set_param_payload.is_mmap_noirq;

  AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "StreamSetParam: enter");

  if (fe_dai_csd2_map_.find(fe_dai_name) == fe_dai_csd2_map_.end() ||
      !fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "StreamSetParam invalid args for fe_dai_name: %s", fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  session_handle = fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle;

  // CSD2_CMD_BUFFER_CONFIG
  struct csd2_stream_buffer_config buff_config;
  memset(&buff_config, 0, sizeof(buff_config));
  buff_config.buff_size = set_param_payload.period_bytes;
  buff_config.num_buffs = set_param_payload.buffer_bytes / set_param_payload.period_bytes;
  buff_config.buffer_mode = CSD2_DATA_MODE_EXTERN_MEM;
  buff_config.start_threshold = 0;
  buff_config.stop_threshold = 0;

  if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode != kSessModeNoHost && !is_mmap_noirq) {
    rc = csd2_ioctl(session_handle, CSD2_CMD_BUFFER_CONFIG,
                    &buff_config, sizeof(buff_config));
    if (rc != IAUD_EOK) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "csd2_ioctl set buffer config failed");
      return rc;
    }
  }

  rc = MapHwParamsBwSr(sw_in_bits, sw_in_rate, set_param_payload);
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "csd2_ioctl MapHwParamsBwSr for playback failed");
    return rc;
  }

  sw_in_chs = set_param_payload.channels;
  fe_dai_csd2_map_.at(fe_dai_name)->rate = sw_in_rate;
  fe_dai_csd2_map_.at(fe_dai_name)->format = sw_in_bits;
  fe_dai_csd2_map_.at(fe_dai_name)->ch = sw_in_chs;

  // one fe_dai may map to many be_dais, so loop through all be_dais to do
  // 'CSD2_CMD_SET_STREAM_FORMAT' and 'CSD2_CMD_DEVICE_CONFIG'
  for (auto iter : fe_dai_csd2_map_.at(fe_dai_name)->be_dai_names) {
    uint32_t dev_rate = csd2_devices_.at(iter).rate;
    uint32_t dev_bits = GetPcmBitsPerSample(csd2_devices_.at(iter).format);
    uint32_t dev_chs = csd2_devices_.at(iter).ch;
    uint32_t sw_out_rate = sw_in_rate;
    uint32_t sw_out_bits = sw_in_bits;
    uint32_t sw_out_chs = 6;
    uint32_t device_id = csd2_devices_.at(iter).device_node_id;

    // CSD2_CMD_SET_STREAM_FORMAT
    if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode != kSessModeNoHost) {
      if ((device_id == CSD2_DEVICE_A2B_SPKR) || (device_id == CSD2_DEVICE_A2B2_SPKR)
        || (device_id == CSD2_DEVICE_A2B_SPKR_62) || (device_id == CSD2_DEVICE_A2B_SPKR_63)
        || (device_id == CSD2_DEVICE_A2B_SPKR_64) || (device_id == CSD2_DEVICE_A2B_SPKR_128)
        || (sw_in_chs <= 6)) {
        sw_out_chs = sw_in_chs;
      }
      rc = SetStreamFormat(session_handle, sw_in_rate, sw_in_bits, sw_in_chs,
                          sw_out_rate, sw_out_bits, sw_out_chs);
      if (rc != IAUD_EOK) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "set str format err");
        return rc;
      }
    }

    // CSD2_CMD_DEVICE_CONFIG
    if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode == kSessModeNoHost) {
      uint32_t hw_chs;
      string loopback_id = fe_dai_csd2_map_.at(fe_dai_name)->loopback_id;
      string loopback_be_dai = fe_dai_csd2_map_.at(loopback_id)->be_dai_names[0];
      uint32_t devices[2] = {device_id,
                            csd2_devices_.at(loopback_be_dai).device_node_id};
      uint32_t rates[2] = {dev_rate,
                          csd2_devices_.at(loopback_be_dai).rate};
      uint32_t bits[2] = {dev_bits,
                          GetPcmBitsPerSample(csd2_devices_.at(loopback_be_dai).format)};
      DeviceChConfig(devices[1], hw_chs);
      uint32_t chs[2] = {dev_chs, hw_chs};

      rc = HwEpConfig(session_handle, devices, rates, bits, chs);
    }
    else
      rc = HwEpConfig(session_handle, device_id, dev_rate, dev_bits, dev_chs);
    if (rc != IAUD_EOK) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "hw ep failure");
      return rc;
    }
  }

  if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode != kSessModeNoHost) {
    fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.num_buffs = buff_config.num_buffs;
    fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.buf_size = buff_config.buff_size;
  }

  return rc;
}

// This function is for playback and capture.
uint32_t CSD2AudioPlugin::StreamPrepare(string fe_dai_name) {
  uint32_t rc = IAUD_EOK;
  struct csd2_reg_event reg_event = {0};
  uint32_t session_handle;

  if (fe_dai_csd2_map_.find(fe_dai_name) == fe_dai_csd2_map_.end() ||
      !fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "StreamPrepare invalid args for fe_dai_name: %s", fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  // Skip StreamPrepare for loopback case as there's no need for the callback
  if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode == kSessModeNoHost) {
    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Skipping for loopback sessions");
    {
      std::lock_guard<std::mutex> stream_lock(fe_dai_csd2_map_.at(fe_dai_name)->stream_mutex);
      if (fe_dai_csd2_map_.at(fe_dai_name)->stream_state != PCM_STATE_RUNNING) {
        fe_dai_csd2_map_.at(fe_dai_name)->stream_state = PCM_STATE_PREPARED;
        fe_dai_csd2_map_.at(fe_dai_name)->csd2_event = 0;
      }
    }
    return IAUD_EOK;
  }

  session_handle = fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle;
  fe_dai_csd2_map_.at(fe_dai_name)->csd2ap_instance = (void*)this;
  if (!session_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "session handle is invalid");
    rc = IAUD_EINVAL;
    return rc;
  }
  AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "StreamPrepare: enter");

  reg_event.event_cb = PlaybackCaptureCallback;
  reg_event.client_data = (void*)fe_dai_csd2_map_.at(fe_dai_name);

  rc = csd2_ioctl(session_handle, CSD2_CMD_REG_EVENT, &reg_event,
      sizeof(reg_event));
  if (rc != IAUD_EOK)
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "csd2_ioctl register event callback failure");

  {
    std::lock_guard<std::mutex> stream_lock(fe_dai_csd2_map_.at(fe_dai_name)->stream_mutex);
    fe_dai_csd2_map_.at(fe_dai_name)->stream_state = PCM_STATE_PREPARED;
    fe_dai_csd2_map_.at(fe_dai_name)->csd2_event = 0;
  }

  return rc;
}

uint32_t CSD2AudioPlugin::StreamRelease(string fe_dai_name) {
  // close session
  uint32_t session_handle;

  AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "StreamRelease: enter");

  if (fe_dai_csd2_map_.find(fe_dai_name) == fe_dai_csd2_map_.end() ||
      fe_dai_csd2_map_.at(fe_dai_name)->be_dai_names.empty()) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "Invalid entry for fe_dai_name %s", fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  session_handle = fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle;
  if (session_handle && csd2_close(session_handle)) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "close session failure");
    return IAUD_EFAILED;
  }

  // clear csd2_devices_ info
  for (auto iter : fe_dai_csd2_map_.at(fe_dai_name)->be_dai_names) {

    auto iter2 = remove(csd2_devices_.at(iter).fe_dai_names.begin(), csd2_devices_.at(iter).fe_dai_names.end(), fe_dai_name);
    csd2_devices_.at(iter).fe_dai_names.erase(iter2, csd2_devices_.at(iter).fe_dai_names.end());

    // if no fe_dai use this be_dai, then clear be's info
    if (csd2_devices_.at(iter).fe_dai_names.size() == 0) {
      csd2_devices_.at(iter).rate = 0;
      csd2_devices_.at(iter).ch = 0;
      csd2_devices_.at(iter).format = 0;
      csd2_devices_.at(iter).data_fmt = 0;
      memset(&csd2_devices_.at(iter).dev_prop_id, 0,
             sizeof(struct PropData));
      csd2_devices_.at(iter).gkv.clear();
      csd2_devices_.at(iter).device_node_id = 0;
    }
  }

  // clear fe_dai_csd2_map_ info
  fe_dai_csd2_map_.at(fe_dai_name)->be_dai_names.clear();
  fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle = 0;
  fe_dai_csd2_map_.at(fe_dai_name)->gkv_str.clear();
  fe_dai_csd2_map_.at(fe_dai_name)->ckv_str.clear();
  fe_dai_csd2_map_.at(fe_dai_name)->tkv_str.clear();
  fe_dai_csd2_map_.at(fe_dai_name)->gkv_strdev.clear();
  fe_dai_csd2_map_.at(fe_dai_name)->tkv_strdev.clear();

  memset(&fe_dai_csd2_map_.at(fe_dai_name)->stream_prop_id, 0,
         sizeof(struct PropData));
  memset(&fe_dai_csd2_map_.at(fe_dai_name)->stream_dev_prop_id, 0,
         sizeof(struct PropData));

  fe_dai_csd2_map_.at(fe_dai_name)->stream_node_id = 0;
  fe_dai_csd2_map_.at(fe_dai_name)->pp_mixer_node_id = 0;
  {
    std::lock_guard<std::mutex> stream_lock(fe_dai_csd2_map_.at(fe_dai_name)->stream_mutex);
    fe_dai_csd2_map_.at(fe_dai_name)->stream_state = PCM_STATE_CLOSED;
    // close related loopback sessions
    if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode == kSessModeNoHost) {
      string loopback_id = fe_dai_csd2_map_.at(fe_dai_name)->loopback_id;
      fe_dai_csd2_map_.at(loopback_id)->stream_state = PCM_STATE_CLOSED;
      fe_dai_csd2_map_.at(loopback_id)->csd2_handle = 0;
    }
  }

  fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.num_buffs = 0;
  fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.buf_size = 0;

  fe_dai_csd2_map_.at(fe_dai_name)->push_pull_data_gvm_fd = -1;
  fe_dai_csd2_map_.at(fe_dai_name)->push_pull_pos_gvm_fd = -1;

  fe_dai_csd2_map_.at(fe_dai_name)->first_stream_read = false;

  fe_dai_csd2_map_.at(fe_dai_name)->stream_cond.notify_all();

  return IAUD_EOK;
}

void CSD2AudioPlugin::ClearSessionInfo() {

  for(auto iter : fe_dai_csd2_map_) {
    std::lock_guard<std::mutex> stream_lock(iter.second->stream_mutex);
    csd2_close(iter.second->csd2_handle);
    iter.second->csd2_handle = 0;
    iter.second->csd2_event |= CSD2_EOS_EVENT;
    iter.second->buf_pool.rw_cond.notify_all();
    iter.second->stream_cond.notify_all();
  }
}

uint32_t CSD2AudioPlugin::StreamStart(string fe_dai_name) {
  uint32_t rc = IAUD_EOK;
  enum CSD2StreamState stream_state;
  uint32_t session_handle;

  AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "StreamStart: enter");

  if (fe_dai_csd2_map_.find(fe_dai_name) == fe_dai_csd2_map_.end() ||
      !fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "StreamStart invalid args for fe_dai_name: %s", fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  session_handle = fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle;

  {
    std::lock_guard<std::mutex> stream_lock(fe_dai_csd2_map_.at(fe_dai_name)->stream_mutex);

    stream_state = fe_dai_csd2_map_.at(fe_dai_name)->stream_state;
    if (stream_state != PCM_STATE_PREPARED)
    {
      if (stream_state == PCM_STATE_RUNNING)
      {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "CSD2 Session Already Started");
        return rc;
      }
      else
      {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "CSD2 stream not in prepared state");
        rc = IAUD_EFAILED;
        return rc;
      }
    }

    // For loopback sessions, only set the stream state to 'running' then return
    // Then, start the csd2 session only when both streams are ready
    if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode == kSessModeNoHost) {
      string loopback_id = fe_dai_csd2_map_.at(fe_dai_name)->loopback_id;
      if (fe_dai_csd2_map_.at(loopback_id)->stream_state != PCM_STATE_RUNNING) {
        fe_dai_csd2_map_.at(fe_dai_name)->stream_state = PCM_STATE_RUNNING;
        return IAUD_EOK;
      }
    }

    rc = csd2_ioctl(session_handle, CSD2_CMD_START, nullptr, 0);
    if (rc != IAUD_EOK)
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2_ioctl start session failure");
    fe_dai_csd2_map_.at(fe_dai_name)->stream_state = PCM_STATE_RUNNING;
  }

  fe_dai_csd2_map_.at(fe_dai_name)->stream_cond.notify_all();

  return rc;
}

uint32_t CSD2AudioPlugin::StreamStop(string fe_dai_name) {
  uint32_t rc = IAUD_EOK;
  enum CSD2StreamState stream_state;
  uint32_t session_handle;
  bool timeout = false;

  AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "StreamStop: enter");

  if (fe_dai_csd2_map_.find(fe_dai_name) == fe_dai_csd2_map_.end() ||
      !fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "StreamStop invalid args for fe_dai_name: %s", fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  session_handle = fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle;
  if (!session_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "session handle is invalid");
    rc = IAUD_EINVAL;
    return rc;
  }

  std::unique_lock<std::mutex> stream_lock(fe_dai_csd2_map_.at(fe_dai_name)->stream_mutex);

  // only send EOS for non-loopback playback
  if ((fe_dai_csd2_map_.at(fe_dai_name)->direction == SNDRV_PCM_STREAM_PLAYBACK) &&
      (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode != kSessModeNoHost) &&
      (fe_dai_csd2_map_.at(fe_dai_name)->push_pull_data_gvm_fd == -1)) {
    rc = csd2_ioctl(session_handle, CSD2_CMD_SET_STREAM_EOS, nullptr, 0);
    if (rc != IAUD_EOK) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_ioctl send stream eos failed");
      return rc;
    }

    timeout = !fe_dai_csd2_map_.at(fe_dai_name)->stream_cond.wait_for(stream_lock,
                                  std::chrono::milliseconds(WaitTimeoutMs(fe_dai_name)),
                                  [stream = fe_dai_csd2_map_.at(fe_dai_name)]{
                                  return stream->csd2_event & CSD2_EOS_EVENT;});
    if (timeout)
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "timed out waiting for EOS response, continuing to stop session");
  }

  // For loopback sessions, only set the stream state to 'prepared', then return
  // Then, stop the csd2 session only when both streams are ready
  if (fe_dai_csd2_map_.at(fe_dai_name)->sess_mode == kSessModeNoHost) {
    string loopback_id_str = fe_dai_csd2_map_.at(fe_dai_name)->loopback_id;
    if (fe_dai_csd2_map_.at(loopback_id_str)->stream_state != PCM_STATE_PREPARED) {
      fe_dai_csd2_map_.at(fe_dai_name)->stream_state = PCM_STATE_PREPARED;
      return IAUD_EOK;
    }
  }

  rc = csd2_ioctl(session_handle, CSD2_CMD_STOP, nullptr, 0);
  if (rc != IAUD_EOK)
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "csd2_ioctl stop session failed");

  fe_dai_csd2_map_.at(fe_dai_name)->stream_state = PCM_STATE_PREPARED;
  fe_dai_csd2_map_.at(fe_dai_name)->stream_cond.notify_all();

  return rc;
}

uint32_t CSD2AudioPlugin::StreamMmap(string fe_dai_name, struct PCMMmapParams &msg) {
  uint32_t rc = IAUD_EOK;
  uint32_t session_handle;
  struct csd2_stream_buffer_config_extern_push_pull buff_config;

  AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "StreamMmap: Enter");

  session_handle = fe_dai_csd2_map_.at(fe_dai_name)->csd2_handle;
  if (!session_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "session handle is invalid");
    return IAUD_EINVAL;
  }

  memset(&buff_config, 0, sizeof(buff_config));

  buff_config.buff_size = msg.data_size;
  buff_config.num_buffs = fe_dai_csd2_map_.at(fe_dai_name)->buf_pool.num_buffs;
  buff_config.start_threshold = 0;
  buff_config.stop_threshold = 0;
  buff_config.push_pull_cfg[0].phandle = (uint64_t*)msg.data_fd;
  buff_config.push_pull_cfg[1].phandle = (uint64_t*)msg.pos_fd;

  rc = csd2_ioctl(session_handle, CSD2_CMD_BUFFER_CONFIG_EXTERN_PUSH_PULL, &buff_config, sizeof(buff_config));
  if (rc != IAUD_EOK) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "csd2_ioctl configure push-pull buffer failed");
    return rc;
  }

  fe_dai_csd2_map_.at(fe_dai_name)->push_pull_data_gvm_fd = msg.data_gvm_fd;
  fe_dai_csd2_map_.at(fe_dai_name)->push_pull_pos_gvm_fd = msg.pos_gvm_fd;

  return IAUD_EOK;
}

uint32_t CSD2AudioPlugin::StreamWrite(struct virtio_snd_pcm_buf_msg &msg) {
  uint32_t rc = IAUD_EOK;
  uint32_t session_handle;
  struct csd2_buffer wr_buff = {0};
  CSD2StreamState stream_state;
  CSD2Stream *stream = nullptr;

  if (fe_dai_csd2_map_.find(msg.fe_dai_name) == fe_dai_csd2_map_.end() ||
      fe_dai_csd2_map_.at(msg.fe_dai_name) == nullptr ||
      !fe_dai_csd2_map_.at(msg.fe_dai_name)->csd2_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "StreamWrite invalid args for msg.fe_dai_name: %s",
            msg.fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  stream = fe_dai_csd2_map_.at(msg.fe_dai_name);

  {
    std::unique_lock<std::mutex> write_lock(stream->stream_mutex);
    session_handle = stream->csd2_handle;
    if (!session_handle) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "session handle is invalid");
      rc = IAUD_EINVAL;
      return rc;
    }

    stream_state = stream->stream_state;
    if (stream_state != PCM_STATE_RUNNING) {
      AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Stream not running!");
      if (stream_state == PCM_STATE_PREPARED) {
          AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "Waiting for trigger");
          stream->stream_cond.wait(write_lock);
          if (stream->stream_state != PCM_STATE_RUNNING) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "Stream still not running, error");
            return IAUD_EFAILED;
          }
      } else {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "failed to write buffer, session not running or prepared");
        return IAUD_EFAILED;
      }
    }
  }

  wr_buff.buf_size = msg.size;
  wr_buff.ext_buf_info.handle = msg.buff_fd;
  wr_buff.ext_buf_info.alloc_size = msg.dma_bytes;
  wr_buff.ext_buf_info.offset = msg.offset;

  rc = csd2_write(session_handle, &wr_buff, sizeof(wr_buff));
  if (rc != wr_buff.buf_size) {
    rc = IAUD_EFAILED;
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "block write err, consumed_size %x expected %x", rc, wr_buff.buf_size);
    return rc;
  }

  return IAUD_EOK;
}

uint32_t CSD2AudioPlugin::StreamRead(struct virtio_snd_pcm_buf_msg &msg)
{
  int32_t rc = IAUD_EOK;
  struct csd2_buffer rd_buff = {0};
  CSD2StreamState stream_state;
  CSD2Stream *stream = nullptr;
  uint32_t session_handle;

  if (fe_dai_csd2_map_.find(msg.fe_dai_name) == fe_dai_csd2_map_.end() ||
      fe_dai_csd2_map_.at(msg.fe_dai_name) == nullptr ||
      !fe_dai_csd2_map_.at(msg.fe_dai_name)->csd2_handle) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "StreamRead invalid args for msg.fe_dai_name: %s", msg.fe_dai_name.c_str());
    return IAUD_EINVAL;
  }

  stream = fe_dai_csd2_map_.at(msg.fe_dai_name);
  session_handle = stream->csd2_handle;

  {
    std::unique_lock<std::mutex> read_lock(stream->stream_mutex);
    stream_state = stream->stream_state;

    if (stream_state != PCM_STATE_RUNNING) {
      AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, , "Stream not running!");
      if (stream_state == PCM_STATE_PREPARED) {
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, , "Waiting for Trigger");
        stream->stream_cond.wait(read_lock);
        if (stream->stream_state != PCM_STATE_RUNNING) {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "Stream still not running, error");
          return IAUD_EFAILED;
        }
      } else {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "failed to read buffer, session not running or prepared");
        return IAUD_EFAILED;
      }
    }
  }

  rd_buff.buf_size = msg.size;
  rd_buff.ext_buf_info.handle = msg.buff_fd;
  rd_buff.ext_buf_info.alloc_size = msg.dma_bytes;
  rd_buff.ext_buf_info.offset = msg.offset;

  rc = csd2_read(session_handle, &rd_buff, sizeof(rd_buff));
  if (rc < 0) {
    rc = IAUD_EFAILED;
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "read error happened");
  } else if (rc == 0) {  // only for non blocking mode
    /* External memory mode always returns 0 for read calls. */
    rc = IAUD_EOK;
  } else if (rc < msg.size) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,  "read < requested, dropping buffer of size %d", rc);
    rc = IAUD_EFAILED;
  } else {
    msg.size = rc;
    rc = IAUD_EOK;
  }

  return rc;
}
