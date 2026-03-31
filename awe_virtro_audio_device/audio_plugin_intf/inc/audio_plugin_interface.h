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

using namespace std;

enum CtlElemAccessControl {
  ACCESS_TLV_READWRITE = 0,
  ACCESS_READWRITE,
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
  CtlElemAccessControl access_control;
  union {
    union {
      int32_t value[128];
    } integer;
    union {
      int64_t value[64];
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

  /* the dmabuf file descriptor for the data buffer */
  __le64 buff_fd;
};

#if defined(__QNX__)
struct PCMMmapParams {
  uint64_t data_fd;
  uint32_t data_size;
  uint64_t pos_fd;
  uint32_t pos_size;
  uint32_t data_gvm_fd;
  uint32_t pos_gvm_fd;
};
#else
struct PCMMmapParams {
  uint32_t data_fd;
  uint32_t data_size;
  uint32_t pos_fd;
  uint32_t pos_size;
  uint32_t data_gvm_fd;
  uint32_t pos_gvm_fd;
};
#endif

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
  virtual uint32_t StreamPrepare(string fe_dai_name) = 0;
  virtual uint32_t StreamRelease(string fe_dai_name) = 0;
  virtual uint32_t StreamStart(string fe_dai_name) = 0;
  virtual uint32_t StreamStop(string fe_dai_name) = 0;
  virtual uint32_t StreamWrite(struct virtio_snd_pcm_buf_msg &msg) = 0; // need to review payload
  virtual uint32_t StreamWriteDone(struct virtio_snd_pcm_buf_msg &msg) = 0; // need to review payload
  virtual int StreamRead(struct virtio_snd_pcm_buf_msg &msg) = 0;  // need to review payload
  virtual uint32_t StreamMmap(string fe_dai_name, struct PCMMmapParams &msg) = 0;
  virtual int RegisterSSREvent(std::function<void(enum SSRState)> ssr_cb_func) = 0;
  virtual int RegisterEventCallback(std::function<void(uint32_t control_id)> event_cb_func) = 0;
  virtual uint32_t InitClassResource() { return 0; };
  virtual void ReleaseClassResource() {};

 protected:
  std::vector<FrontendInfo> *all_fe_dai_;
};
#endif  // AUDIO_PLUGIN_INTERFACE_H_
