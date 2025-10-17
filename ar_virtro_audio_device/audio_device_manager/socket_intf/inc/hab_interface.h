/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file hab_interface.h
 * @brief Abstract class to provide an interface for HAB communication
 */

#ifndef HAB_INTERFACE_H_
#define HAB_INTERFACE_H_

#include <stdint.h>
extern "C" {
#include "habmm.h"
}

struct SocketInfo
{
    int32_t vcid;
    int32_t mmid;
    int32_t vmid;
};

struct DMAExport {
    uint32_t export_id;
    int fd;
    int32_t vcid;
};

 class IHab {
  public:

  virtual ~IHab() {}
  virtual int32_t Open(int32_t *handle, uint32_t mmid,
                       uint32_t timeout, uint32_t flags) = 0;

  virtual int32_t Close(int32_t handle) = 0;

  virtual int32_t SocketQuery(int32_t handle, struct SocketInfo *sock_info,
                              uint32_t flags) = 0;

  virtual int32_t Send(int32_t handle, void *src_buff,
                       uint32_t size_bytes, uint32_t flags) = 0;

  virtual int32_t Receive(int32_t handle, void *dst_buff, uint32_t *size_bytes,
                          uint32_t timeout, uint32_t flags) = 0;

  virtual int32_t Import(int32_t handle, void **buff_shared, uint32_t size_bytes,
                         uint32_t export_id, uint32_t flags) = 0;

  virtual int32_t Export(int32_t handle, void **buff_to_share, uint32_t size_bytes,
                         uint32_t *pexport_id, uint32_t flags) = 0;

  virtual int32_t Unimport(int32_t handle, uint32_t export_id, void *buff_shared, uint32_t flags) = 0;
 };


 class HabImpl : public IHab {
 public:
    ~HabImpl();
    int32_t Open(int32_t *handle, uint32_t mmid,
                       uint32_t timeout, uint32_t flags) override;

    int32_t Close(int32_t handle) override;

    int32_t SocketQuery(int32_t handle, struct SocketInfo *sock_info,
                              uint32_t flags) override;

    int32_t Send(int32_t handle, void *src_buff,
                       uint32_t size_bytes, uint32_t flags) override;

    int32_t Receive(int32_t handle, void *dst_buff, uint32_t *size_bytes,
                          uint32_t timeout, uint32_t flags) override;

    int32_t Import(int32_t handle, void **buff_shared, uint32_t size_bytes,
                         uint32_t export_id, uint32_t flags) override;

    int32_t Export(int32_t handle, void **buff_to_share, uint32_t size_bytes,
                         uint32_t *pexport_id, uint32_t flags) override;

  int32_t Unimport(int32_t handle, uint32_t export_id, void *buff_shared, uint32_t flags) override;
 };

 #endif // HAB_INTERFACE_H_