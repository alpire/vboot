/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Misc functions which need access to vb2_context but are not public APIs
 */

#include "2api.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2rsa.h"
#include "2secdata.h"
#include "2sha.h"
#include "2sysincludes.h"
#include "vb2_common.h"

/*
 * The blob below is the sha1 digest calculated over the packed developer
 * root public key structure.
 */

static const uint8_t dev_key_digest[] = {
	0xb1, 0x1d, 0x74, 0xed, 0xd2, 0x86, 0xc1, 0x44,
	0xe1, 0x13, 0x5b, 0x49, 0xe7, 0xf0, 0xbc, 0x20,
	0xcf, 0x04, 0x1f, 0x10,
};

/**
 * Determine if the root key is the developer key checked into the
 * vboot_reference repository.  Has no effect on boot; just logs this to the
 * debug console.
 *
 * @param root		Root key
 */
static void vb2_report_dev_firmware(struct vb2_public_key *root)
{
	struct vb2_digest_context dc;
	uint8_t digest[sizeof(dev_key_digest)];
	int size = root->arrsize * 4;

	if (!root->arrsize)
		return; /* Must be a test run. */

	if (vb2_digest_init(&dc, VB2_HASH_SHA1) != VB2_SUCCESS)
		return;

	if (vb2_digest_extend(&dc, (uint8_t *)&root->arrsize,
			      sizeof(root->arrsize)) != VB2_SUCCESS)
		return;

	if (vb2_digest_extend(&dc, (uint8_t *)&root->n0inv,
			      sizeof(root->n0inv)) != VB2_SUCCESS)
		return;

	if (vb2_digest_extend(&dc, (uint8_t *)root->n, size) != VB2_SUCCESS)
		return;

	if (vb2_digest_extend(&dc, (uint8_t *)root->rr, size) != VB2_SUCCESS)
		return;

	if (vb2_digest_finalize(&dc, digest, sizeof(digest)) != VB2_SUCCESS)
		return;

	if (!memcmp(digest, dev_key_digest, sizeof(dev_key_digest)))
		VB2_DEBUG("This is developer signed firmware\n");
}

vb2_error_t vb2_load_fw_keyblock(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_gbb_header *gbb = vb2_get_gbb(ctx);
	struct vb2_workbuf wb;

	uint8_t *key_data;
	uint32_t key_size;
	struct vb2_public_key root_key;

	struct vb2_keyblock *kb;
	uint32_t block_size;

	vb2_error_t rv;

	vb2_workbuf_from_ctx(ctx, &wb);

	/* Read the root key */
	key_size = gbb->rootkey_size;
	key_data = vb2_workbuf_alloc(&wb, key_size);
	if (!key_data)
		return VB2_ERROR_FW_KEYBLOCK_WORKBUF_ROOT_KEY;

	rv = vb2ex_read_resource(ctx, VB2_RES_GBB, gbb->rootkey_offset,
				 key_data, key_size);
	if (rv)
		return rv;

	/* Unpack the root key */
	rv = vb2_unpack_key_buffer(&root_key, key_data, key_size);
	if (rv)
		return rv;

	/* If that's the checked-in root key, this is dev-signed firmware */
	vb2_report_dev_firmware(&root_key);

	/* Load the firmware keyblock header after the root key */
	kb = vb2_workbuf_alloc(&wb, sizeof(*kb));
	if (!kb)
		return VB2_ERROR_FW_KEYBLOCK_WORKBUF_HEADER;

	rv = vb2ex_read_resource(ctx, VB2_RES_FW_VBLOCK, 0, kb, sizeof(*kb));
	if (rv)
		return rv;

	block_size = kb->keyblock_size;

	/*
	 * Load the entire keyblock, now that we know how big it is.  Note that
	 * we're loading the entire keyblock instead of just the piece after
	 * the header.  That means we re-read the header.  But that's a tiny
	 * amount of data, and it makes the code much more straightforward.
	 */
	kb = vb2_workbuf_realloc(&wb, sizeof(*kb), block_size);
	if (!kb)
		return VB2_ERROR_FW_KEYBLOCK_WORKBUF;

	rv = vb2ex_read_resource(ctx, VB2_RES_FW_VBLOCK, 0, kb, block_size);
	if (rv)
		return rv;

	/* Verify the keyblock */
	rv = vb2_verify_keyblock(kb, block_size, &root_key, &wb);
	if (rv) {
		vb2api_fail(ctx, VB2_RECOVERY_FW_KEYBLOCK, rv);
		return rv;
	}

	/* Key version is the upper 16 bits of the composite firmware version */
	if (kb->data_key.key_version > VB2_MAX_KEY_VERSION)
		rv = VB2_ERROR_FW_KEYBLOCK_VERSION_RANGE;
	if (!rv && kb->data_key.key_version < (sd->fw_version_secdata >> 16)) {
		if (gbb->flags & VB2_GBB_FLAG_DISABLE_FW_ROLLBACK_CHECK)
			VB2_DEBUG("Ignoring FW key rollback due to GBB flag\n");
		else
			rv = VB2_ERROR_FW_KEYBLOCK_VERSION_ROLLBACK;
	}
	if (rv) {
		vb2api_fail(ctx, VB2_RECOVERY_FW_KEY_ROLLBACK, rv);
		return rv;
	}

	sd->fw_version = kb->data_key.key_version << 16;

	/* Preamble follows the keyblock in the vblock. */
	sd->vblock_preamble_offset = kb->keyblock_size;

	/*
	 * Save the data key in the work buffer.  We'll overwrite the root key
	 * we read above.  That's ok, because now that we have the data key we
	 * no longer need the root key.  First, let's double-check that it is
	 * well-formed though (although the keyblock was signed anyway).
	 */
	rv = vb2_verify_packed_key_inside(kb, block_size, &kb->data_key);
	if (rv)
		return rv;

	/* Save the future offset and size while kb->data_key is still valid.
	   The check above made sure that key_offset and key_size are sane. */
	sd->data_key_offset = vb2_offset_of(sd, key_data);
	sd->data_key_size = kb->data_key.key_offset + kb->data_key.key_size;

	/*
	 * Use memmove() instead of memcpy().  In theory, the destination will
	 * never overlap because with the source because the root key is likely
	 * to be at least as large as the data key, but there's no harm here in
	 * being paranoid.  Make sure we immediately invalidate 'kb' after the
	 * move to guarantee we won't try to access it anymore.
	 */
	memmove(key_data, &kb->data_key, sd->data_key_size);
	kb = NULL;

	/*
	 * Data key will persist in the workbuf after we return.
	 *
	 * Work buffer now contains:
	 *   - vb2_shared_data
	 *   - packed firmware data key
	 */
	vb2_set_workbuf_used(ctx, sd->data_key_offset + sd->data_key_size);

	return VB2_SUCCESS;
}

