/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Misc functions which need access to vb2_context but are not public APIs
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2rsa.h"
#include "2secdata.h"
#include "2sha.h"
#include "2struct.h"
#include "2sysincludes.h"
#include "vb2_common.h"
#include "vboot_api.h"
#include "vboot_struct.h"

vb2_error_t vb2_validate_gbb_signature(uint8_t *sig)
{
	const static uint8_t sig_xor[VB2_GBB_SIGNATURE_SIZE] =
			VB2_GBB_XOR_SIGNATURE;
	int i;
	for (i = 0; i < VB2_GBB_SIGNATURE_SIZE; i++) {
		if (sig[i] != (sig_xor[i] ^ VB2_GBB_XOR_CHARS[i]))
			return VB2_ERROR_GBB_MAGIC;
	}
	return VB2_SUCCESS;
}

test_mockable
struct vb2_gbb_header *vb2_get_gbb(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	return (struct vb2_gbb_header *)((void *)sd + sd->gbb_offset);
}

uint32_t vb2api_get_firmware_size(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	if (!sd->preamble_size)
		return 0;

	const struct vb2_fw_preamble *pre = (const struct vb2_fw_preamble *)
		vb2_member_of(sd, sd->preamble_offset);
	return pre->body_signature.data_size;
}

vb2_error_t vb2_read_gbb_header(struct vb2_context *ctx,
				struct vb2_gbb_header *gbb)
{
	vb2_error_t rv;

	/* Read the entire header */
	rv = vb2ex_read_resource(ctx, VB2_RES_GBB, 0, gbb, sizeof(*gbb));
	if (rv)
		return rv;

	/* Make sure it's really a GBB */
	rv = vb2_validate_gbb_signature(gbb->signature);
	if (rv)
		return rv;

	/* Check for compatible version */
	if (gbb->major_version != VB2_GBB_MAJOR_VER)
		return VB2_ERROR_GBB_VERSION;

	/* Current code is not backwards-compatible to 1.1 headers or older */
	if (gbb->minor_version < VB2_GBB_MINOR_VER)
		return VB2_ERROR_GBB_TOO_OLD;

	/*
	 * Header size should be at least as big as we expect.  It could be
	 * bigger, if the header has grown.
	 */
	if (gbb->header_size < sizeof(*gbb))
		return VB2_ERROR_GBB_HEADER_SIZE;

	return VB2_SUCCESS;
}

void vb2api_fail(struct vb2_context *ctx, uint8_t reason, uint8_t subcode)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	/* If NV data hasn't been initialized, initialize it now */
	if (!(sd->status & VB2_SD_STATUS_NV_INIT))
		vb2_nv_init(ctx);

	/* See if we were far enough in the boot process to choose a slot */
	if (sd->status & VB2_SD_STATUS_CHOSE_SLOT) {

		/* Boot failed */
		vb2_nv_set(ctx, VB2_NV_FW_RESULT, VB2_FW_RESULT_FAILURE);

		/* Use up remaining tries */
		vb2_nv_set(ctx, VB2_NV_TRY_COUNT, 0);

		/*
		 * Try the other slot next time.  We'll alternate
		 * between slots, which may help if one or both slots is
		 * flaky.
		 */
		vb2_nv_set(ctx, VB2_NV_TRY_NEXT, 1 - sd->fw_slot);

		/*
		 * If we didn't try the other slot last boot, or we tried it
		 * and it didn't fail, try it next boot.
		 */
		if (sd->last_fw_slot != 1 - sd->fw_slot ||
		    sd->last_fw_result != VB2_FW_RESULT_FAILURE)
			return;
	}

	/*
	 * If we're still here, we failed before choosing a slot, or both
	 * this slot and the other slot failed in successive boots.  So we
	 * need to go to recovery.
	 *
	 * Set a recovery reason and subcode only if they're not already set.
	 * If recovery is already requested, it's a more specific error code
	 * than later code is providing and we shouldn't overwrite it.
	 */
	VB2_DEBUG("Need recovery, reason: %#x / %#x\n", reason, subcode);
	if (!vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST)) {
		vb2_nv_set(ctx, VB2_NV_RECOVERY_REQUEST, reason);
		vb2_nv_set(ctx, VB2_NV_RECOVERY_SUBCODE, subcode);
	}
}

void vb2_check_recovery(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	uint32_t reason = vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST);
	uint32_t subcode = vb2_nv_get(ctx, VB2_NV_RECOVERY_SUBCODE);

	VB2_DEBUG("Recovery reason from previous boot: %#x / %#x\n",
		  reason, subcode);

	/*
	 * Sets the current recovery request, unless there's already been a
	 * failure earlier in the boot process.
	 */
	if (!sd->recovery_reason)
		sd->recovery_reason = reason;

	if (ctx->flags & VB2_CONTEXT_FORCE_RECOVERY_MODE) {
		VB2_DEBUG("Recovery was requested manually\n");
		if (subcode && !sd->recovery_reason)
			/*
			 * Recovery was requested at 'broken' screen.
			 * Promote subcode to reason.
			 */
			sd->recovery_reason = subcode;
		else
			/* Recovery was forced. Override recovery reason */
			sd->recovery_reason = VB2_RECOVERY_RO_MANUAL;
		sd->flags |= VB2_SD_FLAG_MANUAL_RECOVERY;
	}

	/* If recovery reason is non-zero, tell caller we need recovery mode */
	if (sd->recovery_reason) {
		ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
		VB2_DEBUG("We have a recovery request: %#x / %#x\n",
			  sd->recovery_reason,
			  vb2_nv_get(ctx, VB2_NV_RECOVERY_SUBCODE));
	}
}

