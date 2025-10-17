/**
 * @file audio_plugin_interface.h
 * @brief Abstract class for the IAudioPlugin to have concrete implementations using CSD2 or AlsaLib
 */

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef AUDIO_PLUGIN_INTERFACE_H_
#define AUDIO_PLUGIN_INTERFACE_H_
#include <string>
#include <vector>
#include "SoundCardInfoTypes.h"
#include <functional>

#define __u8    uint8_t
#define __le32  uint32_t

typedef int snd_ctl_elem_type_t;
#define SNDRV_CTL_ELEM_TYPE_NONE       0 /* invalid */
#define SNDRV_CTL_ELEM_TYPE_BOOLEAN    1 /* boolean type */
#define SNDRV_CTL_ELEM_TYPE_INTEGER    2 /* integer type */
#define SNDRV_CTL_ELEM_TYPE_ENUMERATED 3 /* enumerated type */
#define SNDRV_CTL_ELEM_TYPE_BYTES      4 /* byte array */
#define SNDRV_CTL_ELEM_TYPE_IEC958     5 /* IEC958 (S/PDIF) setup */
#define SNDRV_CTL_ELEM_TYPE_INTEGER64  6 /* 64-bit integer type */
#define SNDRV_CTL_ELEM_TYPE_LAST       SNDRV_CTL_ELEM_TYPE_INTEGER64

using namespace std;

enum AccessControl {
  tlv_bytes = 0,
  bytes,
};

enum SSRState {
    SSR_STATE_UNKNOWN = -1,
    SSR_STATE_DOWN,
    SSR_STATE_UP
};

struct CtlElemPayload {
  string name;
  uint32_t control_id;
  snd_ctl_elem_type_t type;   /* R: value type - SNDRV_CTL_ELEM_TYPE_* */
  AccessControl access_control;
  union {
    union {
      long value[128];
    } integer;
    union {
      long long value[64];
    } integer64;
    union {
      unsigned int item[128];
    } enumerated;
    union {
      unsigned char data[512];
    } bytes;
    union {
      uint8_t *tlv;
    } tlv;
  } value;        /* RO */
};

struct PCMSetParams {
  /* size of the hardware buffer */
  __le32 buffer_bytes;
  /* size of the hardware period */
  __le32 period_bytes;
  /* selected feature bit map (1 << VIRTIO_SND_PCM_F_XXX) */
  __le32 features;
  /* selected # of channels */
  __u8 channels;
  /* selected sample format (VIRTIO_SND_PCM_FMT_XXX) */
  __u8 format;
  /* selected frame rate (VIRTIO_SND_PCM_RATE_XXX) */
  __u8 rate;
  /* indicates if this usecase is mmap. In mmap mode,
     the buffer config needs to be done in mmap call */
  __u8 is_mmap_noirq;
};

struct PCMMmapParams {
  uint32_t data_fd;
  uint32_t data_size;
  uint32_t pos_fd;
  uint32_t pos_size;
  uint32_t data_gvm_fd;
  uint32_t pos_gvm_fd;
};

struct MmapBufInfo {
  int32_t data_buf_fd;
  int32_t data_buf_size;
  int32_t pos_buf_fd;
  int32_t pos_buf_size;
};

class IAudioPlugin {
 public:
  // constructor with inputs from the audio_device_backend_manager
  IAudioPlugin(std::vector<FrontendInfo> *all_fe_dai) {
    all_fe_dai_ = all_fe_dai;
  }

  // Each backend needs to implement its down clean up.
  virtual ~IAudioPlugin() {};
  virtual uint32_t DcWrite(const struct CtlElemPayload &ctl_elem_payload) = 0;
  virtual int DcRead(struct CtlElemPayload &ctl_elem_payload) = 0;
  virtual uint32_t StreamSetParam(string fe_dai_name, struct PCMSetParams &set_param_payload) = 0;
  virtual uint32_t SetParamForCapture(string fe_dai_name, struct PCMSetParams &set_param_payload) = 0;
  virtual uint32_t StreamPrepare(string fe_dai_name) = 0;
  virtual uint32_t StreamRelease(string fe_dai_name) = 0;
  virtual uint32_t StreamStart(string fe_dai_name) = 0;
  virtual uint32_t StreamStop(string fe_dai_name) = 0;
  virtual uint32_t StreamWrite(struct virtio_snd_pcm_buf_msg &msg) = 0;
  virtual uint32_t StreamRead(struct virtio_snd_pcm_buf_msg &msg) = 0;

  virtual uint32_t StreamMmap(string fe_dai_name, struct PCMMmapParams &msg) = 0;
  virtual int RegisterSSREvent(std::function<void(enum SSRState)> ssr_cb_func) = 0;
  virtual uint32_t InitClassResource() { return 0; };
  virtual void ReleaseClassResource() {};
  virtual void RegisterPcmMsgCallbackFunc(std::function<void(string &fe_dai_name,
          uint32_t msg_offset)> adm_cb_func) = 0;

 protected:
  std::vector<FrontendInfo> *all_fe_dai_;
};
#endif  // AUDIO_PLUGIN_INTERFACE_H_
