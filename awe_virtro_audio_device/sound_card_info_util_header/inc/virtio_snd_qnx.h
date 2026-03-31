/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __VIRTIO_SND_QNX_H__
#define __VIRTIO_SND_QNX_H__
#include <stdio.h>
#include <stddef.h>

#define COPY_VALUE_TO_BUF(buf, buf_offset, val, val_offset, size)    memcpy((char *)buf + buf_offset, (char *)val + val_offset, size)

#define VIRTIO_CTL_ELEM_ID_NAME_MAXLEN          (44)
#define VIRTIO_EVENT_MASK_VALUE                 (1)
#define VIRTIO_CTL_ID_IFACE_MIXER               (2)
#define VIRTIO_STREAM_PLAYBACK                  (0)
#define VIRTIO_STREAM_CAPTURE                   (1)

typedef enum _virtio_mixer_ctl_mask {
   VIRTIO_MIXER_CTL_TYPE_MASK_NONE               = (0),
   VIRTIO_MIXER_CTL_TYPE_MASK_BOOLEAN            = (1),
   VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER            = (2),
   VIRTIO_MIXER_CTL_TYPE_MASK_ENUMERATED         = (3),
   VIRTIO_MIXER_CTL_TYPE_MASK_BYTES              = (4),
   VIRTIO_MIXER_CTL_TYPE_MASK_IEC958             = (5),
   VIRTIO_MIXER_CTL_TYPE_MASK_INTEGER64          = (6),
   VIRTIO_MIXER_CTL_ACCESS_MASK_READ             = (1<<0),
   VIRTIO_MIXER_CTL_ACCESS_MASK_WRITE            = (1<<1),
   VIRTIO_MIXER_CTL_ACCESS_MASK_READWRITE        = (VIRTIO_MIXER_CTL_ACCESS_MASK_READ | VIRTIO_MIXER_CTL_ACCESS_MASK_WRITE),
   VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_READ         = (1<<4),
   VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_WRITE        = (1<<5),
   VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_COMMAND      = (1<<6),
   VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_READWRITE    = (VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_READ | VIRTIO_MIXER_CTL_ACCESS_MASK_TLV_WRITE),
} virtio_mixer_ctl_mask_t;

typedef enum _virtio_ctl_elem_id_offset {
   VIRTIO_CTL_ELEM_ID_NUMID_OFFSET                      = 0,                                                    // 0
   VIRTIO_CTL_ELEM_ID_IFACE_OFFSET                      = VIRTIO_CTL_ELEM_ID_NUMID_OFFSET + 4,                  // 4
   VIRTIO_CTL_ELEM_ID_DEVICE_OFFSET                     = VIRTIO_CTL_ELEM_ID_IFACE_OFFSET + 4,                  // 8
   VIRTIO_CTL_ELEM_ID_SUBDEVICE_OFFSET                  = VIRTIO_CTL_ELEM_ID_DEVICE_OFFSET + 4,                 // 12
   VIRTIO_CTL_ELEM_ID_NAME_OFFSET                       = VIRTIO_CTL_ELEM_ID_SUBDEVICE_OFFSET + 4,              // 16
   VIRTIO_CTL_ELEM_ID_IDX_OFFSET                        = VIRTIO_CTL_ELEM_ID_NAME_OFFSET + 44,                  // 60
   VIRTIO_CTL_ELEM_ID_TOTAL_SIZE                        = VIRTIO_CTL_ELEM_ID_IDX_OFFSET + 4,                    //64
} virtio_ctl_elem_id_offset_t;

typedef enum _virtio_aes_iec958_offset {
   VIRTIO_AES_STATUS_OFFSET                          = 0,                                       // 0
   VIRTIO_AES_SUBCODE_OFFSET                         = VIRTIO_AES_STATUS_OFFSET + 24,                  // 24
   VIRTIO_AES_PAD_OFFSET                             = VIRTIO_AES_SUBCODE_OFFSET + 147,                // 171
   VIRTIO_AES_DIAG_SUBFRAME_OFFSET                   = VIRTIO_AES_PAD_OFFSET + 1,                      // 172
};

