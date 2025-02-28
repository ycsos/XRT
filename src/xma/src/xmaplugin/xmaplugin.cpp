/*
 * Copyright (C) 2018, Xilinx Inc - All rights reserved
 * Xilinx SDAccel Media Accelerator API
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
#include "xmaplugin.h"
#include "xrt.h"
#include "ert.h"
#include "lib/xmahw_lib.h"
#include "lib/xmaapi.h"
#include "app/xma_utils.hpp"
#include "lib/xma_utils.hpp"
#include "core/common/api/bo.h"
#include "core/common/device.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>


using namespace std;

static_assert(sizeof(XmaCmdState) <= sizeof(int32_t), "XmaCmdState size must be <= sizeof int32_t");
#define XMAPLUGIN_MOD "xmapluginlib"

extern XmaSingleton *g_xma_singleton;

int32_t create_bo(xclDeviceHandle dev_handle, XmaBufferObj& b_obj, uint32_t size, uint32_t ddr_bank, 
    bool device_only_buffer, xrt::bo& xrt_bo_obj) {
    try {
        if (device_only_buffer) {
            xrt_bo_obj = xrt::bo(dev_handle, size, xrt::bo::flags::device_only, ddr_bank);
            b_obj.device_only_buffer = true;
        }
        else {
            xrt_bo_obj = xrt::bo(dev_handle, size, ddr_bank);
        }
        b_obj.paddr = xrt_bo_obj.address();
        if (!device_only_buffer) {
            b_obj.data = xrt_bo_obj.map<uint8_t*>();
            std::fill(b_obj.data, b_obj.data + size, 0);
        }
        return XMA_SUCCESS;
    }
    catch (const xrt_core::system_error&) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc failed to get BO properties");
        return XMA_ERROR;
    }
}

// Initialize cmd obj with default values
void cmd_obj_default(XmaCUCmdObj& cmd_obj) {
    cmd_obj.cmd_id1 = 0;
    cmd_obj.cmd_id2 = 0;
    cmd_obj.cmd_finished = false;
    cmd_obj.cu_index = -1;
    cmd_obj.do_not_use1 = nullptr;
}

XmaBufferObj
create_error_bo()
{
    XmaBufferObj b_obj_error;
    b_obj_error.data = nullptr;
    b_obj_error.size = 0;
    b_obj_error.paddr = 0;
    b_obj_error.bank_index = -1;
    b_obj_error.dev_index = -1;
    b_obj_error.user_ptr = nullptr;
    b_obj_error.device_only_buffer = false;
    b_obj_error.private_do_not_touch = nullptr;
    return b_obj_error;
}

XmaBufferObj  create_xma_buffer_object(XmaSession s_handle, size_t size, bool device_only_buffer, uint32_t ddr_bank ,int32_t* return_code) {
    XmaBufferObj b_obj;
    XmaBufferObj b_obj_error = create_error_bo(); 
    b_obj.data = nullptr;
    b_obj.user_ptr = nullptr;
    b_obj.device_only_buffer = false;
    b_obj.private_do_not_touch = nullptr;

    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc_ddr failed. XMASession is corrupted.");
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    auto dev_handle = priv1->dev_handle;
    
    b_obj.bank_index = ddr_bank;
    b_obj.size = size;
    b_obj.dev_index = s_handle.hw_session.dev_index;

    if (s_handle.session_type >= XMA_ADMIN) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma plugin buffer allocation can not be used for this XMASession type");
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }
    
    xrt::bo b_obj_handle;
    if (create_bo(dev_handle, b_obj, size, ddr_bank, device_only_buffer, b_obj_handle) != XMA_SUCCESS) {
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }

    XmaBufferObjPrivate* tmp1 = new XmaBufferObjPrivate;
    b_obj.private_do_not_touch = tmp1;
    tmp1->dummy = (void*)(((uint64_t)tmp1) | signature);
    tmp1->xrt_bo = b_obj_handle;

    if (return_code) *return_code = XMA_SUCCESS;
    return b_obj;
}

XmaBufferObj
xma_plg_buffer_alloc(XmaSession s_handle, size_t size, bool device_only_buffer, int32_t* return_code)
{
    XmaBufferObj b_obj_error = create_error_bo();
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc failed. XMASession is corrupted.");
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }

    uint32_t ddr_bank = s_handle.hw_session.bank_index;
    if (s_handle.hw_session.bank_index < 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc can not be used for this XMASession as kernel not connected to any DDR");
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }
    //Also check that libxmaapi is linked and loaded. As libxmaplugin can not be used without loading libxmaapi
    //Here it is cheap test rather than checking in other APIs
    if (!g_xma_singleton) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc: libxmaplugin can not be used without loading libxmaapi");
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }
    return create_xma_buffer_object(s_handle, size, device_only_buffer, ddr_bank, return_code);
}

XmaBufferObj
xma_plg_buffer_alloc_arg_num(XmaSession s_handle, size_t size, bool device_only_buffer, int32_t arg_num, int32_t* return_code)
{
    XmaBufferObj b_obj_error = create_error_bo();
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc_arg_num failed. XMASession is corrupted.");
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    XmaHwKernel* kernel_info = priv1->kernel_info;
    uint32_t ddr_bank = -1;
    if (arg_num < 0) {
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc_arg_num: arg_num is invalid, using default session ddr_bank.");
    }
    else {
        auto arg_to_mem_itr1 = kernel_info->CU_arg_to_mem_info.find(arg_num);
        if (arg_to_mem_itr1 == kernel_info->CU_arg_to_mem_info.end()) {
            xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc_arg_num: arg_num is not connected to any DDR bank, using default session ddr_bank.");
        }
        else {
            ddr_bank = arg_to_mem_itr1->second;            
            xma_logmsg(XMA_DEBUG_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc_arg_num: Using ddr_bank# %d connected to arg_num# %d.", ddr_bank, arg_num);
        }
    }
    return create_xma_buffer_object(s_handle, size, device_only_buffer, ddr_bank, return_code);
}

XmaBufferObj
xma_plg_buffer_alloc_ddr(XmaSession s_handle, size_t size, bool device_only_buffer, int32_t ddr_index, int32_t* return_code)
{
    XmaBufferObj b_obj_error = create_error_bo();
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc_ddr failed. XMASession is corrupted.");
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    uint32_t ddr_bank = ddr_index;
    //Use this lambda func to print ddr info
    auto print_ddrs = [&](XmaLogLevelType log_level, XmaHwDevice* device) {
        uint32_t tmp_int1 = 0;
        for (auto& ddr : device->ddrs) {
            if (ddr.in_use) {
                xma_logmsg(log_level, XMAPLUGIN_MOD, "\tMEM# %d - %s - size: %lu KB", tmp_int1, (char*)ddr.name, ddr.size_kb);
            }
            else {
                xma_logmsg(log_level, XMAPLUGIN_MOD, "\tMEM# %d - %s - size: UnUsed", tmp_int1, (char*)ddr.name);
            }
            tmp_int1++;
        }
        return;
    };

    if ((uint32_t)ddr_index >= priv1->device->ddrs.size() || ddr_index < 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc_ddr failed. Invalid DDR index.Available DDRs are:");
        print_ddrs(XMA_ERROR_LOG, priv1->device);
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }
    if (!priv1->device->ddrs[ddr_bank].in_use) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_alloc_ddr failed. This DDR is UnUsed.Available DDRs are:");
        print_ddrs(XMA_ERROR_LOG, priv1->device);
        if (return_code) *return_code = XMA_ERROR;
        return b_obj_error;
    }
    return create_xma_buffer_object(s_handle, size, device_only_buffer, ddr_bank, return_code);
}

void
xma_plg_buffer_free(XmaSession s_handle, XmaBufferObj b_obj)
{
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_free failed. XMASession is corrupted.");
        return;
    }
    if (xma_core::utils::xma_check_device_buffer(&b_obj) != XMA_SUCCESS) {
        return;
    }
    auto b_obj_priv = reinterpret_cast<XmaBufferObjPrivate*>(b_obj.private_do_not_touch);
    b_obj_priv->dummy = nullptr;
    delete b_obj_priv;
}

int32_t
xma_plg_buffer_write(XmaSession s_handle,
                     XmaBufferObj  b_obj,
                     size_t           size,
                     size_t           offset)
{
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_write failed. XMASession is corrupted.");
        return XMA_ERROR;
    }
    if (xma_core::utils::xma_check_device_buffer(&b_obj) != XMA_SUCCESS) {
        return XMA_ERROR;
    }
    auto b_obj_priv = reinterpret_cast<XmaBufferObjPrivate*>(b_obj.private_do_not_touch);

    if (xrt_core::bo::get_flags(b_obj_priv->xrt_bo) == xrt::bo::flags::device_only) {
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_write skipped as it is device only buffer.");
        return XMA_SUCCESS;
    }
    if (size + offset > b_obj_priv->xrt_bo.size()) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_write failed. Can not write past end of buffer.");
        return XMA_ERROR;
    }
    if (size == 0) {
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_write skipped. size is zero. Nothing to write.");
        return XMA_SUCCESS;
    }

    try {
        b_obj_priv->xrt_bo.sync(XCL_BO_SYNC_BO_TO_DEVICE, size, offset);
        return XMA_SUCCESS;
    }
    catch (const xrt_core::system_error&) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_write failed. xclSyncBO failed. Check device logs for more info.");
        return XMA_ERROR;
    }    
}

int32_t
xma_plg_buffer_read(XmaSession s_handle,
                    XmaBufferObj  b_obj,
                    size_t           size,
                    size_t           offset)
{
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_read failed. XMASession is corrupted.");
        return XMA_ERROR;
    }
    if (xma_core::utils::xma_check_device_buffer(&b_obj) != XMA_SUCCESS) {
        return XMA_ERROR;
    }
    auto b_obj_priv = reinterpret_cast<XmaBufferObjPrivate*>(b_obj.private_do_not_touch);
    if (xrt_core::bo::get_flags(b_obj_priv->xrt_bo) == xrt::bo::flags::device_only) {
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_read skipped as it is device only buffer.");
        return XMA_SUCCESS;
    }
    if (size + offset > b_obj_priv->xrt_bo.size()) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_read failed. Can not read past end of buffer.");
        return XMA_ERROR;
    }
    if (size == 0) {
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_read skipped. size is zero. Nothing to read.");
        return XMA_SUCCESS;
    }

    try {
        b_obj_priv->xrt_bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE, size, offset);
        return XMA_SUCCESS;
    }
    catch (const xrt_core::system_error&) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_buffer_read failed. xclSyncBO failed. Check device logs for more info.");
        return XMA_ERROR;
    } 
}

int32_t xma_plg_execbo_avail_get(XmaSession s_handle)
{
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    //std::cout << "Debug - " << __func__ << "; " << __LINE__ << std::endl;    
    int32_t num_execbo = priv1->num_execbo_allocated;
    if (priv1->execbo_lru.size() == 0) {
        int32_t i;
        for (i = 0; i < num_execbo; i++) {
            XmaHwExecBO* execbo_tmp1 = &priv1->kernel_execbos[i];
            if (!execbo_tmp1->in_use) {
                priv1->execbo_lru.emplace_back(i);
            }
        }
    }
    if (priv1->execbo_lru.size() != 0) {
        uint32_t val = priv1->execbo_lru.back();
        priv1->execbo_lru.pop_back();
        XmaHwExecBO* execbo_tmp1 = &priv1->kernel_execbos[val];
        execbo_tmp1->in_use = true;
        priv1->execbo_to_check.emplace_back(val);
        return val;
    }
    return -1;
}

int32_t xma_plg_execbo_avail_get2(XmaSession s_handle)
{
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    //std::cout << "Debug - " << __func__ << "; " << __LINE__ << std::endl;
    int32_t num_execbo = priv1->num_execbo_allocated;
    int32_t i; 
    int32_t rc = -1;
    bool    found = false;
    //NOTE: execbo lock must be already acquired

    for (i = 0; i < num_execbo; i++) {
        XmaHwExecBO* execbo_tmp1 = &priv1->kernel_execbos[i];
        if (!execbo_tmp1->in_use) {
            found = true;
        }
        if (found) {
            execbo_tmp1->in_use = true;
            rc = i;
            break;
        }
    }
    //std::cout << "Sarab: Debug - " << __func__ << "; " << __LINE__ << std::endl;

    return rc;
}

XmaCUCmdObj xma_plg_schedule_work_item(XmaSession s_handle,
                                 void            *regmap,
                                 int32_t         regmap_size,
                                 int32_t*   return_code)
{
    XmaCUCmdObj cmd_obj_error;
    cmd_obj_default(cmd_obj_error);

    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_schedule_work_item failed. XMASession is corrupted.");
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    if (s_handle.session_type >= XMA_ADMIN) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_schedule_work_item can not be used for this XMASession type");
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }

    XmaHwKernel* kernel_tmp1 = priv1->kernel_info;
    XmaHwDevice *dev_tmp1 = priv1->device;
    if (dev_tmp1 == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private pointer is nullptr");
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    if (regmap == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "regmap is NULL");
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    if (regmap_size <= 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. regmap_size of %d is invalid", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str(), regmap_size);
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    //Kernel regmap 4KB in xmahw.h; execBO size is 4096 = 4KB in xmahw_hal.cpp; But ERT uses some space for ert pkt so allow max of 4032 Bytes for regmap
    if (regmap_size > MAX_KERNEL_REGMAP_SIZE) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. Max kernel regmap size is %d Bytes", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str(), MAX_KERNEL_REGMAP_SIZE);
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    if ((uint32_t)regmap_size != ((uint32_t)regmap_size & 0xFFFFFFFC)) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. regmap_size of %d is not a multiple of four bytes", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str(), regmap_size);
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    if (kernel_tmp1->regmap_size > 0) {
        if (regmap_size > kernel_tmp1->regmap_size) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. Can not exceed kernel register_map size. Kernel regamp_size: %d, trying to use size: %d", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str(), kernel_tmp1->regmap_size, regmap_size);
            /*Sarab TODO
            if (return_code) *return_code = XMA_ERROR;
            return cmd_obj_error;
            */
        }
    }

    uint8_t *src = (uint8_t*)regmap;
    
    bool expected = false;
    bool desired = true;
    int32_t bo_idx = -1;
    //With KDS2.0 ensure no outstanding command
    while (priv1->num_cu_cmds != 0 && !g_xma_singleton->kds_old) {
        std::unique_lock<std::mutex> lk(priv1->m_mutex);
        priv1->kernel_done_or_free.wait_for(lk, std::chrono::milliseconds(1));
    }

    // Find an available execBO buffer
    uint32_t itr = 0;
    while (true) {
        expected = false;
        while (!priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
            std::this_thread::yield();
            expected = false;
        }

        if (g_xma_singleton->cpu_mode == XMA_CPU_MODE2) {
            bo_idx = xma_plg_execbo_avail_get2(s_handle);
        } else {
            bo_idx = xma_plg_execbo_avail_get(s_handle);
        }
        if (bo_idx != -1) {
            break;
        }
        xma_logmsg(XMA_DEBUG_LOG, XMAPLUGIN_MOD, "No available execbo found");
        priv1->execbo_locked = false;
        if (itr > 15) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Unable to find free execbo to use\n");
            if (return_code) *return_code = XMA_ERROR;
            return cmd_obj_error;
        }
        std::unique_lock<std::mutex> lk(priv1->m_mutex);
        priv1->execbo_is_free.wait(lk);
        lk.unlock();
        itr++;
    }
            
    priv1->kernel_execbos[bo_idx].xrt_run =  xrt::run(priv1->kernel_execbos[bo_idx].xrt_kernel);
    auto cu_cmd = reinterpret_cast<ert_start_kernel_cmd*>(priv1->kernel_execbos[bo_idx].xrt_run.get_ert_packet());
    // Copy reg_map into execBO buffer 
    memcpy(&cu_cmd->data + cu_cmd->extra_cu_masks, src, regmap_size);

    //With KDS2.0 ensure no outstanding command
    if (priv1->num_cu_cmds != 0 && !g_xma_singleton->kds_old) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. Unexpected error. Outstanding cmd found.", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
        priv1->execbo_locked = false;
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }

    if (priv1->num_cu_cmds != 0) {
//#ifdef __GNUC__
//# pragma GCC diagnostic push
//# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
//#endif
//	/* This is no longer supported by new KDS implementation. */
//        if (xclExecBufWithWaitList(priv1->dev_handle.get_handle()->get_device_handle(),
//                        priv1->kernel_execbos[bo_idx].handle, 1, &priv1->last_execbo_handle) != 0) {
//#ifdef __GNUC__
//# pragma GCC diagnostic pop
//#endif
//            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
//                        "Failed to submit kernel start with xclExecBuf");
//            priv1->execbo_locked = false;
//            if (return_code) *return_code = XMA_ERROR;
//            return cmd_obj_error;
//        }
    } else {
        try {
            priv1->kernel_execbos[bo_idx].xrt_run.start();//start Kernel
        }
        catch (const xrt_core::system_error&) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "Failed to submit kernel start with xclExecBuf");
            priv1->execbo_locked = false;
            if (return_code) *return_code = XMA_ERROR;
            return cmd_obj_error;
        }
    }

    XmaCUCmdObj cmd_obj;
    cmd_obj_default(cmd_obj);
    cmd_obj.cu_index = kernel_tmp1->cu_index;
    cmd_obj.do_not_use1 = s_handle.session_signature;

    bool found = false;
    while(!found) {
        dev_tmp1->cu_cmd_id1++;
        uint32_t tmp_int1 = dev_tmp1->cu_cmd_id1;
        if (tmp_int1 == 0) {
            tmp_int1 = 1;
            dev_tmp1->cu_cmd_id1 = tmp_int1;
            //Change seed of random_generator
            std::random_device rd;
            uint32_t tmp_int = time(0);
            std::seed_seq seed_seq{rd(), tmp_int};
            dev_tmp1->mt_gen = std::mt19937(seed_seq);
            dev_tmp1->cu_cmd_id2 = dev_tmp1->rnd_dis(dev_tmp1->mt_gen);
        } else {
            dev_tmp1->cu_cmd_id2++;
        }
        auto itr_tmp1 = priv1->CU_cmds.emplace(tmp_int1, XmaCUCmdObjPrivate{});
        if (itr_tmp1.second) {//It is newly inserted item;
            priv1->num_cu_cmds++;
            found = true;
            cmd_obj.cmd_id1 = tmp_int1;
            cmd_obj.cmd_id2 = dev_tmp1->cu_cmd_id2;
            itr_tmp1.first->second.cmd_id2 = cmd_obj.cmd_id2;
            itr_tmp1.first->second.cu_id = cmd_obj.cu_index;
            itr_tmp1.first->second.execbo_id = bo_idx;

            priv1->kernel_execbos[bo_idx].cu_cmd_id1 = tmp_int1;
            priv1->kernel_execbos[bo_idx].cu_cmd_id2 = cmd_obj.cmd_id2;
        }
    }

    //xma_logmsg(XMA_DEBUG_LOG, XMAPLUGIN_MOD, "2. Num of cmds in-progress = %lu", priv1->CU_cmds.size());
    //Release execbo lock only after the command is fully populated and inserted in the command list
    priv1->execbo_locked = false;
    if (return_code) *return_code = XMA_SUCCESS;
    return cmd_obj;
}

