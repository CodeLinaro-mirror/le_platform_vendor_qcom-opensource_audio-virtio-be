
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <errno.h>
#include "AGMAudioPlugin.h"
#include <iostream>
#include <sstream>
#include "plat_dmabuf.h" //GL temp as we need to mmap gvm buffer


#define IAUD_EOK                                  0
#define IAUD_EFAILED                              1
#define IAUD_ENORESOURCE                          2
#define IAUD_EGKVERROR                            3
#define IAUD_ECKVERROR                            4
#define IAUD_EINVAL                               5

#define VMID_KEY 0xDD000000u

enum SetParamType {
  PARAM_TYPE_UNKNOWN,
  PARAM_TYPE_SET_PARAM,
  PARAM_TYPE_SET_PARAM_TAG,
  PARAM_TYPE_SET_PARAM_TAG_ACDB,
};

static const std::unordered_map<std::string, ControlType> controlMap = {
    {"connect", CONNECT},
    {"disconnect", DISCONNECT},
    {"control", CONTROL},
    {"metadata", METADATA},
    {"setParam", SET_PARAM},
    {"setParamTag", SET_PARAM_TAG},
    {"setParamTagACDB", SET_PARAM_TAG_ACDB},
    {"getTaggedInfo", GET_TAGGED_INFO},
    {"event", EVENT},
    {"setCalibration", SET_CALIBRATION},
    {"getParam", GET_PARAM},
    {"getBufInfo", GET_BUF_INFO},
    {"rate", RATE_CH_FMT},
    {"setACDBTunnel", SET_ACDB_TUNNEL},
    {"getACDBTunnel", GET_ACDB_TUNNEL},
    {"loopback", LOOPBACK},
    {"echoReference", ECHO_REFERENCE},
    {"bufTimestamp", BUF_TIMESTAMP},
    {"sidetone", SIDETONE},
    {"datapathParams", DATAPATH_PARAMS},
    {"flush", FLUSH}
};

ControlType AGMAudioPlugin::stringToControlType(const std::string& str) {
    auto it = controlMap.find(str);

    if (it != controlMap.end()) {
        return it->second;
    } else {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "stringToControlType Error Unable to find Control");
        return ControlType::INVALID;
    }
}

const std::unordered_map<int, agm_media_format> virtio2agm_fmt_map = {
    {VIRTIO_SND_PCM_FMT_S8, AGM_FORMAT_PCM_S8},
    {VIRTIO_SND_PCM_FMT_S16, AGM_FORMAT_PCM_S16_LE},
    {VIRTIO_SND_PCM_FMT_S24, AGM_FORMAT_PCM_S24_LE},
    {VIRTIO_SND_PCM_FMT_S24_3, AGM_FORMAT_PCM_S24_3LE},
    {VIRTIO_SND_PCM_FMT_S32, AGM_FORMAT_PCM_S32_LE},
    {VIRTIO_SND_PCM_FMT_U8, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U16, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U24, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U24_3, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U32, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S18_3, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U18_3, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S20, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U20, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_S20_3, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_U20_3, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_FLOAT, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_FLOAT64, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_IMA_ADPCM, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_MU_LAW, AGM_FORMAT_G711},
    {VIRTIO_SND_PCM_FMT_A_LAW, AGM_FORMAT_G711},
    {VIRTIO_SND_PCM_FMT_DSD_U8, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_DSD_U16, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_DSD_U32, AGM_FORMAT_INVALID},
    {VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME, AGM_FORMAT_INVALID},
};

// based on supported PCM frame rates defined in virtio_snd.h
const std::map<uint32_t, uint32_t> virtio2agm_rate_map = {
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
const std::map<uint32_t, uint32_t> virtiot2agm_fmt_map = {
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

AGMAudioPlugin::AGMAudioPlugin(vector<FrontendInfo> *all_fe_dai,
    vector<struct MixerCtlInfo> *all_mixer_ctl_info)
    : IAudioPlugin(all_fe_dai), all_mixer_ctl_info_(all_mixer_ctl_info) {
    if (all_fe_dai != nullptr &&
        all_mixer_ctl_info != nullptr) {
        InitClassResource();
    }
}

AGMAudioPlugin::~AGMAudioPlugin() {
}

// This func should be called after constructor, returns an integer to
// indicate whether initialization was successful. If initialization
// fails, should call the destructor after this func.
uint32_t AGMAudioPlugin::InitClassResource() {
  uint32_t rc;
  int j = 0; /* used for assigning backend id */

  rc = agm_init();

  // init fe_dai_session_id_map_
  int i = 0;
  for (auto iter : *all_fe_dai_) {
    fe_dai_session_id_map_[iter.name].fe_dai_name = iter.name;
    if (iter.direction == SNDRV_PCM_STREAM_PLAYBACK)
        fe_dai_session_id_map_[iter.name].session_config.dir = RX;
    else
        fe_dai_session_id_map_[iter.name].session_config.dir = TX;
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "dai_name %s fe_dai_session_id_map_[iter.name].session_config.dir %x", fe_dai_session_id_map_[iter.name].fe_dai_name.c_str(), fe_dai_session_id_map_[iter.name].session_config.dir);
    fe_dai_session_id_map_[iter.name].session_config.sess_mode = AGM_SESSION_DEFAULT;
    fe_dai_session_id_map_[iter.name].session_config.data_mode = AGM_DATA_EXTERN_MEM;
    fe_dai_session_id_map_[iter.name].push_pull_data_gvm_fd = -1;
    fe_dai_session_id_map_[iter.name].push_pull_pos_gvm_fd = -1;
    fe_dai_session_id_map_[iter.name].push_pull_data_pvm_fd = -1;
    fe_dai_session_id_map_[iter.name].push_pull_pos_pvm_fd = -1;
    fe_dai_session_id_map_[iter.name].push_pull_data_size = 0;
    fe_dai_session_id_map_[iter.name].push_pull_pos_size = 0;
    fe_dai_session_id_map_[iter.name].agm_session_id = i++;
    fe_dai_session_id_map_[iter.name].agm_ap_instance = (void*)this;
    fe_dai_session_id_map_[iter.name].vaddr_dma_area = nullptr; /* GL need this in order to mmap GVM dma buffer */
    fe_dai_session_id_map_[iter.name].be_dai_name = "ZERO";
  }

  for (auto iter : *all_mixer_ctl_info_) {
    // Extract the dai_name
    std::string mixer_ctrl = iter.name.c_str();
    std::istringstream iss(mixer_ctrl);

    std::string dai_name;
    iss >> dai_name;

    if ((std::strstr(dai_name.c_str(), "TDM-")) && (be_dai_aif_id_map_.find(dai_name) == be_dai_aif_id_map_.end()))
      be_dai_aif_id_map_[dai_name] = j++;

#if 0 /* GL debug printout for mixer control mappings */
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "all_mixer_ctl_info_. %s", iter.name.c_str());
    if (iter.enum_mapping.size() > 0){
      for (auto it1 : iter.enum_mapping){
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "enum_mapping %s", it1.c_str());
      }
    }
#endif
  }
  return rc;
}

