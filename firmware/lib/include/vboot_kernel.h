/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Data structure and API definitions for a verified boot kernel image.
 * (Firmware Portion)
 */

#ifndef VBOOT_REFERENCE_VBOOT_KERNEL_H_
#define VBOOT_REFERENCE_VBOOT_KERNEL_H_

#include "cgptlib.h"
#include "gpt_misc.h"
#include "load_kernel_fw.h"
#include "vboot_api.h"

struct vb2_context;

/**
 * Attempt loading a kernel from the specified type(s) of disks.
 *
 * If successful, sets lkp.disk_handle to the disk for the kernel and returns
 * VB2_SUCCESS.
 *
 * @param ctx			Vboot context
 * @param get_info_flags	Flags to pass to VbExDiskGetInfo()
 * @return VB2_SUCCESS or the most specific VB2_ERROR_LK error.
 */
vb2_error_t VbTryLoadKernel(struct vb2_context *ctx, uint32_t get_info_flags);

/* Flags for VbUserConfirms() */
#define VB_CONFIRM_MUST_TRUST_KEYBOARD (1 << 0)
#define VB_CONFIRM_SPACE_MEANS_NO      (1 << 1)

/**
 * Ask the user to confirm something.
 *
 * We should display whatever the question is first, then call this. ESC is
 * always "no", ENTER is always "yes", and we'll specify what SPACE means. We
 * don't return until one of those keys is pressed, or until asked to shut
 * down.
 *
 * Additionally, in some situations we don't accept confirmations from an
 * untrusted keyboard (such as a USB device).  In those cases, a recovery
 * button press is needed for confirmation, instead of ENTER.
 *
 * Returns: 1=yes, 0=no, -1 = shutdown.
 */
int VbUserConfirms(struct vb2_context *ctx, uint32_t confirm_flags);

/**
 * Handle a normal boot.
 */
vb2_error_t VbBootNormal(struct vb2_context *ctx);

/**
 * Handle a developer-mode boot.
 */
vb2_error_t VbBootDeveloper(struct vb2_context *ctx);

/**
 * Handle a diagnostic-mode boot.
 */
vb2_error_t VbBootDiagnostic(struct vb2_context *ctx);

/**
 * Handle a recovery-mode boot.
 */
vb2_error_t VbBootRecovery(struct vb2_context *ctx);

/**
 * Handle a developer-mode boot using detachable menu ui
 */
vb2_error_t VbBootDeveloperMenu(struct vb2_context *ctx);

/**
 * Handle a recovery-mode boot using detachable menu ui
 */
vb2_error_t VbBootRecoveryMenu(struct vb2_context *ctx);

/**
 * Writes modified secdata spaces and nvdata.
 *
 * This is a temporary wrapper around vb2ex_commit_data, until secdata-writing
 * functions are relocated into depthcharge.
 *
 * (See chromium:972956, chromium:1006689.)
 *
 * @param ctx		Vboot context
 * @returns VB2_SUCCESS, or non-zero error code.
 */
vb2_error_t vb2_commit_data(struct vb2_context *ctx);

#endif  /* VBOOT_REFERENCE_VBOOT_KERNEL_H_ */