XmaCUCmdObj xma_plg_schedule_cu_cmd(XmaSession s_handle,
                                 void       *regmap,
                                 int32_t    regmap_size,
                                 int32_t    cu_index,
                                 int32_t*   return_code)
{
    XmaCUCmdObj cmd_obj_error;
    cmd_obj_default(cmd_obj_error);

    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_schedule_cu_cmd failed. XMASession is corrupted.");
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    XmaHwDevice *dev_tmp1 = priv1->device;
    if (dev_tmp1 == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private pointer is NULL");
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    XmaHwKernel* kernel_tmp1 = priv1->kernel_info;
    if (s_handle.session_type < XMA_ADMIN) {
        xma_logmsg(XMA_INFO_LOG, XMAPLUGIN_MOD, "xma_plg_schedule_cu_cmd: cu_index ignored for this session type");
    } else {
        //Get the kernel_info
        if (cu_index < 0 || (uint32_t)cu_index > priv1->device->kernels.size()) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. xma_plg_schedule_cu_cmd failed. Invalud cu_index.", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
            if (return_code) *return_code = XMA_ERROR;
            return cmd_obj_error;
        }
        kernel_tmp1 = &priv1->device->kernels[cu_index];
    
        if (!kernel_tmp1->soft_kernel && !kernel_tmp1->in_use && !kernel_tmp1->context_opened) {
	    //Obtain lock only for a) singleton changes & b) kernel_info changes
            std::unique_lock<std::mutex> guard1(g_xma_singleton->m_mutex);
            //Singleton lock acquired
            try {
                dev_tmp1->xrt_device.get_handle()->open_context(dev_tmp1->uuid, kernel_tmp1->cu_index_ert, true);
            }
            catch (const xrt_core::system_error&) {
                xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Failed to open context to CU %s for this session", kernel_tmp1->name);
                if (return_code) *return_code = XMA_ERROR;
                return cmd_obj_error;
            }
            kernel_tmp1->in_use = true;
        }
        xma_logmsg(XMA_DEBUG_LOG, XMAPLUGIN_MOD, "xma_plg_schedule_cu_cmd: Using admin session with CU %s", kernel_tmp1->name);
    }

    if (regmap == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "regmap is nullptr");
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    if (regmap_size <= 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. regmap_size of %d is invalid", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str(), regmap_size);
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    //Kernel regmap 4KB in xmahw.h; execBO size is 4096 = 4KB in xmahw_hal.cpp; But ERT uses some space for ert pkt so allow max of 4032 Bytes for regmap
    if (regmap_size > MAX_KERNEL_REGMAP_SIZE) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. Max kernel regmap size is %d Bytes", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str(), MAX_KERNEL_REGMAP_SIZE);
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    if ((uint32_t)regmap_size != ((uint32_t)regmap_size & 0xFFFFFFFC)) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. regmap_size of %d is not a multiple of four bytes", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str(), regmap_size);
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }
    if (kernel_tmp1->regmap_size > 0) {
        if (regmap_size > kernel_tmp1->regmap_size) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. Can not exceed kernel register_map size. Kernel regamp_size: %d, trying to use size: %d", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str(), kernel_tmp1->regmap_size, regmap_size);
            /*Sarab TODO
            if (return_code) *return_code = XMA_ERROR;
            return cmd_obj_error;
            */
        }
    }

    uint8_t *src = (uint8_t*)regmap;
    
    bool expected = false;
    bool desired = true;
    int32_t bo_idx = -1;
    //With KDS2.0 ensure no outstanding command
    while (priv1->num_cu_cmds != 0 && !g_xma_singleton->kds_old) {
        std::unique_lock<std::mutex> lk(priv1->m_mutex);
        priv1->kernel_done_or_free.wait_for(lk, std::chrono::milliseconds(1));
    }

    // Find an available execBO buffer
    uint32_t itr = 0;
    while (true) {
        expected = false;
        while (!priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
            std::this_thread::yield();
            expected = false;
        }

        if (g_xma_singleton->cpu_mode == XMA_CPU_MODE2) {
            bo_idx = xma_plg_execbo_avail_get2(s_handle);
        } else {
            bo_idx = xma_plg_execbo_avail_get(s_handle);
        }
        if (bo_idx != -1) {
            break;
        }
        priv1->execbo_locked = false;
        xma_logmsg(XMA_DEBUG_LOG, XMAPLUGIN_MOD, "No available execbo found");
        if (itr > 15) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Unable to find free execbo to use\n");
            if (return_code) *return_code = XMA_ERROR;
            return cmd_obj_error;
        }
        std::unique_lock<std::mutex> lk(priv1->m_mutex);
        priv1->execbo_is_free.wait(lk);
        lk.unlock();
        itr++;
    }

    priv1->kernel_execbos[bo_idx].xrt_run = xrt::run(priv1->kernel_execbos[bo_idx].xrt_kernel);
    auto cu_cmd = reinterpret_cast<ert_start_kernel_cmd*>(priv1->kernel_execbos[bo_idx].xrt_run.get_ert_packet());
    // Copy reg_map into execBO buffer 
    memcpy(&cu_cmd->data + cu_cmd->extra_cu_masks, src, regmap_size);

    //With KDS2.0 ensure no outstanding command
    if (priv1->num_cu_cmds != 0 && !g_xma_singleton->kds_old) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. Unexpected error. Outstanding cmd found.", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
        priv1->execbo_locked = false;
        if (return_code) *return_code = XMA_ERROR;
        return cmd_obj_error;
    }

    if (priv1->num_cu_cmds != 0) {
//#ifdef __GNUC__
//# pragma GCC diagnostic push
//# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
//#endif
//	/* This is no longer supported by new KDS implementation. */
//        if (xclExecBufWithWaitList(priv1->dev_handle.get_handle()->get_device_handle(),
//                        priv1->kernel_execbos[bo_idx].handle, 1, &priv1->last_execbo_handle) != 0) {
//#ifdef __GNUC__
//# pragma GCC diagnostic pop
//#endif
//            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
//                        "Failed to submit kernel start with xclExecBuf");
//            priv1->execbo_locked = false;
//            if (return_code) *return_code = XMA_ERROR;
//            return cmd_obj_error;
//        }
    } else {
        try {
            priv1->kernel_execbos[bo_idx].xrt_run.start();//start Kernel
        }
        catch (const xrt_core::system_error&) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "Failed to submit kernel start with xclExecBuf");
            priv1->execbo_locked = false;
            if (return_code) *return_code = XMA_ERROR;
            return cmd_obj_error;
        }
    }

    XmaCUCmdObj cmd_obj;
    cmd_obj_default(cmd_obj);
    cmd_obj.cu_index = kernel_tmp1->cu_index;
    cmd_obj.do_not_use1 = s_handle.session_signature;

    bool found = false;
    while(!found) {
        dev_tmp1->cu_cmd_id1++;
        uint32_t tmp_int1 = dev_tmp1->cu_cmd_id1;
        if (tmp_int1 == 0) {
            tmp_int1 = 1;
            dev_tmp1->cu_cmd_id1 = tmp_int1;
            //Change seed of random_generator
            std::random_device rd;
            uint32_t tmp_int = time(0);
            std::seed_seq seed_seq{rd(), tmp_int};
            dev_tmp1->mt_gen = std::mt19937(seed_seq);
            dev_tmp1->cu_cmd_id2 = dev_tmp1->rnd_dis(dev_tmp1->mt_gen);
        } else {
            dev_tmp1->cu_cmd_id2++;
        }
        auto itr_tmp1 = priv1->CU_cmds.emplace(tmp_int1, XmaCUCmdObjPrivate{});
        if (itr_tmp1.second) {//It is newly inserted item;
            priv1->num_cu_cmds++;
            found = true;
            cmd_obj.cmd_id1 = tmp_int1;
            cmd_obj.cmd_id2 = dev_tmp1->cu_cmd_id2;
            itr_tmp1.first->second.cmd_id2 = cmd_obj.cmd_id2;
            itr_tmp1.first->second.cu_id = cmd_obj.cu_index;
            itr_tmp1.first->second.execbo_id = bo_idx;

            priv1->kernel_execbos[bo_idx].cu_cmd_id1 = tmp_int1;
            priv1->kernel_execbos[bo_idx].cu_cmd_id2 = cmd_obj.cmd_id2;
        }
    }

    //xma_logmsg(XMA_DEBUG_LOG, XMAPLUGIN_MOD, "2. Num of cmds in-progress = %lu", priv1->CU_cmds.size());
    //Release execbo lock only after the command is fully populated and inserted in the command list
    priv1->execbo_locked = false;
    if (return_code) *return_code = XMA_SUCCESS;
    return cmd_obj;
}