/*
 * SSRCallbackFunc - static AGM SSR callback.
 *
 * On AUDIO_SVC_DN: close all open AGM sessions to unblock any blocked
 * read/write calls, then notify the upper layer (ADM) via adm_ssr_cb.
 * On AUDIO_SVC_UP: notify the upper layer so it can re-open sessions.
 */
void AGMAudioPlugin::SSRCallbackFunc(enum agm_ssr_event_type event_type,
                                     void *client_data)
{
    enum SSRState ssr_state = SSR_STATE_UNKNOWN;
    AGMAudioPlugin *agm_plug = (AGMAudioPlugin *)client_data;

    switch (event_type) {
    case AGM_SSR_EVENT_AUDIO_SVC_DN:
        ssr_state = SSR_STATE_DOWN;
        break;
    case AGM_SSR_EVENT_AUDIO_SVC_UP:
        ssr_state = SSR_STATE_UP;
        break;
    default:
        AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
                       "SSRCallbackFunc: unsupported event_type %d", event_type);
        return;
    }

    /* On DN: close all open sessions before notifying upper layer */
    if (ssr_state == SSR_STATE_DOWN)
        agm_plug->ClearSessionInfo();

    if (agm_plug->adm_ssr_cb)
        agm_plug->adm_ssr_cb(ssr_state);
}

// This func should be called before destructor, no return value.
void AGMAudioPlugin::ReleaseClassResource() {
  agm_deregister_ssr_cb(SSRCallbackFunc, (void *)this);

  // call agm_deinit()
  agm_deinit();

  // clear fe_dai_session_id_map_
  fe_dai_session_id_map_.clear();
  be_dai_aif_id_map_.clear();
}

int AGMAudioPlugin::RegisterEventCallback(std::function<void(uint32_t)> event_cb_func) {
  return 0;
}

void eventCallback(uint32_t session_id, struct agm_event_cb_params *event_params,
                  void *client_data)
{
    uint32_t event_id = 0;
    void *event_data = NULL;
    uint32_t event_size = 0;
    struct agm_event_read_write_done_payload *agm_rw_done_payload;

    struct AGMsession*agm_session = (struct AGMsession*) client_data;
    AGMAudioPlugin *agm_audio_plug = (AGMAudioPlugin*)agm_session->agm_ap_instance;

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
      "Agm event callback, agm_session");

    if (event_params->event_id == AGM_EVENT_READ_DONE ||
        event_params->event_id == AGM_EVENT_WRITE_DONE) {

        agm_rw_done_payload = (struct agm_event_read_write_done_payload *)event_params->event_payload;


        if (agm_rw_done_payload != nullptr){

           AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,
            "EventCallback FE %s : status %d alloc size %lu timestamp %lx offset %d",
            agm_session->fe_dai_name.c_str(),
            agm_rw_done_payload->status,
            agm_rw_done_payload->buff.alloc_info.alloc_size,
            agm_rw_done_payload->buff.timestamp,
            agm_rw_done_payload->buff.alloc_info.offset);

            agm_session->timestamp =  agm_rw_done_payload->buff.timestamp;
            agm_audio_plug->adm_pcm_msg_cb_func(agm_session->fe_dai_name, agm_rw_done_payload->buff.alloc_info.offset);
        } else {
          AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "eventCallback: agm_rw_done_payload is NULL, fe_dai_name=%s", agm_session->fe_dai_name.c_str());
        }
    } else if (event_params->event_id == AGM_EVENT_EOS_RENDERED ||
               event_params->event_id == AGM_EVENT_EARLY_EOS) {

        if (event_params->event_id == AGM_EVENT_EOS_RENDERED)
            event_id = 0;
        else
            event_id = 1;

        event_data = NULL;
    } else if (event_params->event_id == AGM_EVENT_EARLY_EOS_INTERNAL) {
        /* As it is internal event sent by AGM, don't call sessionCb */
        goto done;
    }

done:
    return;
}

/*
 * in = original metadata buffer (u32 words)
 * If VMID_KEY exists -> no change, *out = in, return 0
 * Else -> alloc new buffer with VMID appended, *out = new, return 1
 */
