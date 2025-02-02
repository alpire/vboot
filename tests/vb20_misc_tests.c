/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Tests for misc library
 */

#include <stdio.h>

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2rsa.h"
#include "2secdata.h"
#include "2sysincludes.h"
#include "test_common.h"
#include "vb2_common.h"

/* Common context for tests */
static uint8_t workbuf[VB2_FIRMWARE_WORKBUF_RECOMMENDED_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static struct vb2_context *ctx;
static struct vb2_shared_data *sd;
static struct vb2_gbb_header gbb;

/* Mocked function data */

static struct {
	struct vb2_gbb_header h;
	struct vb2_packed_key rootkey;
	char rootkey_data[32];
} mock_gbb;

static struct {
	/* Keyblock */
	struct {
		struct vb2_keyblock kb;
		char data_key_data[16];
		uint8_t kbdata[128];
	} k;
	/* Preamble follows keyblock */
	struct {
		struct vb2_fw_preamble pre;
		uint8_t predata[128];
	} p;

} mock_vblock;

static int mock_read_res_fail_on_call;
static int mock_unpack_key_retval;
static int mock_verify_keyblock_retval;
static int mock_verify_preamble_retval;

/* Type of test to reset for */
enum reset_type {
	FOR_KEYBLOCK,
	FOR_PREAMBLE
};

static void reset_common_data(enum reset_type t)
{
	struct vb2_keyblock *kb = &mock_vblock.k.kb;
	struct vb2_fw_preamble *pre = &mock_vblock.p.pre;

	memset(workbuf, 0xaa, sizeof(workbuf));

	TEST_SUCC(vb2api_init(workbuf, sizeof(workbuf), &ctx),
		  "vb2api_init failed");

	memset(&gbb, 0, sizeof(gbb));
	sd = vb2_get_sd(ctx);

	vb2_nv_init(ctx);

	vb2api_secdata_firmware_create(ctx);
	vb2_secdata_firmware_init(ctx);

	mock_read_res_fail_on_call = 0;
	mock_unpack_key_retval = VB2_SUCCESS;
	mock_verify_keyblock_retval = VB2_SUCCESS;
	mock_verify_preamble_retval = VB2_SUCCESS;

	/* Set up mock data for verifying keyblock */
	sd->fw_version_secdata = 0x20002;
	vb2_secdata_firmware_set(ctx, VB2_SECDATA_FIRMWARE_VERSIONS,
				 sd->fw_version_secdata);

	gbb.rootkey_offset = vb2_offset_of(&mock_gbb, &mock_gbb.rootkey);
	gbb.rootkey_size = sizeof(mock_gbb.rootkey_data);
	sd->last_fw_result = VB2_FW_RESULT_SUCCESS;

	mock_gbb.rootkey.algorithm = 11;
	mock_gbb.rootkey.key_offset =
		vb2_offset_of(&mock_gbb.rootkey,
			      &mock_gbb.rootkey_data);
	mock_gbb.rootkey.key_size = sizeof(mock_gbb.rootkey_data);

	kb->keyblock_size = sizeof(mock_vblock.k);
	kb->data_key.algorithm = 7;
	kb->data_key.key_version = 2;
	kb->data_key.key_offset =
		vb2_offset_of(&mock_vblock.k, &mock_vblock.k.data_key_data) -
		vb2_offset_of(&mock_vblock.k, &kb->data_key);
	kb->data_key.key_size = sizeof(mock_vblock.k.data_key_data);
	strcpy(mock_vblock.k.data_key_data, "data key data!!");

	pre->preamble_size = sizeof(mock_vblock.p);
	pre->firmware_version = 2;

	/* If verifying preamble, verify keyblock first to set up data key */
	if (t == FOR_PREAMBLE)
		vb2_load_fw_keyblock(ctx);
};

/* Mocked functions */
struct vb2_gbb_header *vb2_get_gbb(struct vb2_context *c)
{
	return &gbb;
}

vb2_error_t vb2ex_read_resource(struct vb2_context *c,
				enum vb2_resource_index index, uint32_t offset,
				void *buf, uint32_t size)
{
	uint8_t *rptr;
	uint32_t rsize;

	if (--mock_read_res_fail_on_call == 0)
		return VB2_ERROR_EX_READ_RESOURCE_INDEX;

	switch(index) {
	case VB2_RES_GBB:
		rptr = (uint8_t *)&mock_gbb;
		rsize = sizeof(mock_gbb);
		break;
	case VB2_RES_FW_VBLOCK:
		rptr = (uint8_t *)&mock_vblock;
		rsize = sizeof(mock_vblock);
		break;
	default:
		return VB2_ERROR_EX_READ_RESOURCE_INDEX;
	}

	if (offset > rsize || offset + size > rsize)
		return VB2_ERROR_EX_READ_RESOURCE_SIZE;

	memcpy(buf, rptr + offset, size);
	return VB2_SUCCESS;
}

vb2_error_t vb2_unpack_key_buffer(struct vb2_public_key *key,
				  const uint8_t *buf, uint32_t size)
{
	key->arrsize = 0;
	return mock_unpack_key_retval;
}

vb2_error_t vb2_verify_keyblock(struct vb2_keyblock *block, uint32_t size,
				const struct vb2_public_key *key,
				const struct vb2_workbuf *wb)
{
	return mock_verify_keyblock_retval;
}

vb2_error_t vb2_verify_fw_preamble(struct vb2_fw_preamble *preamble,
				   uint32_t size,
				   const struct vb2_public_key *key,
				   const struct vb2_workbuf *wb)
{
	return mock_verify_preamble_retval;
}

/* Tests */

static void verify_keyblock_tests(void)
{
	struct vb2_keyblock *kb = &mock_vblock.k.kb;
	struct vb2_packed_key *k;
	int expected_offset;

	/* Test successful call */
	reset_common_data(FOR_KEYBLOCK);
	expected_offset = sd->workbuf_used;
	TEST_SUCC(vb2_load_fw_keyblock(ctx), "keyblock verify");
	TEST_EQ(sd->fw_version, 0x20000, "keyblock version");
	TEST_EQ(sd->vblock_preamble_offset, sizeof(mock_vblock.k),
		"preamble offset");
	TEST_EQ(sd->data_key_offset, expected_offset,
		"keyblock data key offset");
	TEST_EQ(sd->workbuf_used,
		vb2_wb_round_up(sd->data_key_offset +
				sd->data_key_size),
		"workbuf used");

	/* Make sure data key was properly saved */
	k = vb2_member_of(sd, sd->data_key_offset);
	TEST_EQ(k->algorithm, 7, "data key algorithm");
	TEST_EQ(k->key_version, 2, "data key version");
	TEST_EQ(k->key_size, sizeof(mock_vblock.k.data_key_data),
		"data key size");
	TEST_EQ(memcmp(vb2_member_of(k, k->key_offset),
		       mock_vblock.k.data_key_data,
		       sizeof(mock_vblock.k.data_key_data)),
		0, "data key data");
	TEST_EQ(sd->workbuf_used,
		vb2_wb_round_up(sd->data_key_offset +
				sd->data_key_size),
		"workbuf used after");

	/* Test failures */
	reset_common_data(FOR_KEYBLOCK);
	sd->workbuf_used = sd->workbuf_size + VB2_WORKBUF_ALIGN -
			   vb2_wb_round_up(gbb.rootkey_size);
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_FW_KEYBLOCK_WORKBUF_ROOT_KEY,
		"keyblock not enough workbuf for root key");

	reset_common_data(FOR_KEYBLOCK);
	gbb.rootkey_size = sizeof(mock_gbb);
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_EX_READ_RESOURCE_SIZE,
		"keyblock read root key");

	reset_common_data(FOR_KEYBLOCK);
	mock_unpack_key_retval = VB2_ERROR_UNPACK_KEY_SIG_ALGORITHM;
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_UNPACK_KEY_SIG_ALGORITHM,
		"keyblock unpack root key");

	reset_common_data(FOR_KEYBLOCK);
	sd->workbuf_used = sd->workbuf_size -
			   vb2_wb_round_up(gbb.rootkey_size);
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_FW_KEYBLOCK_WORKBUF_HEADER,
		"keyblock not enough workbuf for header");

	reset_common_data(FOR_KEYBLOCK);
	mock_read_res_fail_on_call = 2;
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_EX_READ_RESOURCE_INDEX,
		"keyblock read keyblock header");

	reset_common_data(FOR_KEYBLOCK);
	sd->workbuf_used = sd->workbuf_size -
			   vb2_wb_round_up(gbb.rootkey_size) -
			   vb2_wb_round_up(sizeof(struct vb2_keyblock));
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_FW_KEYBLOCK_WORKBUF,
		"keyblock not enough workbuf for entire keyblock");

	reset_common_data(FOR_KEYBLOCK);
	kb->keyblock_size = sizeof(mock_vblock) + 1;
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_EX_READ_RESOURCE_SIZE,
		"keyblock read keyblock");

	reset_common_data(FOR_KEYBLOCK);
	mock_verify_keyblock_retval = VB2_ERROR_KEYBLOCK_MAGIC;
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_KEYBLOCK_MAGIC,
		"keyblock verify keyblock");

	reset_common_data(FOR_KEYBLOCK);
	kb->data_key.key_version = 0x10000;
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_FW_KEYBLOCK_VERSION_RANGE,
		"keyblock version range");

	reset_common_data(FOR_KEYBLOCK);
	kb->data_key.key_version = 1;
	TEST_EQ(vb2_load_fw_keyblock(ctx),
		VB2_ERROR_FW_KEYBLOCK_VERSION_ROLLBACK,
		"keyblock rollback");

	reset_common_data(FOR_KEYBLOCK);
	kb->data_key.key_version = 1;
	gbb.flags |= VB2_GBB_FLAG_DISABLE_FW_ROLLBACK_CHECK;
	TEST_SUCC(vb2_load_fw_keyblock(ctx), "keyblock rollback with GBB flag");
}

