/*
 * Copyright (C) 2020  OpenSynergy GmbH
 *
 * This header is BSD licensed so anyone can use the definitions to
 * implement compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of OpenSynergy GmbH nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

// TODO: we should just include the full and proper virtio_snd.h file
// just take snippets during development.

#ifndef __VIRTIO_SND_H__
#define __VIRTIO_SND_H__

#define VIRTIO_SND_DC_NAME_MAXLEN   64

#include "sound/asound.h"

#ifndef __bitwise
#define __bitwise __attribute__((bitwise))
#endif

#ifndef __force
#define __force __attribute__((force))
#endif

typedef unsigned long dma_addr_t;

typedef uint8_t __u8;
typedef uint32_t __u32;
//typedef uint64_t __u64;
typedef uint16_t __u16;

typedef __u32 __bitwise __le32;
typedef __u64 __bitwise  __le64;
typedef __u16 __bitwise __le16;

enum {
  /* jack control request types */
  VIRTIO_SND_R_JACK_INFO = 1,
  VIRTIO_SND_R_JACK_REMAP,

  /* PCM control request types */
  VIRTIO_SND_R_PCM_INFO = 0x0100,
  VIRTIO_SND_R_PCM_SET_PARAMS,
  VIRTIO_SND_R_PCM_PREPARE,
  VIRTIO_SND_R_PCM_RELEASE,
  VIRTIO_SND_R_PCM_START,
  VIRTIO_SND_R_PCM_STOP,
  VIRTIO_SND_R_EXPORT_MSG,
  VIRTIO_SND_R_PCM_MSG,

  /* channel map control request types */
  VIRTIO_SND_R_CHMAP_INFO = 0x0200,

  /* jack event types */
  VIRTIO_SND_EVT_JACK_CONNECTED = 0x1000,
  VIRTIO_SND_EVT_JACK_DISCONNECTED,

  /* PCM event types */
  VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED = 0x1100,
  VIRTIO_SND_EVT_PCM_XRUN,

  /* common status codes */
  VIRTIO_SND_S_OK = 0x8000,
  VIRTIO_SND_S_BAD_MSG,
  VIRTIO_SND_S_NOT_SUPP,
  VIRTIO_SND_S_IO_ERR,

  /* ALSA device controls */
  VIRTIO_SND_R_DC_INFO = 0xf200,
  VIRTIO_SND_R_DC_ENUM_INFO,
  VIRTIO_SND_R_DC_READ,
  VIRTIO_SND_R_DC_WRITE,
  VIRTIO_SND_R_DC_TLV_READ,
  VIRTIO_SND_R_DC_TLV_WRITE,
  VIRTIO_SND_R_DC_TLV_COMMAND,

  /* ALSA device control events */
  VIRTIO_SND_EVT_DC_NOTIFY = 0xff00,

  /* qualcomm extensions */
  VIRTIO_SND_R_PCM_COUNT,
  VIRTIO_SND_R_DC_COUNT,

  VIRTIO_SND_R_PCM_MMAP,

  VIRTIO_SND_EVT_SSR,
};

/* supported PCM stream features */
enum {
   VIRTIO_SND_PCM_F_SHMEM_HOST = 0,
   VIRTIO_SND_PCM_F_SHMEM_GUEST,
   VIRTIO_SND_PCM_F_MSG_POLLING,
   VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS,
   VIRTIO_SND_PCM_F_EVT_XRUNS,
   VIRTIO_SND_PCM_F_HOSTLESS
};