vb2_error_t vb2_load_fw_preamble(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_gbb_header *gbb = vb2_get_gbb(ctx);
	struct vb2_workbuf wb;

	uint8_t *key_data = vb2_member_of(sd, sd->data_key_offset);
	uint32_t key_size = sd->data_key_size;
	struct vb2_public_key data_key;

	/* Preamble goes in the next unused chunk of work buffer */
	struct vb2_fw_preamble *pre;
	uint32_t pre_size;

	vb2_error_t rv;

	vb2_workbuf_from_ctx(ctx, &wb);

	/* Unpack the firmware data key */
	if (!sd->data_key_size)
		return VB2_ERROR_FW_PREAMBLE2_DATA_KEY;

	rv = vb2_unpack_key_buffer(&data_key, key_data, key_size);
	if (rv)
		return rv;

	/* Load the firmware preamble header */
	pre = vb2_workbuf_alloc(&wb, sizeof(*pre));
	if (!pre)
		return VB2_ERROR_FW_PREAMBLE2_WORKBUF_HEADER;

	rv = vb2ex_read_resource(ctx, VB2_RES_FW_VBLOCK,
				 sd->vblock_preamble_offset,
				 pre, sizeof(*pre));
	if (rv)
		return rv;

	pre_size = pre->preamble_size;

	/* Load the entire firmware preamble, now that we know how big it is */
	pre = vb2_workbuf_realloc(&wb, sizeof(*pre), pre_size);
	if (!pre)
		return VB2_ERROR_FW_PREAMBLE2_WORKBUF;

	rv = vb2ex_read_resource(ctx, VB2_RES_FW_VBLOCK,
				 sd->vblock_preamble_offset,
				 pre, pre_size);
	if (rv)
		return rv;

	/* Work buffer now contains the data subkey data and the preamble */

	/* Verify the preamble */
	rv = vb2_verify_fw_preamble(pre, pre_size, &data_key, &wb);
	if (rv) {
		vb2api_fail(ctx, VB2_RECOVERY_FW_PREAMBLE, rv);
		return rv;
	}

	/*
	 * Firmware version is the lower 16 bits of the composite firmware
	 * version.
	 */
	if (pre->firmware_version > VB2_MAX_PREAMBLE_VERSION)
		rv = VB2_ERROR_FW_PREAMBLE_VERSION_RANGE;
	/* Combine with the key version from vb2_load_fw_keyblock() */
	sd->fw_version |= pre->firmware_version;
	if (!rv && sd->fw_version < sd->fw_version_secdata) {
		if (gbb->flags & VB2_GBB_FLAG_DISABLE_FW_ROLLBACK_CHECK)
			VB2_DEBUG("Ignoring FW rollback due to GBB flag\n");
		else
			rv = VB2_ERROR_FW_PREAMBLE_VERSION_ROLLBACK;
	}
	if (rv) {
		vb2api_fail(ctx, VB2_RECOVERY_FW_ROLLBACK, rv);
		return rv;
	}

	/*
	 * If this is a newer version than in secure storage, and we
	 * successfully booted the same slot last boot, roll forward the
	 * version in secure storage.
	 *
	 * Note that this happens before we've verified the firmware data this
	 * boot; we're relying on the indicator that the last boot was
	 * successful.  That's ok, because even if the firmware data has a
	 * valid hash, the only way we can know if it's functional is to trust
	 * the status from the last boot.
	 */
	if (sd->fw_version > sd->fw_version_secdata &&
	    sd->last_fw_slot == sd->fw_slot &&
	    sd->last_fw_result == VB2_FW_RESULT_SUCCESS) {
		sd->fw_version_secdata = sd->fw_version;
		vb2_secdata_firmware_set(ctx, VB2_SECDATA_FIRMWARE_VERSIONS,
					 sd->fw_version);
	}

	/* Keep track of where we put the preamble */
	sd->preamble_offset = vb2_offset_of(sd, pre);
	sd->preamble_size = pre_size;

	/*
	 * Preamble will persist in work buffer after we return.
	 *
	 * Work buffer now contains:
	 *   - vb2_shared_data
	 *   - vb2_gbb_header
	 *   - packed firmware data key
	 *   - firmware preamble
	 *
	 * TODO: we could move the preamble down over the firmware data key
	 * since we don't need it anymore.
	 */
	vb2_set_workbuf_used(ctx, sd->preamble_offset + pre_size);

	return VB2_SUCCESS;
}