int32_t xma_plg_cu_cmd_status(XmaSession s_handle, XmaCUCmdObj* cmd_obj_array, int32_t num_cu_objs, bool wait_for_cu_cmds)
{
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_cu_cmd_status failed. XMASession is corrupted.");
        return XMA_ERROR;
    }
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);

    XmaHwKernel* kernel_tmp1 = priv1->kernel_info;
    XmaHwDevice *dev_tmp1 = priv1->device;
    if (dev_tmp1 == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private pointer is NULL-1");
        return XMA_ERROR;
    }
    if (s_handle.session_type != XMA_ADMIN && kernel_tmp1 == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private pointer is NULL-2");
        return XMA_ERROR;
    }
    if (priv1->using_work_item_done) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. xma_plg_cu_cmd_status & xma_plg_is_work_item_done both can not be used in same session", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
        return XMA_ERROR;
    }
    priv1->using_cu_cmd_status = true;

    int32_t num_execbo = priv1->num_execbo_allocated;
    if (num_execbo <= 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private: No execbo allocated");
        return XMA_ERROR;
    }
    if (cmd_obj_array == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "cmd_obj_array is NULL");
        return XMA_ERROR;
    }
    if (num_cu_objs <= 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "num_cu_objs of %d is invalid", num_cu_objs);
        return XMA_ERROR;
    }

    bool expected = false;
    bool desired = true;
    bool all_done = true;
    while (!priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
        std::this_thread::yield();
        expected = false;
    }

    if (xma_core::utils::check_all_execbo(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "work_item_done->check_all_execbo. Unexpected error");
        //Release execbo lock
        priv1->execbo_locked = false;
        return XMA_ERROR;
    }
    //Release execbo lock
    priv1->execbo_locked = false;

    std::vector<XmaCUCmdObj> cmd_vector(cmd_obj_array, cmd_obj_array+num_cu_objs);
    do {
        all_done = true;
        for (auto& cmd: cmd_vector) {
            if (s_handle.session_type < XMA_ADMIN && cmd.cu_index != kernel_tmp1->cu_index) {
                xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "cmd_obj_array is corrupted-1");
                return XMA_ERROR;
            }
            if (cmd.cmd_id1 == 0 || cmd.cu_index == -1) {
                xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "cmd_obj is invalid. Schedule_command may have  failed");
                return XMA_ERROR;
            }
            auto itr_tmp1 = priv1->CU_cmds.find(cmd.cmd_id1);
            if (itr_tmp1 == priv1->CU_cmds.end()) {
                cmd.cmd_finished = true;
            } else {
                all_done = false;
            }

            if (cmd.do_not_use1 != s_handle.session_signature) {
                xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "cmd_obj_array is corrupted-5");
                return XMA_ERROR;
            }
        }

        if (!wait_for_cu_cmds) {
            //Don't wait for all cu_cmds to finsh
            all_done = true;
        } else if (!all_done) {
            if (g_xma_singleton->cpu_mode == XMA_CPU_MODE1) {
                std::unique_lock<std::mutex> lk(priv1->m_mutex);
                priv1->kernel_done_or_free.wait(lk);
            } else if (g_xma_singleton->cpu_mode == XMA_CPU_MODE2) {
                std::this_thread::yield();
            } else {
                priv1->dev_handle.get_handle()->exec_wait(100); // Created CR-1120629 to handle this, supposed to use xrt::run::wait() call.
            }
        }
    } while(!all_done);

    for(int32_t i = 0; i < num_cu_objs; i++) {
        cmd_obj_array[i].cmd_finished = cmd_vector[i].cmd_finished;
    }

    return XMA_SUCCESS;
}