static int add_vmid_kv(const uint32_t *in, uint32_t in_bytes,
                       uint32_t vmid_val,
                       uint32_t **out, uint32_t *out_bytes)
{
    if (!in || !out || !out_bytes) return -1;
    if (in_bytes < 4 || (in_bytes & 0x3)) return -2; // must be multiple of 4

    uint32_t in_words = in_bytes / 4;
    uint32_t gkv_num  = in[0];

    uint64_t gkv_words = 1ull + 2ull * (uint64_t)gkv_num;
    if (gkv_words > in_words) return -3;

    // Already present? leave as-is
    for (uint32_t i = 0; i < gkv_num; i++) {
        uint32_t key = in[1 + 2*i];
        if (key == VMID_KEY) {
            *out = (uint32_t *)in;
            *out_bytes = in_bytes;
            return 0;
        }
    }

    // Append (VMID_KEY, vmid_val)
    uint32_t new_words = in_words + 2;
    uint32_t *nb = (uint32_t *)calloc(new_words, sizeof(uint32_t));
    if (!nb) return -4;

    nb[0] = gkv_num + 1;

    // copy old pairs
    memcpy(nb + 1, in + 1, (size_t)(2 * gkv_num) * sizeof(uint32_t));

    // add VMID pair
    nb[1 + 2*gkv_num + 0] = VMID_KEY;
    nb[1 + 2*gkv_num + 1] = vmid_val;

    // copy tail
    memcpy(nb + (1 + 2*gkv_num + 2),
           in + (1 + 2*gkv_num),
           (size_t)(in_words - (1 + 2*gkv_num)) * sizeof(uint32_t));

    *out = nb;
    *out_bytes = new_words * 4;
    return 1;
}