/* supported PCM sample formats */
enum {
   /* analog formats (width / physical width) */
   VIRTIO_SND_PCM_FMT_IMA_ADPCM = 0,   /*  4 /  4 bits */
   VIRTIO_SND_PCM_FMT_MU_LAW,      /*  8 /  8 bits */
   VIRTIO_SND_PCM_FMT_A_LAW,       /*  8 /  8 bits */
   VIRTIO_SND_PCM_FMT_S8,          /*  8 /  8 bits */
   VIRTIO_SND_PCM_FMT_U8,          /*  8 /  8 bits */
   VIRTIO_SND_PCM_FMT_S16,         /* 16 / 16 bits */
   VIRTIO_SND_PCM_FMT_U16,         /* 16 / 16 bits */
   VIRTIO_SND_PCM_FMT_S18_3,       /* 18 / 24 bits */
   VIRTIO_SND_PCM_FMT_U18_3,       /* 18 / 24 bits */
   VIRTIO_SND_PCM_FMT_S20_3,       /* 20 / 24 bits */
   VIRTIO_SND_PCM_FMT_U20_3,       /* 20 / 24 bits */
   VIRTIO_SND_PCM_FMT_S24_3,       /* 24 / 24 bits */
   VIRTIO_SND_PCM_FMT_U24_3,       /* 24 / 24 bits */
   VIRTIO_SND_PCM_FMT_S20,         /* 20 / 32 bits */
   VIRTIO_SND_PCM_FMT_U20,         /* 20 / 32 bits */
   VIRTIO_SND_PCM_FMT_S24,         /* 24 / 32 bits */
   VIRTIO_SND_PCM_FMT_U24,         /* 24 / 32 bits */
   VIRTIO_SND_PCM_FMT_S32,         /* 32 / 32 bits */
   VIRTIO_SND_PCM_FMT_U32,         /* 32 / 32 bits */
   VIRTIO_SND_PCM_FMT_FLOAT,       /* 32 / 32 bits */
   VIRTIO_SND_PCM_FMT_FLOAT64,     /* 64 / 64 bits */
   /* digital formats (width / physical width) */
   VIRTIO_SND_PCM_FMT_DSD_U8,      /*  8 /  8 bits */
   VIRTIO_SND_PCM_FMT_DSD_U16,     /* 16 / 16 bits */
   VIRTIO_SND_PCM_FMT_DSD_U32,     /* 32 / 32 bits */
   VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME  /* 32 / 32 bits */
};

/* supported PCM frame rates */
enum {
   VIRTIO_SND_PCM_RATE_5512 = 0,
   VIRTIO_SND_PCM_RATE_8000,
   VIRTIO_SND_PCM_RATE_11025,
   VIRTIO_SND_PCM_RATE_12000,
   VIRTIO_SND_PCM_RATE_16000,
   VIRTIO_SND_PCM_RATE_22050,
   VIRTIO_SND_PCM_RATE_24000,
   VIRTIO_SND_PCM_RATE_32000,
   VIRTIO_SND_PCM_RATE_44100,
   VIRTIO_SND_PCM_RATE_48000,
   VIRTIO_SND_PCM_RATE_64000,
   VIRTIO_SND_PCM_RATE_88200,
   VIRTIO_SND_PCM_RATE_96000,
   VIRTIO_SND_PCM_RATE_176400,
   VIRTIO_SND_PCM_RATE_192000,
   VIRTIO_SND_PCM_RATE_384000
};

/*******************************************************************************
 * PCM I/O MESSAGES
 */

/* I/O request header */
struct virtio_snd_pcm_xfer {
  /* 0 ... virtio_snd_config::streams - 1 */
  __le32 stream_id;
};

/* I/O request status */
struct virtio_snd_pcm_status {
  /* VIRTIO_SND_S_XXX */
  __le32 status;
  /* current device latency */
  __le32 latency_bytes;
};


struct virtio_snd_hdr {
   __le32 code;
};

struct virtio_snd_pcm_hdr {
  struct virtio_snd_hdr hdr;
   __le32 stream_id;
};

struct virtio_snd_dc_hdr {
   struct virtio_snd_hdr hdr;
   __le16 control_id;
   __le16 subcontrol_id;
};

struct virtio_snd_query_info {
   struct virtio_snd_hdr hdr;
   __le32 start_id;
   __le32 count;
   __le32 size;
} __attribute__((packed));

struct virtio_snd_info {
   __le32 hda_fn_nid;
};

struct virtio_snd_count_msg {
   struct virtio_snd_hdr request;
   struct virtio_snd_hdr response;
   __le32 count;
} __attribute__((packed));

struct virtio_snd_pcm_info {
   struct virtio_snd_info hdr;
   __le32 features;
   __le64 formats;
   __le64 rates;
   __u8 direction;
   __u8 channels_min;
   __u8 channels_max;