int32_t xma_plg_is_work_item_done(XmaSession s_handle, uint32_t timeout_ms)
{
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_is_work_item_done failed. XMASession is corrupted.");
        return XMA_ERROR;
    }
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    if (s_handle.session_type >= XMA_ADMIN) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_is_work_item_done can not be used for this XMASession type");
        return XMA_ERROR;
    }
    if (priv1->using_cu_cmd_status) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. xma_plg_is_work_item_done & xma_plg_cu_cmd_status both can not be used in same session", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
        return XMA_ERROR;
    }
    priv1->using_work_item_done = true;

    XmaHwDevice *dev_tmp1 = priv1->device;
    if (dev_tmp1 == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private pointer is NULL");
        return XMA_ERROR;
    }
    int32_t num_execbo = priv1->num_execbo_allocated;
    if (num_execbo <= 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private: No execbo allocated");
        return XMA_ERROR;
    }

    int32_t count = 0;

    count = priv1->kernel_complete_count;
    if (count) {
        priv1->kernel_complete_count--;
        if (count > 255) {
            xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "CU completion count is more than 256. Application maybe slow to process CU output");
        }
        return XMA_SUCCESS;
    }
    bool expected = false;
    bool desired = true;
/*
    while (!priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
        std::this_thread::yield();
        expected = false;
    }

    if (xma_core::utils::check_all_execbo(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "work_item_done->check_all_execbo. Unexpected error");
        //Release execbo lock
        priv1->execbo_locked = false;
        return XMA_ERROR;
    }
    //Release execbo lock
    priv1->execbo_locked = false;

    count = priv1->kernel_complete_count;
    if (count) {
        priv1->kernel_complete_count--;
        if (count > 255) {
            xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "CU completion count is more than 256. Application maybe slow to process CU output");
        }
        return XMA_SUCCESS;
    }
    if (priv1->num_cu_cmds == 0 && count == 0) {
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. There may not be any outstandng CU command to wait for\n", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
    }
*/
    uint32_t iter1 = timeout_ms / 10;
    if (iter1 < 10) {
        iter1 = 10;
    }
    uint32_t timeout1 = 10;
    uint32_t tmp_num_cmds = 1;
    if (g_xma_singleton->cpu_mode == XMA_CPU_MODE1) {
        while (iter1 > 0) {
            std::unique_lock<std::mutex> lk(priv1->m_mutex);
            //priv1->work_item_done_1plus.wait(lk);
	    //Timeout required if cu is hung; Unblock and check status again
            priv1->work_item_done_1plus.wait_for(lk, std::chrono::milliseconds(timeout1));
            lk.unlock();

            tmp_num_cmds = priv1->num_cu_cmds;
            count = priv1->kernel_complete_count;
            if (count) {
                priv1->kernel_complete_count--;
                if (count > 255) {
                    xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "CU completion count is more than 256. Application maybe slow to process CU output");
                }
                return XMA_SUCCESS;
            }
	    //Get num_cmds pending first before the done count check
            if (tmp_num_cmds == 0 && count == 0) {
                xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. There may not be any outstandng CU command to wait for\n", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
            }

            iter1--;
        }
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. CU cmd is still pending. Cu might be stuck", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
        return XMA_ERROR;
    }

    if (g_xma_singleton->cpu_mode == XMA_CPU_MODE2) {
	iter1 = iter1 * 10;
        while (iter1 > 0) {
            expected = false;
            if (priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
                //kernel completion lock acquired

                if (xma_core::utils::check_all_execbo(s_handle) != XMA_SUCCESS) {
                    xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "check_all-2: Unexpected error\n");
                    //Release execbo lock
                    priv1->execbo_locked = false;
                    return XMA_ERROR;
                }
                //Release execbo lock
                priv1->execbo_locked = false;
            }
            tmp_num_cmds = priv1->num_cu_cmds;
            count = priv1->kernel_complete_count;
            if (count) {
                priv1->kernel_complete_count--;
                if (count > 255) {
                    xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "CU completion count is more than 256. Application maybe slow to process CU output");
                }
                return XMA_SUCCESS;
            }
	    //Get num_cmds pending first before the done count check
            if (tmp_num_cmds == 0 && count == 0) {
                xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. There may not be any outstandng CU command to wait for\n", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
            }

            iter1--;
            //std::this_thread::yield();
	    //Debug mode: Use small timeout
	    std::unique_lock<std::mutex> lk(priv1->m_mutex);
            priv1->work_item_done_1plus.wait_for(lk, std::chrono::milliseconds(1));

        }
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. CU cmd is still pending. Cu might be stuck", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
        return XMA_ERROR;
    }

    if (g_xma_singleton->cpu_mode == XMA_CPU_MODE3) {
        while (iter1 > 0) {
            expected = false;
            while (!priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
                std::this_thread::yield();
                expected = false;
            }

            if (xma_core::utils::check_all_execbo(s_handle) != XMA_SUCCESS) {
                xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "work_item_done->check_all_execbo. Unexpected error");
                //Release execbo lock
                priv1->execbo_locked = false;
                return XMA_ERROR;
            }
            //Release execbo lock
            priv1->execbo_locked = false;

            tmp_num_cmds = priv1->num_cu_cmds;
            count = priv1->kernel_complete_count;
            if (count) {
                priv1->kernel_complete_count--;
                if (count > 255) {
                    xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "CU completion count is more than 256. Application maybe slow to process CU output");
                }
                return XMA_SUCCESS;
            }
	    //Get num_cmds pending first before the done count check
            if (tmp_num_cmds == 0 && count == 0) {
                xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. There may not be any outstandng CU command to wait for\n", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
            }
            priv1->dev_handle.get_handle()->exec_wait(timeout1); // Created CR-1120629 to handle this, supposed to use xrt::run::wait() call.
            iter1--;
        }
        xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. CU cmd is still pending. Cu might be stuck", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
        return XMA_ERROR;
    }

    //Below is CPU mode-4; low cpu load mode
    int32_t give_up = 0;
    if (iter1 < 20) {
        iter1 = 20;
    }
    while (give_up < (int32_t)iter1) {
        count = priv1->kernel_complete_count;
        if (count) {
            priv1->kernel_complete_count--;
            if (count > 255) {
                xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "CU completion count is more than 256. Application maybe slow to process CU output\n");
            }
            return XMA_SUCCESS;
        }

        expected = false;
        if (priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
            //kernel completion lock acquired

            if (xma_core::utils::check_all_execbo(s_handle) != XMA_SUCCESS) {
                xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "check_all-2: Unexpected error\n");
                //Release execbo lock
                priv1->execbo_locked = false;
                return XMA_ERROR;
            }
            //Release execbo lock
            priv1->execbo_locked = false;

            count = priv1->kernel_complete_count;
            if (count) {
                priv1->kernel_complete_count--;
                if (count > 255) {
                    xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "CU completion count is more than 256. Application maybe slow to process CU output\n");
                }
                return XMA_SUCCESS;
            }
        }
    
        // Wait for a notification
        if (give_up > 10) {
            priv1->dev_handle.get_handle()->exec_wait(timeout1);
            tmp_num_cmds = priv1->num_cu_cmds;
            count = priv1->kernel_complete_count;
            if (count) {
                priv1->kernel_complete_count--;
                if (count > 255) {
                    xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "CU completion count is more than 256. Application maybe slow to process CU output\n");
                }
                return XMA_SUCCESS;
            }
	    //Get num_cmds pending first before the done count check
            if (tmp_num_cmds == 0 && count == 0) {
                xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. There may not be any outstandng CU command to wait for\n", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        give_up++;
    }
    xma_logmsg(XMA_WARNING_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. CU cmd is still pending. Cu might be stuck", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
    return XMA_ERROR;
}