uint32_t AGMAudioPlugin::DcWrite(const CtlElemPayload& ctl_elem_payload) {
  std::string ctrl_name = ctl_elem_payload.name;
  std::istringstream iss(ctrl_name);
  std::string dai_name, pcontrol;
  uint32_t aif_id = 0;

  uint32_t session_handle = 0;
  ControlType controlType;
  std::string enum_str;
  unsigned int enum_val = 0;
  int32_t ret = 0;

  iss >> dai_name >> pcontrol;

  AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "dai_name %s, pcontrol %s", dai_name.c_str(), pcontrol.c_str());

  controlType = stringToControlType(pcontrol);

  if (controlType == ControlType::INVALID)
    return IAUD_EINVAL;

  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
    enum_val = ctl_elem_payload.value.enumerated.item[0];
    enum_str = all_mixer_ctl_info_->at(ctl_elem_payload.control_id).enum_mapping.at(enum_val);
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "control_id %d, enum_val %d, enum_str %s",
      ctl_elem_payload.value.enumerated.item[0],
      enum_val,
      enum_str.c_str());
  }

  int counter = 0;
  uint32_t* pMetadata;
  uint32_t pMetadataSize;

  if (dai_name.substr(0,3) == "PCM"){
    if (fe_dai_session_id_map_.find(dai_name) == fe_dai_session_id_map_.end()) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "DcWrite: DAI %s not found in session map", dai_name.c_str());
      return IAUD_EINVAL;
    }
    uint32_t agm_session_id;
    agm_session_id = fe_dai_session_id_map_[dai_name].agm_session_id;
    int ctl_val = fe_dai_session_id_map_[dai_name].ctl_val_;
    string aif_name;
    int32_t aif_id = -1;

    /* handle PCM*/
    switch (controlType){
      case CONNECT:
          aif_name = enum_str;
          if (aif_name == "ZERO")
            aif_id = -1;
          else
            aif_id = be_dai_aif_id_map_.at(aif_name);

          AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "aif_id %d", aif_id);
          ret = agm_session_aif_connect(agm_session_id, aif_id, true);
          break;
      case DISCONNECT:
          aif_name = enum_str;
          if (aif_name == "ZERO")
            aif_id = -1;
          else
            aif_id = be_dai_aif_id_map_.at(aif_name);

          AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "disconnect aif_id %d", aif_id);
          ret = agm_session_aif_connect(agm_session_id, aif_id, false);
          break;
      case CONTROL:
          fe_dai_session_id_map_[dai_name].ctl_val_ = enum_val;
          fe_dai_session_id_map_[dai_name].be_dai_name = all_mixer_ctl_info_->at(ctl_elem_payload.control_id).enum_mapping.at(enum_val);
          AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "DcWrite CONTROL");
          break;
      case METADATA:
          AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "DcWrite METADATA ctl_val %d, agm_session_id %d, direction %d",ctl_val, agm_session_id, fe_dai_session_id_map_[dai_name].session_config.dir);
          AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "DcWrite metadata mixer payload size %d", ctl_elem_payload.value.tlv_data.tlv_size);
          pMetadata = (uint32_t*) ctl_elem_payload.value.tlv_data.tlv;
          pMetadataSize = ctl_elem_payload.value.tlv_data.tlv_size;
          counter = ctl_elem_payload.value.tlv_data.tlv_size/4;
          if (ctl_val == 0){
            uint32_t vmid_val = VMID_KEY | ctl_elem_payload.vmid;
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "DcWrite METADATA: ctl_elem_payload.vmid=0x%x vmid_val=0x%x", ctl_elem_payload.vmid, vmid_val);

            uint32_t *md_send = pMetadata;
            uint32_t  md_send_sz = pMetadataSize;

            uint32_t *md_new = NULL;
            uint32_t  md_new_sz = 0;

            int vmrc = add_vmid_kv(pMetadata, pMetadataSize, vmid_val, &md_new, &md_new_sz);
            if (vmrc == 1) { // appended, use allocated buffer
              md_send = md_new;
              md_send_sz = md_new_sz;
            }

            ret = agm_session_set_metadata(agm_session_id, md_send_sz, (uint8_t *)md_send);
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "ret agm_session_set_metadata %x", ret);
            // Free ONLY if we allocated
            if (md_new) {
              free(md_new);
              md_new = NULL;
            }
          } else {
            aif_name = fe_dai_session_id_map_[dai_name].be_dai_name;

            if (aif_name == "ZERO")
              aif_id = -1;
            else
              aif_id = be_dai_aif_id_map_.at(aif_name);

            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "metadata aif_name %s aif_id %d",
              aif_name.c_str(),
              aif_id);

            ret = agm_session_aif_set_metadata(agm_session_id, aif_id, pMetadataSize, (uint8_t *)pMetadata);
             AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "ret agm_session_aif_set_metadata %x", ret);
          }
          break;
        case GET_PARAM:
            // handle GET_PARAM
            /* GL: this needs to be handled because we are writing the tlv size*/
            break;
      case SET_PARAM:
          if (ctl_val == 0){
            ret = agm_session_set_params(agm_session_id, ctl_elem_payload.value.tlv_data.tlv, ctl_elem_payload.value.tlv_data.tlv_size);
          } else {
            /* aif_id is not carried in the setParam payload; resolve it from the
               currently selected backend, mirroring the METADATA case. Without this
               aif_id stays at its -1 init and reaches AGM as 0xFFFFFFFF. */
            aif_name = fe_dai_session_id_map_[dai_name].be_dai_name;
            if (aif_name == "ZERO")
              aif_id = -1;
            else
              aif_id = be_dai_aif_id_map_.at(aif_name);
            ret = agm_session_aif_set_params(agm_session_id, aif_id, ctl_elem_payload.value.tlv_data.tlv, ctl_elem_payload.value.tlv_data.tlv_size);
          }
          break;
      case SET_PARAM_TAG:
          aif_name = fe_dai_session_id_map_[dai_name].be_dai_name;
          if (aif_name == "ZERO")
            aif_id = -1;
          else
            aif_id = be_dai_aif_id_map_.at(aif_name);
          ret = agm_set_params_with_tag(agm_session_id, aif_id, (struct agm_tag_config *)ctl_elem_payload.value.tlv_data.tlv);
          /* The LA-GVM virtio graph's ACDB currently has no TKV entry for these
             mute/pause controls, so AGM returns -ENODEV. Treat it as a no-op to
             prevent stream teardown.*/
          if (ret == -ENODEV) {
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                "SET_PARAM_TAG: no tag data in ACDB (ret %d), treating as no-op", ret);
            ret = 0;
          }
          break;
      case SET_PARAM_TAG_ACDB:
          aif_name = fe_dai_session_id_map_[dai_name].be_dai_name;
          if (aif_name == "ZERO")
            aif_id = -1;
          else
            aif_id = be_dai_aif_id_map_.at(aif_name);
          ret =  agm_set_params_with_tag_to_acdb(agm_session_id, aif_id, ctl_elem_payload.value.tlv_data.tlv, ctl_elem_payload.value.tlv_data.tlv_size);
          if (ret == -ENODEV) {
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                "SET_PARAM_TAG_ACDB: no tag data in ACDB (ret %d), treating as no-op", ret);
            ret = 0;
          }
          break;
      case EVENT:
          // Handle EVENT
          /* register event here using agm_session_register_for_events */
          break;
      case SET_CALIBRATION:
          aif_name = fe_dai_session_id_map_[dai_name].be_dai_name;

          if (aif_name == "ZERO")
            aif_id = -1;
          else
            aif_id = be_dai_aif_id_map_.at(aif_name);
          AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "metadata aif_name %s aif_id %d", aif_name.c_str(), aif_id);
          if (ctl_val > 0){
            ret = agm_session_aif_set_cal(agm_session_id, aif_id, (struct agm_cal_config *)ctl_elem_payload.value.bytes.data);
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "agm_session_aif_set_cal ret %d", ret);
          }
          break;
      case LOOPBACK:
          // Handle LOOPBACK
          break;
      case ECHO_REFERENCE:
          {
            bool state = true;
            if (enum_val == 0)
              state = false;

            ret = agm_session_set_ec_ref(agm_session_id, aif_id, state);
          }
          break;
      case SET_ACDB_TUNNEL:
      case GET_ACDB_TUNNEL:
      case DATAPATH_PARAMS:
      case FLUSH:
      default:
        //ret = agm_sessionid_flush(agm_session_id);
        /* it is possible to do a flush but no mixer control is exposed at the moment*/

        /* These are not enabled on Automotive */
          // Handle unknown or unexpected value
          break;
    }
  }
  else if (dai_name.substr(0,3) == "TDM"){
    struct agm_media_config media_fmt;
    aif_id = be_dai_aif_id_map_[dai_name];

    switch (controlType){
        case METADATA:
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "DcWrite metadata mixer payload size %d", ctl_elem_payload.value.tlv_data.tlv_size);
            pMetadata = (uint32_t*) ctl_elem_payload.value.tlv_data.tlv;
            pMetadataSize = ctl_elem_payload.value.tlv_data.tlv_size;

            ret = agm_aif_set_metadata(aif_id, pMetadataSize, (uint8_t *)pMetadata);
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "ret agm_aif_set_metadata %x", ret);
            break;
        case SET_PARAM:
            ret = agm_aif_set_params(aif_id, ctl_elem_payload.value.tlv_data.tlv, ctl_elem_payload.value.tlv_data.tlv_size);
            break;
        case RATE_CH_FMT:
            media_fmt.rate = (uint32_t) ctl_elem_payload.value.integer.value[0];
            media_fmt.channels = (uint32_t) ctl_elem_payload.value.integer.value[1];
            media_fmt.format =  virtio2agm_fmt_map.at((int)ctl_elem_payload.value.integer.value[2]);
            media_fmt.data_format = (uint32_t) ctl_elem_payload.value.integer.value[3];

            ret = agm_aif_set_media_config(aif_id, &media_fmt);
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "ret of agm_aif_set_media_config %d", ret);
            break;
        default:
            // Handle unknown or unexpected value
            break;
    }
  }else{
    return IAUD_EINVAL;
  }
  AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "DcWrite ret %d", ret);
  return ret;
}