typedef enum _virtio_ctl_elem_info_offset {
   VIRTIO_CTL_ELEM_INFO_ID_OFFSET                              = 0,
   VIRTIO_CTL_ELEM_INFO_TYPE_OFFSET                            = VIRTIO_CTL_ELEM_ID_TOTAL_SIZE,                               // 64
   VIRTIO_CTL_ELEM_INFO_ACCESS_OFFSET                          = VIRTIO_CTL_ELEM_INFO_TYPE_OFFSET + 4,                        // 68
   VIRTIO_CTL_ELEM_INFO_COUNT_OFFSET                           = VIRTIO_CTL_ELEM_INFO_ACCESS_OFFSET + 4,                      // 72
   VIRTIO_CTL_ELEM_INFO_OWNER_OFFSET                           = VIRTIO_CTL_ELEM_INFO_COUNT_OFFSET + 4,                       // 76
   VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER_MIN_OFFSET               = VIRTIO_CTL_ELEM_INFO_OWNER_OFFSET + 4,                       // 80
   VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER_MAX_OFFSET               = VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER_MIN_OFFSET + 8,           // 88
   VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER_STEP_OFFSET              = VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER_MAX_OFFSET + 8,           // 96
   VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER64_MIN_OFFSET             = VIRTIO_CTL_ELEM_INFO_OWNER_OFFSET + 4,                       // 80
   VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER64_MAX_OFFSET             = VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER64_MIN_OFFSET + 8,         // 88
   VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER64_STEP_OFFSET            = VIRTIO_CTL_ELEM_INFO_VALUE_INTEGER64_MAX_OFFSET + 8,         // 96
   VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_ITEMS_OFFSET          = VIRTIO_CTL_ELEM_INFO_OWNER_OFFSET + 4,                       // 80
   VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_ITEM_OFFSET           = VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_ITEMS_OFFSET + 4,      // 84
   VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_NAME_OFFSET           = VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_ITEM_OFFSET + 4,       // 88
   VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_NAME_PTR_OFFSET       = VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_NAME_OFFSET + 64,      // 152
   VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_LENGTH_OFFSET         = VIRTIO_CTL_ELEM_INFO_VALUE_ENUMERATED_NAME_PTR_OFFSET + 8,   // 160
   VIRTIO_CTL_ELEM_INFO_VALUE_RESERVED_OFFSET                  = VIRTIO_CTL_ELEM_INFO_OWNER_OFFSET + 4,                       // 80
   VIRTIO_CTL_ELEM_INFO_RESERVED_OFFSET                        = VIRTIO_CTL_ELEM_INFO_VALUE_RESERVED_OFFSET + 128,            // 208
   VIRTIO_CTL_ELEM_INFO_TOTAL_SIZE                             = VIRTIO_CTL_ELEM_INFO_RESERVED_OFFSET + 64,                   // 272
} virtio_ctl_elem_info_offset_t;

typedef enum _virtio_ctl_elem_value_offset {
   VIRTIO_CTL_ELEM_VALUE_ID_OFFSET                             = 0,
   VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET                  = VIRTIO_CTL_ELEM_VALUE_ID_OFFSET  + 72,              // 72
   VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_PTR_OFFSET              = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET,         // 72
   VIRTIO_CTL_ELEM_VALUE_INTEGER64_VALUE_OFFSET                = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET,         // 72
   VIRTIO_CTL_ELEM_VALUE_INTEGER64_VALUE_PTR_OFFSET            = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET,         // 72
   VIRTIO_CTL_ELEM_VALUE_ENUMERATED_ITEM_OFFSET                = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET,         // 72
   VIRTIO_CTL_ELEM_VALUE_ENUMERATED_ITEM_PTR_OFFSET            = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET,         // 72
   VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_OFFSET                     = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET,         // 72
   VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_PTR_OFFSET                 = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET,         // 72
   VIRTIO_CTL_ELEM_VALUE_IEC958_OFFSET                         = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET,         // 72
   VIRTIO_CTL_ELEM_VALUE_RESERVED_OFFSET                       = VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_OFFSET + 1024,  // 1096
   VIRTIO_CTL_ELEM_VALUE_TOTAL_SIZE                            = VIRTIO_CTL_ELEM_VALUE_RESERVED_OFFSET + 128,        // 1224
} virtio_ctl_elem_value_offset_t;

typedef enum _virtio_ctl_elem_size {
   VIRTIO_CTL_ELEM_VALUE_INTEGER_VALUE_SIZE = 1024,
   VIRTIO_CTL_ELEM_VALUE_INTEGER64_VALUE_SIZE = 512,
   VIRTIO_CTL_ELEM_VALUE_ENUMERATED_ITEM_SIZE = 512,
   VIRTIO_CTL_ELEM_VALUE_BYTES_DATA_SIZE = 512,
} virtio_ctl_elem_size_t;

#endif //__VIRTIO_SND_QNX_H__