int32_t xma_plg_work_item_return_code(XmaSession s_handle, XmaCUCmdObj* cmd_obj_array, int32_t num_cu_objs, uint32_t* num_cu_errors)
{
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_cu_cmd_status failed. XMASession is corrupted.");
        return XMA_ERROR;
    }
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);

    XmaHwKernel* kernel_tmp1 = priv1->kernel_info;
    XmaHwDevice *dev_tmp1 = priv1->device;
    if (dev_tmp1 == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private pointer is NULL-1");
        return XMA_ERROR;
    }
    if (s_handle.session_type != XMA_ADMIN && kernel_tmp1 == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session XMA private pointer is NULL-2");
        return XMA_ERROR;
    }

    if (cmd_obj_array == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "cmd_obj_array is NULL");
        return XMA_ERROR;
    }
    if (num_cu_objs <= 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "num_cu_objs of %d is invalid", num_cu_objs);
        return XMA_ERROR;
    }

    XmaCUCmdObj* cmd_end = cmd_obj_array+num_cu_objs;
    uint32_t num_errors = 0;
    for (auto itr = cmd_obj_array; itr < cmd_end; ++itr) {
        auto& cmd = *itr;
        if (cmd.do_not_use1 != s_handle.session_signature) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "cmd_obj_array is corrupted-1");
            return XMA_ERROR;
        }
        if (s_handle.session_type < XMA_ADMIN && cmd.cu_index != kernel_tmp1->cu_index) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "cmd_obj_array is corrupted-2");
            return XMA_ERROR;
        }
        if (cmd.cmd_id1 == 0 || cmd.cu_index == -1) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "cmd_obj is invalid. Schedule_command may have  failed");
            return XMA_ERROR;
        }
        auto itr_tmp1 = priv1->CU_cmds.find(cmd.cmd_id1);
        if (itr_tmp1 != priv1->CU_cmds.end()) {
            xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "Session id: %d, type: %s. CU cmd has not finished yet. Return code must be checked only after the command has finished", s_handle.session_id, xma_core::get_session_name(s_handle.session_type).c_str());
            return XMA_ERROR;
        }
        cmd.cmd_finished = true;
        //cmd.return_code = 0; As return_code is now shared with cmd_id1
        cmd.cmd_state = static_cast<XmaCmdState>(xma_cmd_state::completed);
        cmd.do_not_use1 = nullptr;
        auto itr_tmp2 = priv1->CU_error_cmds.find(cmd.cmd_id1);
        if (itr_tmp2 != priv1->CU_error_cmds.end()) {
            num_errors++;
            cmd.return_code = itr_tmp2->second.return_code;
            cmd.cmd_state = static_cast<XmaCmdState>(itr_tmp2->second.cmd_state);
        }
    }

    if (num_cu_errors)
        *num_cu_errors = num_errors;

    return XMA_SUCCESS;
}