vb2_error_t vb2_fw_init_gbb(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_gbb_header *gbb;
	struct vb2_workbuf wb;
	vb2_error_t rv;

	vb2_workbuf_from_ctx(ctx, &wb);

	/* Read GBB into next chunk of work buffer */
	gbb = vb2_workbuf_alloc(&wb, sizeof(*gbb));
	if (!gbb)
		return VB2_ERROR_GBB_WORKBUF;

	rv = vb2_read_gbb_header(ctx, gbb);
	if (rv)
		return rv;

	/* Keep on the work buffer permanently */
	sd->gbb_offset = vb2_offset_of(sd, gbb);
	vb2_set_workbuf_used(ctx, vb2_offset_of(sd, wb.buf));

	/* Set any context flags based on GBB flags */
	if (gbb->flags & VB2_GBB_FLAG_DISABLE_FWMP)
		ctx->flags |= VB2_CONTEXT_NO_SECDATA_FWMP;

	return VB2_SUCCESS;
}

vb2_error_t vb2_check_dev_switch(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_gbb_header *gbb = vb2_get_gbb(ctx);
	uint32_t flags = 0;
	uint32_t old_flags;
	int is_dev = 0;
	int valid_secdata = 1;
	vb2_error_t rv;

	/* Check whether secdata_firmware is initialized */
	if (!(sd->status & VB2_SD_STATUS_SECDATA_FIRMWARE_INIT))
		valid_secdata = 0;

	/* Read secure flags */
	flags = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS);
	old_flags = flags;

	/* Handle dev disable request */
	if (valid_secdata && vb2_nv_get(ctx, VB2_NV_DISABLE_DEV_REQUEST)) {
		flags &= ~VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE;

		/* Clear the request */
		vb2_nv_set(ctx, VB2_NV_DISABLE_DEV_REQUEST, 0);
	}

	/*
	 * Check if we've been asked by the caller to disable dev mode.  Note
	 * that hardware switch and GBB flag will take precedence over this.
	 */
	if (ctx->flags & VB2_CONTEXT_DISABLE_DEVELOPER_MODE)
		flags &= ~VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE;

	/* Check virtual dev switch */
	if (flags & VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE)
		is_dev = 1;

	/* Check if GBB is forcing dev mode */
	if (gbb->flags & VB2_GBB_FLAG_FORCE_DEV_SWITCH_ON)
		is_dev = 1;

	/* Handle whichever mode we end up in */
	if (is_dev) {
		/* Developer mode */
		sd->flags |= VB2_SD_FLAG_DEV_MODE_ENABLED;
		ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;

		flags |= VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER;
	} else {
		/* Normal mode */
		flags &= ~VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER;

		/*
		 * Disable dev_boot_* flags.  This ensures they will be
		 * initially disabled if the user later transitions back into
		 * developer mode.
		 */
		vb2_nv_set(ctx, VB2_NV_DEV_BOOT_USB, 0);
		vb2_nv_set(ctx, VB2_NV_DEV_BOOT_LEGACY, 0);
		vb2_nv_set(ctx, VB2_NV_DEV_BOOT_SIGNED_ONLY, 0);
		vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT, 0);
	}

	if (ctx->flags & VB2_CONTEXT_FORCE_WIPEOUT_MODE)
		vb2_nv_set(ctx, VB2_NV_REQ_WIPEOUT, 1);

	if (flags != old_flags) {
		/*
		 * Just changed dev mode state.  Clear TPM owner.  This must be
		 * done here instead of simply passing a flag to
		 * vb2_check_tpm_clear(), because we don't want to update
		 * last_boot_developer and then fail to clear the TPM owner.
		 *
		 * Note that we do this even if secdata_firmware is having
		 * issues, since the TPM owner and secdata_firmware may be
		 * independent, and we want the owner to be cleared if *this
		 * boot* is different than the last one (perhaps due to GBB or
		 * hardware override).
		 */
		rv = vb2ex_tpm_clear_owner(ctx);
		/* Check for failure to clear owner */
		if (valid_secdata && rv) {
			/*
			 * Note that this truncates rv to 8 bit.  Which
			 * is not as useful as the full error code, but
			 * we don't have NVRAM space to store the full
			 * 32-bit code.
			 */
			vb2api_fail(ctx, VB2_RECOVERY_TPM_CLEAR_OWNER, rv);
			return rv;
		}

		/* Save new flags */
		vb2_secdata_firmware_set(ctx, VB2_SECDATA_FIRMWARE_FLAGS,
					 flags);
	}

	return VB2_SUCCESS;
}