int AGMAudioPlugin::DcRead(CtlElemPayload& ctl_elem_payload) {
  std::string ctrl_name = ctl_elem_payload.name;
  std::istringstream iss(ctrl_name);
  std::string dai_name, pcontrol;

  int32_t rc = IAUD_EOK;
  uint32_t session_handle = 0;
  ControlType controlType;
  std::string enum_str;
  unsigned int enum_val = 0;
  uint64_t *timestamp = NULL;

  if (!(iss >> dai_name >> pcontrol)) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "DcRead dai_name or pcontrol is empty");
      return IAUD_EINVAL;
  }

  controlType = stringToControlType(pcontrol);

  if (controlType == ControlType::INVALID)
    return IAUD_EINVAL;

  AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "dai_name %s, pcontrol %s", dai_name.c_str(), pcontrol.c_str());

  AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "controlType %d, dai_name %s,",
        controlType,
        dai_name.c_str());

  // Handle all enum type controls: <to be filled>
  if (ctl_elem_payload.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
    AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "Received an unsupported ENUM type command, no-op-ing ctl %s %s",
        dai_name.c_str(), pcontrol.c_str());
  }

  if (dai_name.substr(0,3) == "PCM"){
    if (fe_dai_session_id_map_.find(dai_name) == fe_dai_session_id_map_.end()) {
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
          "DcRead: DAI %s not found in session map", dai_name.c_str());
      return IAUD_EINVAL;
    }
    uint32_t agm_session_id = fe_dai_session_id_map_[dai_name].agm_session_id;
    int32_t ret = 0;
    int ctl_val = fe_dai_session_id_map_[dai_name].ctl_val_;
    string aif_name; //= all_mixer_ctl_info_->at(ctl_elem_payload.control_id).enum_mapping.at(ctl_val);
    int32_t aif_id = 0;
    size_t getSize = 0;
    struct agm_buf_info *buf_info;
    void *payload;
    size_t tlv_size;

      /* handle PCM*/
    switch (controlType) {
        case CONTROL:
            // handle CONTROL
            ctl_elem_payload.value.enumerated.item[0] = ctl_val;
            break;
        case GET_TAGGED_INFO:
            // handle GET_TAGGED_INFO
            aif_name = fe_dai_session_id_map_[dai_name].be_dai_name;
            if (aif_name == "ZERO") {
                ctl_elem_payload.value.tlv_data.tlv_size = 0;
                break;
            }
            else
              aif_id = be_dai_aif_id_map_.at(aif_name);

            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "metadata aif_name %s aif_id %d", aif_name.c_str(), aif_id);

            tlv_size = ctl_elem_payload.value.tlv_data.tlv_size;

            ret = agm_session_aif_get_tag_module_info(agm_session_id, aif_id, NULL, &getSize);
            if (ret || getSize == 0 || tlv_size < getSize) {
              AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,"invalid size, ret %d, tlv_size %zu, getSize %zu",
                ret, tlv_size, getSize);
              return IAUD_EINVAL;
            }

            payload = (void *)&ctl_elem_payload.value.tlv_data.tlv[0];
            ret = agm_session_aif_get_tag_module_info(agm_session_id, aif_id, payload, &getSize);
            if (ret) {
              AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,"agm_session_aif_get_tag_module_info failed ret %d for %d",
                ret, agm_session_id);
            }
            break;

        case EVENT:
            // handle EVENT
            /* from agm plugin this is getting the event from a cached listed. need to see if this is even required.*/
            /* yes */
            break;
        case GET_PARAM:
            // handle GET_PARAM
            break;
        case GET_BUF_INFO: {
            /* Return the GVM-side fds and buffer sizes to GVM PAL.
             * GVM PAL uses these fds to mmap() the shared buffers and
             * establish the zero-copy data path to the DSP. */
            if (ctl_elem_payload.value.tlv_data.tlv_size < sizeof(struct MmapBufInfo)) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                    "GET_BUF_INFO: tlv too small %u < %zu",
                    ctl_elem_payload.value.tlv_data.tlv_size, sizeof(struct MmapBufInfo));
                rc = IAUD_EINVAL;
                break;
            }
            struct MmapBufInfo *buf_info =
                reinterpret_cast<struct MmapBufInfo *>(ctl_elem_payload.value.tlv_data.tlv);
            if (buf_info == nullptr) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                    "GET_BUF_INFO: null buf_info pointer");
                rc = IAUD_EINVAL;
                break;
            }
            if (fe_dai_session_id_map_[dai_name].push_pull_data_gvm_fd < 0 ||
                fe_dai_session_id_map_[dai_name].push_pull_pos_gvm_fd < 0) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                    "GET_BUF_INFO: fds not ready (StreamMmap not called), fe=%s",
                    dai_name.c_str());
                rc = IAUD_EINVAL;
                break;
            }
            buf_info->data_buf_fd   = fe_dai_session_id_map_[dai_name].push_pull_data_gvm_fd;
            buf_info->pos_buf_fd    = fe_dai_session_id_map_[dai_name].push_pull_pos_gvm_fd;
            buf_info->data_buf_size = static_cast<int32_t>(
                fe_dai_session_id_map_[dai_name].push_pull_data_size);
            buf_info->pos_buf_size  = static_cast<int32_t>(
                fe_dai_session_id_map_[dai_name].push_pull_pos_size);
            AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                "GET_BUF_INFO: fe=%s data_gvm_fd=%d pos_gvm_fd=%d "
                "data_size=%d pos_size=%d",
                dai_name.c_str(),
                buf_info->data_buf_fd, buf_info->pos_buf_fd,
                buf_info->data_buf_size, buf_info->pos_buf_size);
            break;
        }
        case BUF_TIMESTAMP:
            // handle BUF_TIMESTAMP
            timestamp = (uint64_t *)ctl_elem_payload.value.bytes.data;
            ret = agm_get_buffer_timestamp(agm_session_id, timestamp);

            AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,"BUF_TIMESTAMP: timestamp= %zu",
              *timestamp);
            break;
        default:
            // handle unknown or unsupported ControlType
            break;
    }
  }else{
    return IAUD_EINVAL;
  }
  return rc;
}