int32_t xma_plg_channel_id(XmaSession s_handle) {
    if (xma_core::utils::check_xma_session(s_handle) != XMA_SUCCESS) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_channel_id failed. XMASession is corrupted.");
        return XMA_ERROR;
    }
    if (s_handle.session_type >= XMA_ADMIN) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_channel_id can not be used for this XMASession type");
        return XMA_ERROR;
    }
    return s_handle.channel_id;
}

int32_t xma_plg_add_buffer_to_data_buffer(XmaDataBuffer *data, XmaBufferObj *dev_buf) {
    if (data == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "%s(): data XmaDataBuffer is NULL", __func__);
        return XMA_ERROR;
    }
    if (dev_buf == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "%s(): dev_buf XmaBufferObj is NULL", __func__);
        return XMA_ERROR;
    }
    if (xma_core::utils::xma_check_device_buffer(dev_buf) != XMA_SUCCESS) {
        return XMA_ERROR;
    }
    if (data->data.buffer_type != NO_BUFFER) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "%s(): Buffer already has assigned memory. Invalid XmaDataBuffer type", __func__);
        return XMA_ERROR;
    }
    data->data.buffer = dev_buf->data;
    data->data.xma_device_buf = dev_buf;
    if (dev_buf->device_only_buffer) {
        data->data.buffer_type = XMA_DEVICE_ONLY_BUFFER_TYPE;
    } else {
        data->data.buffer_type = XMA_DEVICE_BUFFER_TYPE;
    }
    data->alloc_size = dev_buf->size;
    data->data.is_clone = true;//so that others do not free the device buffer. Plugin owns device buffer

    return XMA_SUCCESS;
}

