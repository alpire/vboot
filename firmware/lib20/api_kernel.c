/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Externally-callable APIs
 * (Kernel portion)
 */

#include "2api.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2rsa.h"
#include "2secdata.h"
#include "2sha.h"
#include "2sysincludes.h"
#include "vb2_common.h"
#include "vboot_struct.h"

vb2_error_t vb2api_kernel_phase1(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_workbuf wb;
	struct vb2_packed_key *packed_key;
	vb2_error_t rv;

	vb2_workbuf_from_ctx(ctx, &wb);

	/*
	 * Init secdata_kernel and secdata_fwmp spaces.  No need to init
	 * secdata_firmware, since it was already read during firmware
	 * verification.  Ignore errors in recovery mode.
	 */
	rv = vb2_secdata_kernel_init(ctx);
	if (rv && !(ctx->flags & VB2_CONTEXT_RECOVERY_MODE)) {
		VB2_DEBUG("TPM: init secdata_kernel returned %#x\n", rv);
		vb2api_fail(ctx, VB2_RECOVERY_SECDATA_KERNEL_INIT, rv);
		return rv;
	}
	rv = vb2_secdata_fwmp_init(ctx);
	if (rv && !(ctx->flags & VB2_CONTEXT_RECOVERY_MODE)) {
		VB2_DEBUG("TPM: init secdata_fwmp returned %#x\n", rv);
		vb2api_fail(ctx, VB2_RECOVERY_SECDATA_FWMP_INIT, rv);
		return rv;
	}

	/* Read kernel version from secdata. */
	sd->kernel_version_secdata =
		vb2_secdata_kernel_get(ctx, VB2_SECDATA_KERNEL_VERSIONS);
	sd->vbsd->kernel_version_tpm = sd->kernel_version_secdata;
	sd->vbsd->kernel_version_tpm_start = sd->kernel_version_secdata;

	/* Find the key to use to verify the kernel keyblock */
	if ((ctx->flags & VB2_CONTEXT_RECOVERY_MODE)) {
		/* Load recovery key from GBB. */
		rv = vb2_gbb_read_recovery_key(ctx, &packed_key, NULL, &wb);
		if (rv) {
			if (vb2_allow_recovery(ctx))
				VB2_DIE("GBB read recovery key failed.\n");
			else
				/*
				 * If we're headed for the BROKEN screen,
				 * we won't need the recovery key.  Just
				 * short-circuit with success.
				 */
				return VB2_SUCCESS;
		}
	} else {
		/* Kernel subkey from firmware preamble */
		struct vb2_fw_preamble *pre;

		/* Make sure we have a firmware preamble loaded */
		if (!sd->preamble_size)
			return VB2_ERROR_API_KPHASE1_PREAMBLE;

		pre = (struct vb2_fw_preamble *)
			vb2_member_of(sd, sd->preamble_offset);
		packed_key = &pre->kernel_subkey;
	}

	sd->kernel_key_offset = vb2_offset_of(sd, packed_key);
	sd->kernel_key_size = packed_key->key_offset + packed_key->key_size;

	vb2_set_workbuf_used(ctx, vb2_offset_of(sd, wb.buf));

	return VB2_SUCCESS;
}

vb2_error_t vb2api_load_kernel_vblock(struct vb2_context *ctx)
{
	vb2_error_t rv;

	/* Verify kernel keyblock */
	rv = vb2_load_kernel_keyblock(ctx);
	if (rv)
		return rv;

	/* Verify kernel preamble */
	rv = vb2_load_kernel_preamble(ctx);
	if (rv)
		return rv;

	return VB2_SUCCESS;
}