uint32_t AGMAudioPlugin::StreamSetParam(std::string fe_dai_name, PCMSetParams& set_param_payload) {
    int ret = 0;
    if (fe_dai_session_id_map_.find(fe_dai_name) != fe_dai_session_id_map_.end() &&
        fe_dai_session_id_map_[fe_dai_name].agm_handle == 0){
            ret = agm_session_open( fe_dai_session_id_map_[fe_dai_name].agm_session_id,
                                    AGM_SESSION_DEFAULT,
                                    &fe_dai_session_id_map_[fe_dai_name].agm_handle);
              AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "ret of agm_session_open %d", ret);

            if (ret != 0)
              return ret;
    }
    /* session config is filled in from the the init class resources */
    fe_dai_session_id_map_[fe_dai_name].buffer_config.count = set_param_payload.buffer_bytes / set_param_payload.period_bytes;
    fe_dai_session_id_map_[fe_dai_name].buffer_config.size = set_param_payload.period_bytes;

    fe_dai_session_id_map_[fe_dai_name].media_config.rate = virtio2agm_rate_map.at(set_param_payload.rate);
    fe_dai_session_id_map_[fe_dai_name].media_config.channels = set_param_payload.channels;
    fe_dai_session_id_map_[fe_dai_name].media_config.format = virtio2agm_fmt_map.at(set_param_payload.format);
    fe_dai_session_id_map_[fe_dai_name].media_config.data_format = AGM_DATA_FORMAT_FIXED_POINT; /* need to look into this*/

    /* For mmap (aaudio) use case, switch to EXTERN_PUSH_PULL mode.
     * The actual data/pos buf fds will be provided later via StreamMmap()
     * and passed to GSL during configure_buffer_params() at PREPARE time. */
    if (set_param_payload.is_mmap_noirq) {
        fe_dai_session_id_map_[fe_dai_name].session_config.data_mode = AGM_DATA_EXTERN_PUSH_PULL;
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
            "StreamSetParam: mmap mode detected, data_mode set to AGM_DATA_EXTERN_PUSH_PULL");
    }

    ret = agm_session_set_config(  fe_dai_session_id_map_[fe_dai_name].agm_handle,
                                    &fe_dai_session_id_map_[fe_dai_name].session_config,
                                    &fe_dai_session_id_map_[fe_dai_name].media_config,
                                    &fe_dai_session_id_map_[fe_dai_name].buffer_config);

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "ret of agm_session_set_config %d", ret);
    return ret;
}

uint32_t AGMAudioPlugin::SetParamForCapture(std::string fe_dai_name, PCMSetParams& set_param_payload) {
   return StreamSetParam(fe_dai_name, set_param_payload);
}

uint32_t AGMAudioPlugin::StreamPrepare(std::string fe_dai_name) {
    /* this is short function in agm plugin
        it should be noted that agm plugin tracks the pos buf wallclk lsw and msw*/

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV, "StreamPrepare called fe_dai_name=%s", fe_dai_name.c_str());
    int ret = 0;
    uint32_t agm_session_id;
    uint64_t agm_handle;
    {
        std::lock_guard<std::mutex> stream_lock(fe_dai_session_id_map_[fe_dai_name].stream_mutex);
        agm_session_id = fe_dai_session_id_map_[fe_dai_name].agm_session_id;
        agm_handle     = fe_dai_session_id_map_[fe_dai_name].agm_handle;
    }

    ret = agm_session_register_cb(agm_session_id,
                                  eventCallback, AGM_EVENT_DATA_PATH, (void*)&(fe_dai_session_id_map_[fe_dai_name]));

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,"ret agm_session_register_cb Data %d", ret);

    if (ret != 0){
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
        "AP::StreamPrepare: error: agm_session_register_cb ");
      return IAUD_EINVAL;
    }
    ret = agm_session_register_cb(agm_session_id,
                                  eventCallback, AGM_EVENT_MODULE, (void*)&(fe_dai_session_id_map_[fe_dai_name]));

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,"ret agm_session_register_cb Event %d", ret);

    if (ret != 0){
      AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
      "AP::StreamPrepare: error: agm_session_register_cb ");
      return IAUD_EINVAL;
    }

    /* For EXTERN_PUSH_PULL (MMAP/AAudio) mode, pass shared buffer fds to AGM before prepare */
    if (fe_dai_session_id_map_[fe_dai_name].session_config.data_mode == AGM_DATA_EXTERN_PUSH_PULL) {
        if (fe_dai_session_id_map_[fe_dai_name].push_pull_data_pvm_fd < 0 ||
            fe_dai_session_id_map_[fe_dai_name].push_pull_pos_pvm_fd < 0) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "StreamPrepare: EXTERN_PUSH_PULL but pvm fds not ready (StreamMmap not called)");
            return IAUD_EINVAL;
        }
        struct agm_buf_info buf_info = {};
        buf_info.data_buf_fd   = fe_dai_session_id_map_[fe_dai_name].push_pull_data_pvm_fd;
        buf_info.data_buf_size = fe_dai_session_id_map_[fe_dai_name].push_pull_data_size;
        buf_info.pos_buf_fd    = fe_dai_session_id_map_[fe_dai_name].push_pull_pos_pvm_fd;
        buf_info.pos_buf_size  = fe_dai_session_id_map_[fe_dai_name].push_pull_pos_size;
        ret = agm_session_set_buf_info(fe_dai_session_id_map_[fe_dai_name].agm_session_id,
                                       &buf_info, DATA_BUF | POS_BUF);
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
            "StreamPrepare: agm_session_set_buf_info data_fd=%d pos_fd=%d ret=%d",
            buf_info.data_buf_fd, buf_info.pos_buf_fd, ret);
        if (ret) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "StreamPrepare: agm_session_set_buf_info failed ret=%d", ret);
            return IAUD_EINVAL;
        }
    }


    ret = agm_session_prepare(agm_handle);
    if (ret)
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,"agm_session_prepare failed ret %d", ret);
    else
        AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,"agm_session_prepare ret %d", ret);

    return ret;
}