int32_t xma_plg_add_buffer_to_frame(XmaFrame *frame, XmaBufferObj **dev_buf_list, uint32_t num_dev_buf) {
    if (frame == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "%s(): frame XmaFrame is NULL", __func__);
        return XMA_ERROR;
    }
    if (dev_buf_list == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "%s(): dev_buf_list XmaBufferObj is NULL", __func__);
        return XMA_ERROR;
    }
    if (num_dev_buf > XMA_MAX_PLANES) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "%s(): num_dev_buf is more than max planes in frame", __func__);
        return XMA_ERROR;
    }
    if (num_dev_buf == 0) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "%s(): num_dev_buf is zero", __func__);
        return XMA_ERROR;
    }
    for (uint32_t i = 0; i < num_dev_buf; i++) {
        if (xma_core::utils::xma_check_device_buffer(dev_buf_list[i]) != XMA_SUCCESS) {
            return XMA_ERROR;
        }
    }
    if (frame->data[0].buffer_type != NO_BUFFER) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD,
                "%s(): Frame already has assigned memory. Invalid frame buffer type", __func__);
        return XMA_ERROR;
    }
    for (uint32_t i = 0; i < num_dev_buf; i++) {
        if (frame->data[i].buffer_type != NO_BUFFER) {
            break;
        }
        frame->data[i].buffer = dev_buf_list[i]->data;
        frame->data[i].xma_device_buf = dev_buf_list[i];
        if (dev_buf_list[i]->device_only_buffer) {
            frame->data[i].buffer_type = XMA_DEVICE_ONLY_BUFFER_TYPE;
        } else {
            frame->data[i].buffer_type = XMA_DEVICE_BUFFER_TYPE;
        }
        //frame->data[i].alloc_size = dev_buf_list[i].size;
        frame->data[i].is_clone = true;//so that others do not free the device buffer. Plugin owns device buffer
    }

    return XMA_SUCCESS;
}

int32_t xma_plg_add_ref_cnt(XmaBufferObj *b_obj, int32_t num) {
    xma_logmsg(XMA_DEBUG_LOG, XMAPLUGIN_MOD,
               "%s(), line# %d", __func__, __LINE__);

    if (xma_core::utils::xma_check_device_buffer(b_obj) != XMA_SUCCESS) {
        return -999;
    }
    auto b_obj_priv = reinterpret_cast<XmaBufferObjPrivate*>(b_obj->private_do_not_touch);
    b_obj_priv->ref_cnt += num;
    return b_obj_priv->ref_cnt;
}

void* xma_plg_get_dev_handle(XmaSession s_handle) {
    auto priv1 = reinterpret_cast<XmaHwSessionPrivate*>(s_handle.hw_session.private_do_not_use);
    if (priv1 == nullptr) {
        xma_logmsg(XMA_ERROR_LOG, XMAPLUGIN_MOD, "xma_plg_get_dev_handle failed. XMASession is corrupted.");
        return nullptr;
    }
    return priv1->dev_handle.get_handle()->get_device_handle();
}