vb2_error_t vb2api_get_kernel_size(struct vb2_context *ctx,
				   uint32_t *offset_ptr, uint32_t *size_ptr)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	const struct vb2_kernel_preamble *pre;

	/* Get preamble pointer */
	if (!sd->preamble_size)
		return VB2_ERROR_API_GET_KERNEL_SIZE_PREAMBLE;

	pre = (const struct vb2_kernel_preamble *)
		vb2_member_of(sd, sd->preamble_offset);

	if (offset_ptr) {
		/* The kernel implicitly follows the preamble */
		*offset_ptr = sd->vblock_preamble_offset +
			sd->preamble_size;
	}

	if (size_ptr) {
		/* Expect the kernel to be the size of data we signed */
		*size_ptr = pre->body_signature.data_size;
	}

	return VB2_SUCCESS;
}

vb2_error_t vb2api_verify_kernel_data(struct vb2_context *ctx, const void *buf,
				      uint32_t size)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_kernel_preamble *pre;
	struct vb2_digest_context *dc;
	struct vb2_public_key key;
	struct vb2_workbuf wb;

	uint8_t *digest;
	uint32_t digest_size;

	vb2_error_t rv;

	vb2_workbuf_from_ctx(ctx, &wb);

	/* Get preamble pointer */
	if (!sd->preamble_size)
		return VB2_ERROR_API_VERIFY_KDATA_PREAMBLE;

	pre = (struct vb2_kernel_preamble *)
		vb2_member_of(sd, sd->preamble_offset);

	/* Make sure we were passed the right amount of data */
	if (size != pre->body_signature.data_size)
		return VB2_ERROR_API_VERIFY_KDATA_SIZE;

	/* Allocate workbuf space for the hash */
	dc = vb2_workbuf_alloc(&wb, sizeof(*dc));
	if (!dc)
		return VB2_ERROR_API_VERIFY_KDATA_WORKBUF;

	/*
	 * Unpack the kernel data key to see which hashing algorithm we
	 * should use.
	 *
	 * TODO: really, the kernel body should be hashed, and not signed,
	 * because the signature we're checking is already signed as part of
	 * the kernel preamble.  But until we can change the signing scripts,
	 * we're stuck with a signature here instead of a hash.
	 */
	if (!sd->data_key_size)
		return VB2_ERROR_API_VERIFY_KDATA_KEY;

	rv = vb2_unpack_key_buffer(&key,
				   vb2_member_of(sd, sd->data_key_offset),
				   sd->data_key_size);
	if (rv)
		return rv;

	rv = vb2_digest_init(dc, key.hash_alg);
	if (rv)
		return rv;

	rv = vb2_digest_extend(dc, buf, size);
	if (rv)
		return rv;

	digest_size = vb2_digest_size(key.hash_alg);
	digest = vb2_workbuf_alloc(&wb, digest_size);
	if (!digest)
		return VB2_ERROR_API_CHECK_HASH_WORKBUF_DIGEST;

	rv = vb2_digest_finalize(dc, digest, digest_size);
	if (rv)
		return rv;

	/*
	 * The body signature is currently a *signature* of the body data, not
	 * just its hash.  So we need to verify the signature.
	 */

	/*
	 * Check digest vs. signature.  Note that this destroys the signature.
	 * That's ok, because we only check each signature once per boot.
	 */
	return vb2_verify_digest(&key, &pre->body_signature, digest, &wb);
}

vb2_error_t vb2api_kernel_phase3(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	/*
	 * If the kernel is a newer version than in secure storage, and the
	 * kernel signature is valid, and we're not in recovery mode, and we're
	 * allowed to, roll forward the version in secure storage.
	 */
	if (sd->kernel_version > sd->kernel_version_secdata &&
	    (sd->flags & VB2_SD_FLAG_KERNEL_SIGNED) &&
	    !(ctx->flags & VB2_CONTEXT_RECOVERY_MODE) &&
	    (ctx->flags & VB2_CONTEXT_ALLOW_KERNEL_ROLL_FORWARD)) {
		vb2_secdata_kernel_set(ctx, VB2_SECDATA_KERNEL_VERSIONS,
					    sd->kernel_version);
		sd->kernel_version_secdata = sd->kernel_version;
	}

	return VB2_SUCCESS;
}