   __u8 padding[5];
};

struct virtio_snd_pcm_info_msg {
   struct virtio_snd_query_info request;
   struct virtio_snd_hdr response;
   struct virtio_snd_pcm_info response_ext[0];
} __attribute__((packed));

struct virtio_snd_dc_info {
   struct virtio_snd_info hdr;
   struct snd_ctl_elem_info elem_info;
};

struct virtio_snd_dc_info_msg {
   struct virtio_snd_query_info request;
   struct virtio_snd_hdr response;
   struct virtio_snd_dc_info response_ext[0];
} __attribute__((packed));

struct virtio_snd_dc_enum_value {

  __u8 name[VIRTIO_SND_DC_NAME_MAXLEN];
};

struct virtio_snd_dc_enum_info_msg {
    struct virtio_snd_dc_hdr request;
    struct virtio_snd_hdr response;
    struct virtio_snd_dc_enum_value response_ext[0];
} __attribute__((packed));

struct virtio_snd_dc_read_msg {
    struct virtio_snd_dc_hdr request;
    struct virtio_snd_hdr response;
    struct snd_ctl_elem_value response_ext;
} __attribute__((packed));

struct virtio_snd_dc_write_msg {
    struct virtio_snd_dc_hdr request;
    struct snd_ctl_elem_value request_ext;
    struct virtio_snd_hdr response;
} __attribute__((packed));

struct virtio_snd_dc_tlv_read_msg {
    struct virtio_snd_dc_hdr request;
    struct virtio_snd_hdr response;
    unsigned int tlv[0];
} __attribute__((packed));

struct virtio_snd_dc_tlv_write_msg {
    struct virtio_snd_dc_hdr request;
    unsigned int tlv[0];
//    struct virtio_snd_hdr response;
} __attribute__((packed));

struct virtio_snd_pcm_set_params {
 /* .code = VIRTIO_SND_R_PCM_SET_PARAMS */
 struct virtio_snd_pcm_hdr hdr;
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

 __u8 is_mmap_noirq;
};

struct virtio_snd_pcm_set_params_msg {
  struct virtio_snd_pcm_set_params request;
  struct virtio_snd_hdr response;
} __attribute__((packed));

struct virtio_snd_pcm_trigger_msg {
  struct virtio_snd_pcm_hdr request;
  struct virtio_snd_hdr response;
};

struct virtio_snd_pcm_prepare_msg {
  struct virtio_snd_pcm_hdr request;
  struct virtio_snd_hdr response;
};

struct scatterlist {
  unsigned long page_link;
  unsigned int  offset;
  unsigned int  length;
  dma_addr_t  dma_address;
  unsigned int dma_length;
};

enum pcm_msg_sg_index {
  PCM_MSG_SG_XFER = 0,
  PCM_MSG_SG_DATA,
  PCM_MSG_SG_STATUS,
  PCM_MSG_SG_MAX
};

struct virtio_pcm_msg {
   void *substream;
   struct virtio_snd_pcm_xfer xfer;
   struct virtio_snd_pcm_status status;
   size_t length;
   unsigned int sid;
//   unsigned char buffer[4096];

   struct dma_data_desc {
      uint64_t addr;
      uint32_t offset;
      uint32_t period;
      uint32_t dma_bytes;
      uint32_t export_id;
   } desc;

   struct scatterlist sgs[PCM_MSG_SG_MAX];
};

struct virtio_snd_pcm_buf_msg {
  int64_t buff_fd;
  uint32_t size;
  uint32_t offset;
  std::string fe_dai_name;
  uint32_t dma_bytes;
};

struct virtio_snd_pcm_push_pull_info {
        struct virtio_snd_pcm_hdr hdr;
        uint32_t data_fd;
        uint32_t pos_fd;
        uint32_t data_size;
        uint32_t pos_size;
        uint32_t data_export_id;
        uint32_t pos_export_id;
} __attribute__((packed));

struct virtio_snd_pcm_mmap_msg {
    struct virtio_snd_pcm_push_pull_info request;
    struct virtio_snd_hdr response;
} __attribute__((packed));

struct virtio_snd_event {
    struct virtio_snd_hdr hdr;
    uint32_t data;
};

#endif