uint32_t AGMAudioPlugin::StreamRelease(std::string fe_dai_name) {
    int ret = 0;
    AGMsession &sess = fe_dai_session_id_map_[fe_dai_name];
    std::lock_guard<std::mutex> stream_lock(sess.stream_mutex);

    /* Unregister callbacks to avoid duplicate entries on next StreamPrepare */
    agm_session_register_cb(sess.agm_session_id,
                            NULL, AGM_EVENT_DATA_PATH, (void*)&sess);
    agm_session_register_cb(sess.agm_session_id,
                            NULL, AGM_EVENT_MODULE, (void*)&sess);

    ret = agm_session_close(sess.agm_handle);
    sess.agm_handle = 0;
    sess.ctl_val_ = 0;
    sess.timestamp = 0;
    sess.be_dai_name = "ZERO";
    sess.push_pull_data_gvm_fd = -1;
    sess.push_pull_pos_gvm_fd = -1;
    sess.push_pull_data_pvm_fd = -1;
    sess.push_pull_pos_pvm_fd  = -1;
    sess.push_pull_data_size   = 0;
    sess.push_pull_pos_size    = 0;
    sess.session_config.data_mode = AGM_DATA_EXTERN_MEM;
    memset(&sess.media_config, 0, sizeof(agm_media_config));
    memset(&sess.buffer_config, 0, sizeof(agm_buffer_config));
    /*GL unmap dma area*/
    munmap(sess.vaddr_dma_area, sess.dma_area_size);
    sess.vaddr_dma_area = nullptr;
    sess.dma_area_size = 0;
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,"StreamRelease ");
    return ret;
}


uint32_t AGMAudioPlugin::StreamStart(std::string fe_dai_name) {
    int32_t ret = 0;
    uint64_t agm_handle;
    {
        std::lock_guard<std::mutex> stream_lock(fe_dai_session_id_map_[fe_dai_name].stream_mutex);
        agm_handle = fe_dai_session_id_map_[fe_dai_name].agm_handle;
    }
    ret = agm_session_start(agm_handle);

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,"ret agm_session_start %d", ret);

    return ret;
}

uint32_t AGMAudioPlugin::StreamStop(std::string fe_dai_name) {
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,"StreamStop ");
    uint64_t agm_handle;
    {
        std::lock_guard<std::mutex> stream_lock(fe_dai_session_id_map_[fe_dai_name].stream_mutex);
        agm_handle = fe_dai_session_id_map_[fe_dai_name].agm_handle;
    }
    return agm_session_stop(agm_handle);
}

uint32_t AGMAudioPlugin::StreamWriteDone(struct virtio_snd_pcm_buf_msg& msg) {
    return 0;
}
uint32_t AGMAudioPlugin::StreamWrite(struct virtio_snd_pcm_buf_msg& msg) {
    size_t size = 512;
    uint64_t agm_handle;
    struct agm_buff buff_info;
    size_t bytes_written = 0;
    int ret = 0;

    memset(&buff_info, 0, sizeof(agm_buff));

    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV,"StreamWrite enter");

    if (fe_dai_session_id_map_.find(msg.fe_dai_name) == fe_dai_session_id_map_.end()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "StreamWrite invalid args for msg.fe_dai_name: %s",
                msg.fe_dai_name.c_str());
        return IAUD_EINVAL;
    }

    AGMsession &sess = fe_dai_session_id_map_.at(msg.fe_dai_name);
    {
        std::lock_guard<std::mutex> stream_lock(sess.stream_mutex);
        agm_handle = sess.agm_handle;
        if (!agm_handle) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                    "StreamWrite invalid args for msg.fe_dai_name: %s",
                    msg.fe_dai_name.c_str());
            return IAUD_EINVAL;
        }
        if (sess.vaddr_dma_area == nullptr) {
            sess.vaddr_dma_area = mmap(NULL, msg.dma_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, msg.buff_fd, 0);
            sess.dma_area_size = msg.dma_bytes;
            if (sess.vaddr_dma_area == (void*)-1) {
                AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV, "AGMAudioPlugin - mmap failed with err\n");
            } else {
                AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "AGMAudioPlugin - mmap succeeded %x\n", sess.vaddr_dma_area);
            }
        }
    }
    buff_info.timestamp = 0;
    buff_info.flags = 0;
    buff_info.size = msg.size;
    //buff_info.addr = nullptr;
    /* GL use the mmap'd buffer for now.*/
    //buff_info.addr = (uint8_t *)(fe_dai_session_id_map_.at(msg.fe_dai_name).vaddr_dma_area + msg.offset);
    buff_info.metadata_size = 0;
    buff_info.metadata = nullptr;
    buff_info.alloc_info.alloc_handle = msg.buff_fd;
    buff_info.alloc_info.alloc_size = msg.dma_bytes;
    buff_info.alloc_info.offset = msg.offset;

    AUDIO_BASE_LOG(BASE_LOG_DEBUG, MODULE_VIO_AUD_DEV, "StreamWrite - alloc_handle %x, alloc_size %d, offset %d\n", buff_info.alloc_info.alloc_handle,buff_info.alloc_info.alloc_size,buff_info.alloc_info.offset);

    /* GarmondL in csd2ap there is a bunch of mutex to ensure flow do we need this?*/
    ret = agm_session_write_with_metadata(agm_handle, &buff_info, &bytes_written);

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,"agm_session_write_with_metadata %d", ret);

    return ret;
}

uint32_t AGMAudioPlugin::StreamRead(struct virtio_snd_pcm_buf_msg& msg) {
    uint64_t agm_handle;
    struct agm_buff buff_info;
    uint32_t bytes_read = 0;
    int ret = 0;

    memset(&buff_info, 0, sizeof(agm_buff));

    if (fe_dai_session_id_map_.find(msg.fe_dai_name) == fe_dai_session_id_map_.end()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                "StreamRead invalid args for msg.fe_dai_name: %s",
                msg.fe_dai_name.c_str());
        return IAUD_EINVAL;
    }

    AGMsession &sess = fe_dai_session_id_map_.at(msg.fe_dai_name);
    {
        std::lock_guard<std::mutex> stream_lock(sess.stream_mutex);
        agm_handle = sess.agm_handle;
        if (!agm_handle) {
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                    "StreamRead invalid args for msg.fe_dai_name: %s",
                    msg.fe_dai_name.c_str());
            return IAUD_EINVAL;
        }
    }
    buff_info.timestamp = 0;
    buff_info.flags = 0;
    buff_info.size = msg.size;
    buff_info.addr = nullptr;
    buff_info.metadata_size = 0;
    buff_info.metadata = nullptr;
    buff_info.alloc_info.alloc_handle = msg.buff_fd;
    buff_info.alloc_info.alloc_size = msg.dma_bytes;
    buff_info.alloc_info.offset = msg.offset;

    /* GarmondL in csd2ap there is a bunch of mutex to ensure flow do we need this?*/
    ret =  agm_session_read_with_metadata(agm_handle, &buff_info, &bytes_read);

    msg.size = bytes_read;

    return ret;
}