static void verify_preamble_tests(void)
{
	struct vb2_fw_preamble *pre = &mock_vblock.p.pre;
	int expected_offset;
	uint32_t v;

	/* Test successful call */
	reset_common_data(FOR_PREAMBLE);
	expected_offset = sd->workbuf_used;
	TEST_SUCC(vb2_load_fw_preamble(ctx), "preamble good");
	TEST_EQ(sd->fw_version, 0x20002, "combined version");
	TEST_EQ(sd->preamble_offset, expected_offset,
		"preamble offset");
	TEST_EQ(sd->preamble_size, pre->preamble_size, "preamble size");
	TEST_EQ(sd->workbuf_used,
		vb2_wb_round_up(sd->preamble_offset +
				sd->preamble_size),
		"workbuf used");

	/* Expected failures */
	reset_common_data(FOR_PREAMBLE);
	sd->data_key_size = 0;
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_FW_PREAMBLE2_DATA_KEY,
		"preamble no data key");

	reset_common_data(FOR_PREAMBLE);
	mock_unpack_key_retval = VB2_ERROR_UNPACK_KEY_HASH_ALGORITHM;
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_UNPACK_KEY_HASH_ALGORITHM,
		"preamble unpack data key");

	reset_common_data(FOR_PREAMBLE);
	sd->workbuf_used = sd->workbuf_size + VB2_WORKBUF_ALIGN -
			   vb2_wb_round_up(sizeof(struct vb2_fw_preamble));
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_FW_PREAMBLE2_WORKBUF_HEADER,
		"preamble not enough workbuf for header");

	reset_common_data(FOR_PREAMBLE);
	sd->vblock_preamble_offset = sizeof(mock_vblock);
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_EX_READ_RESOURCE_SIZE,
		"preamble read header");

	reset_common_data(FOR_PREAMBLE);
	sd->workbuf_used = sd->workbuf_size + VB2_WORKBUF_ALIGN -
			   vb2_wb_round_up(sizeof(mock_vblock.p));
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_FW_PREAMBLE2_WORKBUF,
		"preamble not enough workbuf");

	reset_common_data(FOR_PREAMBLE);
	pre->preamble_size = sizeof(mock_vblock);
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_EX_READ_RESOURCE_SIZE,
		"preamble read full");

	reset_common_data(FOR_PREAMBLE);
	mock_verify_preamble_retval = VB2_ERROR_PREAMBLE_SIG_INVALID;
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_PREAMBLE_SIG_INVALID,
		"preamble verify");

	reset_common_data(FOR_PREAMBLE);
	pre->firmware_version = 0x10000;
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_FW_PREAMBLE_VERSION_RANGE,
		"preamble version range");

	reset_common_data(FOR_PREAMBLE);
	pre->firmware_version = 1;
	TEST_EQ(vb2_load_fw_preamble(ctx),
		VB2_ERROR_FW_PREAMBLE_VERSION_ROLLBACK,
		"preamble version rollback");

	reset_common_data(FOR_PREAMBLE);
	pre->firmware_version = 1;
	gbb.flags |= VB2_GBB_FLAG_DISABLE_FW_ROLLBACK_CHECK;
	TEST_SUCC(vb2_load_fw_preamble(ctx), "version rollback with GBB flag");

	reset_common_data(FOR_PREAMBLE);
	pre->firmware_version = 3;
	TEST_SUCC(vb2_load_fw_preamble(ctx),
		  "preamble version roll forward");
	v = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_VERSIONS);
	TEST_EQ(v, 0x20003, "roll forward");

	/* Newer version without result success doesn't roll forward */
	reset_common_data(FOR_PREAMBLE);
	pre->firmware_version = 3;
	sd->last_fw_result = VB2_FW_RESULT_UNKNOWN;
	TEST_SUCC(vb2_load_fw_preamble(ctx),
		  "preamble version no roll forward 1");
	v = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_VERSIONS);
	TEST_EQ(v, 0x20002, "no roll forward");

	/* Newer version with success but for other slot doesn't roll forward */
	reset_common_data(FOR_PREAMBLE);
	pre->firmware_version = 3;
	sd->last_fw_slot = 1;
	TEST_SUCC(vb2_load_fw_preamble(ctx),
		  "preamble version no roll forward 2");
	v = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_VERSIONS);
	TEST_EQ(v, 0x20002, "no roll forward");
}

int main(int argc, char* argv[])
{
	verify_keyblock_tests();
	verify_preamble_tests();

	return gTestSuccess ? 0 : 255;
}