vb2_error_t vb2_check_tpm_clear(struct vb2_context *ctx)
{
	vb2_error_t rv;

	/* Check if we've been asked to clear the owner */
	if (!vb2_nv_get(ctx, VB2_NV_CLEAR_TPM_OWNER_REQUEST))
		return VB2_SUCCESS;  /* No need to clear */

	/* Request applies one time only */
	vb2_nv_set(ctx, VB2_NV_CLEAR_TPM_OWNER_REQUEST, 0);

	/* Try clearing */
	rv = vb2ex_tpm_clear_owner(ctx);
	if (rv) {
		/*
		 * Note that this truncates rv to 8 bit.  Which is not as
		 * useful as the full error code, but we don't have NVRAM space
		 * to store the full 32-bit code.
		 */
		vb2api_fail(ctx, VB2_RECOVERY_TPM_CLEAR_OWNER, rv);
		return rv;
	}

	/* Clear successful */
	vb2_nv_set(ctx, VB2_NV_CLEAR_TPM_OWNER_DONE, 1);
	return VB2_SUCCESS;
}

vb2_error_t vb2_select_fw_slot(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	uint32_t tries;

	/* Get result of last boot */
	sd->last_fw_slot = vb2_nv_get(ctx, VB2_NV_FW_TRIED);
	sd->last_fw_result = vb2_nv_get(ctx, VB2_NV_FW_RESULT);

	/* Save to the previous result fields in NV storage */
	vb2_nv_set(ctx, VB2_NV_FW_PREV_TRIED, sd->last_fw_slot);
	vb2_nv_set(ctx, VB2_NV_FW_PREV_RESULT, sd->last_fw_result);

	/* Clear result, since we don't know what will happen this boot */
	vb2_nv_set(ctx, VB2_NV_FW_RESULT, VB2_FW_RESULT_UNKNOWN);

	/* Get slot to try */
	sd->fw_slot = vb2_nv_get(ctx, VB2_NV_TRY_NEXT);

	/* Check try count */
	tries = vb2_nv_get(ctx, VB2_NV_TRY_COUNT);

	if (sd->last_fw_result == VB2_FW_RESULT_TRYING &&
	    sd->last_fw_slot == sd->fw_slot &&
	    tries == 0) {
		/*
		 * We used up our last try on the previous boot, so fall back
		 * to the other slot this boot.
		 */
		sd->fw_slot = 1 - sd->fw_slot;
		vb2_nv_set(ctx, VB2_NV_TRY_NEXT, sd->fw_slot);
	}

	if (tries > 0) {
		/* Still trying this firmware */
		vb2_nv_set(ctx, VB2_NV_FW_RESULT, VB2_FW_RESULT_TRYING);

		/* Decrement non-zero try count, unless told not to */
		if (!(ctx->flags & VB2_CONTEXT_NOFAIL_BOOT))
			vb2_nv_set(ctx, VB2_NV_TRY_COUNT, tries - 1);
	}

	/* Store the slot we're trying */
	vb2_nv_set(ctx, VB2_NV_FW_TRIED, sd->fw_slot);

	/* Set context flag if we're using slot B */
	if (sd->fw_slot)
		ctx->flags |= VB2_CONTEXT_FW_SLOT_B;

	/* Set status flag */
	sd->status |= VB2_SD_STATUS_CHOSE_SLOT;

	return VB2_SUCCESS;
}

vb2_error_t vb2_enable_developer_mode(struct vb2_context *ctx)
{
	uint32_t flags;

	VB2_DEBUG("Enabling developer mode...\n");

	flags = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS);
	flags |= VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE;
	vb2_secdata_firmware_set(ctx, VB2_SECDATA_FIRMWARE_FLAGS, flags);

	VB2_DEBUG("Mode change will take effect on next reboot\n");

	return VB2_SUCCESS;
}

int vb2_allow_recovery(struct vb2_context *ctx)
{
	/* VB2_GBB_FLAG_FORCE_MANUAL_RECOVERY forces this to always return
	   true. */
	if (vb2_get_gbb(ctx)->flags & VB2_GBB_FLAG_FORCE_MANUAL_RECOVERY)
		return 1;

	/*
	 * If EC is in RW, it implies recovery wasn't manually requested.
	 * On some platforms, EC_IN_RW can't be reset by the EC, thus, this may
	 * return false (=RW). That's ok because if recovery is manual, we will
	 * get the right signal and that's the case we care about.
	 */
	if (!vb2ex_ec_trusted())
		return 0;

	/* Now we confidently check the recovery switch state at boot */
	return !!(vb2_get_sd(ctx)->flags & VB2_SD_FLAG_MANUAL_RECOVERY);
}

int vb2api_need_reboot_for_display(struct vb2_context *ctx)
{
	if (!(vb2_get_sd(ctx)->flags & VB2_SD_FLAG_DISPLAY_AVAILABLE)) {
		VB2_DEBUG("Need reboot to initialize display\n");
		vb2_nv_set(ctx, VB2_NV_DISPLAY_REQUEST, 1);
		return 1;
	}
	return 0;
}