uint32_t AGMAudioPlugin::StreamMmap(std::string fe_dai_name, PCMMmapParams& msg) {
    if (fe_dai_session_id_map_.find(fe_dai_name) == fe_dai_session_id_map_.end()) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
            "StreamMmap: unknown fe_dai_name %s", fe_dai_name.c_str());
        return IAUD_EINVAL;
    }

    AGMsession &sess = fe_dai_session_id_map_[fe_dai_name];

    /* Store PVM-side fds — these will be passed to AGM/GSL at configure_buffer_params() time */
    sess.push_pull_data_pvm_fd = static_cast<int>(msg.data_fd);
    sess.push_pull_pos_pvm_fd  = static_cast<int>(msg.pos_fd);
    /* Store GVM-side fds — returned to GVM PAL via DcRead(getBufInfo) */
    sess.push_pull_data_gvm_fd = static_cast<int>(msg.data_gvm_fd);
    sess.push_pull_pos_gvm_fd  = static_cast<int>(msg.pos_gvm_fd);
    /* Store sizes — also returned via getBufInfo */
    sess.push_pull_data_size   = msg.data_size;
    sess.push_pull_pos_size    = msg.pos_size;

    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
        "StreamMmap: fe=%s data_pvm_fd=%d pos_pvm_fd=%d data_gvm_fd=%d pos_gvm_fd=%d "
        "data_size=%u pos_size=%u",
        fe_dai_name.c_str(),
        sess.push_pull_data_pvm_fd, sess.push_pull_pos_pvm_fd,
        sess.push_pull_data_gvm_fd, sess.push_pull_pos_gvm_fd,
        sess.push_pull_data_size,   sess.push_pull_pos_size);

    return IAUD_EOK;
}

void AGMAudioPlugin::ClearSessionInfo()
{
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                   "ClearSessionInfo: closing all open AGM sessions due to SSR");

    for (auto &iter : fe_dai_session_id_map_) {
        AGMsession &sess = iter.second;
        std::lock_guard<std::mutex> stream_lock(sess.stream_mutex);

        if (sess.agm_handle == 0)
            continue;
        agm_session_register_cb(sess.agm_session_id, NULL, AGM_EVENT_DATA_PATH, (void*)&sess);
        agm_session_register_cb(sess.agm_session_id, NULL, AGM_EVENT_MODULE, (void*)&sess);

        if (sess.vaddr_dma_area != nullptr) {
            munmap(sess.vaddr_dma_area, sess.dma_area_size);
            sess.vaddr_dma_area = nullptr;
            sess.dma_area_size = 0;
        }

        int ret = agm_session_close(sess.agm_handle);
        if (ret)
            AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                           "ClearSessionInfo: agm_session_close failed ret=%d for %s",
                           ret, iter.first.c_str());

        sess.agm_handle = 0;
        sess.ctl_val_ = 0;
        sess.timestamp = 0;
        sess.be_dai_name = "ZERO";
        sess.push_pull_data_gvm_fd = -1;
        sess.push_pull_pos_gvm_fd = -1;
        memset(&sess.media_config, 0, sizeof(agm_media_config));
        memset(&sess.buffer_config, 0, sizeof(agm_buffer_config));
    }
}

int AGMAudioPlugin::RegisterSSREvent(std::function<void(enum SSRState)> ssr_cb_func) {
    int rc = agm_register_ssr_cb(SSRCallbackFunc, (void *)this);
    if (rc != 0) {
        AUDIO_BASE_LOG(BASE_LOG_ERROR, MODULE_VIO_AUD_DEV,
                       "RegisterSSREvent: agm_register_ssr_cb failed rc=%d", rc);
        return rc;
    }
    adm_ssr_cb = ssr_cb_func;
    AUDIO_BASE_LOG(BASE_LOG_INFO, MODULE_VIO_AUD_DEV,
                   "RegisterSSREvent: SSR callback registered successfully");
    return 0;
}

void AGMAudioPlugin::RegisterPcmMsgCallbackFunc(std::function<void(std::string& fe_dai_name, uint32_t msg_offset)> adm_cb_func) {
    std::cout << "RegisterPcmMsgCallbackFunc not supported in AGM\n";
    /* callback for read / write done can be handled by the agm_buf info in the call back */
    adm_pcm_msg_cb_func = adm_cb_func;
}

extern "C" {

/*
 *
 * createAGMAudioPlugin / destroyAGMAudioPlugin use C linkage (no name
 * mangling) so that the caller can locate them via dlsym().  However,
 * their signatures contain C++ STL types (std::vector).  This is safe
 * only when the caller and this DSO are compiled with the same C++ ABI
 * and linked against the same STL implementation (e.g. both using
 * libc++ from the same toolchain).  If either side is ever built with a
 * different toolchain or STL variant, the std::vector layout may differ
 * and the call will produce undefined behaviour.  Keep caller and callee
 * in sync.
 */
IAudioPlugin* createAGMAudioPlugin(std::vector<FrontendInfo>* all_fe_dai,
                                    std::vector<MixerCtlInfo>* all_mixer_ctl_info)
{
    return new AGMAudioPlugin(all_fe_dai, all_mixer_ctl_info);
}

void destroyAGMAudioPlugin(IAudioPlugin* plugin)
{
    delete plugin;
}

} // extern "C"
