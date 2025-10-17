/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "hab_interface.h"

HabImpl::~HabImpl() {}

int32_t HabImpl::Open(int32_t *handle, uint32_t mmid,
             uint32_t timeout, uint32_t flags)
{
    return habmm_socket_open(handle, mmid, timeout, flags);
}

int32_t HabImpl::Close(int32_t handle)
{
    return habmm_socket_close(handle);
}

int32_t HabImpl::SocketQuery(int32_t handle, struct SocketInfo *sock_info,
                    uint32_t flags)
{
    struct hab_socket_info socket_info;
    int32_t ret;

    ret = habmm_socket_query(handle, &socket_info, flags);
    sock_info->vmid = socket_info.vmid_remote;
    return ret;
}

int32_t HabImpl::Send(int32_t handle, void *src_buff,
             uint32_t size_bytes, uint32_t flags)
{
    return habmm_socket_send(handle, src_buff, size_bytes, flags);
}

int32_t HabImpl::Receive(int32_t handle, void *dst_buff, uint32_t *size_bytes,
                uint32_t timeout, uint32_t flags)
{
    return habmm_socket_recv(handle, dst_buff, size_bytes, timeout, flags);
}

int32_t HabImpl::Import(int32_t handle, void **buff_shared, uint32_t size_bytes,
               uint32_t export_id, uint32_t flags)
{
    return habmm_import(handle, buff_shared, size_bytes, export_id, flags);
}

int32_t HabImpl::Export(int32_t handle, void **buff_to_share, uint32_t size_bytes,
                         uint32_t *pexport_id, uint32_t flags)
{
    return habmm_export(handle, buff_to_share, size_bytes, pexport_id, flags);
}

int32_t HabImpl::Unimport(int32_t handle, uint32_t export_id, void *buff_shared, uint32_t flags)
{
    return habmm_unimport(handle, export_id, buff_shared, flags);
}