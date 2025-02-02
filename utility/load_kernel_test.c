/* Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Routines for verifying a file's signature. Useful in testing the core
 * RSA verification implementation.
 */

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "2api.h"
#include "2misc.h"
#include "2sysincludes.h"
#include "host_common.h"
#include "load_kernel_fw.h"
#include "vboot_kernel.h"

#define LBA_BYTES 512
#define KERNEL_BUFFER_SIZE 0xA00000

static uint8_t workbuf[VB2_KERNEL_WORKBUF_RECOMMENDED_SIZE];
static struct vb2_context *ctx;
static struct vb2_shared_data *sd;

static uint8_t shared_data[VB_SHARED_DATA_MIN_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static VbSharedDataHeader *shared = (VbSharedDataHeader *)shared_data;

/* Global variables for stub functions */
static LoadKernelParams lkp;
static FILE *image_file = NULL;


/* Boot device stub implementations to read from the image file */
vb2_error_t VbExDiskRead(VbExDiskHandle_t handle, uint64_t lba_start,
			 uint64_t lba_count, void *buffer)
{
	printf("Read(%" PRIu64 ", %" PRIu64 ")\n", lba_start, lba_count);

	if (lba_start >= lkp.streaming_lba_count ||
	    lba_start + lba_count > lkp.streaming_lba_count) {
		fprintf(stderr,
			"Read overrun: %" PRIu64 " + %" PRIu64
			" > %" PRIu64 "\n", lba_start,
			lba_count, lkp.streaming_lba_count);
		return 1;
	}

	if (0 != fseek(image_file, lba_start * lkp.bytes_per_lba, SEEK_SET) ||
	    1 != fread(buffer, lba_count * lkp.bytes_per_lba, 1, image_file)) {
		fprintf(stderr, "Read error.");
		return 1;
	}
	return VB2_SUCCESS;
}


vb2_error_t VbExDiskWrite(VbExDiskHandle_t handle, uint64_t lba_start,
			  uint64_t lba_count, const void *buffer)
{
	printf("Write(%" PRIu64 ", %" PRIu64 ")\n", lba_start, lba_count);

	if (lba_start >= lkp.streaming_lba_count ||
	    lba_start + lba_count > lkp.streaming_lba_count) {
		fprintf(stderr,
			"Read overrun: %" PRIu64 " + %" PRIu64
			" > %" PRIu64 "\n", lba_start, lba_count,
			lkp.streaming_lba_count);
		return 1;
	}

	/* TODO: enable writes, once we're sure it won't trash
	   our example file */
	return VB2_SUCCESS;

	fseek(image_file, lba_start * lkp.bytes_per_lba, SEEK_SET);
	if (1 != fwrite(buffer, lba_count * lkp.bytes_per_lba, 1, image_file)) {
		fprintf(stderr, "Read error.");
		return 1;
	}
	return VB2_SUCCESS;
}


#define BOOT_FLAG_DEVELOPER (1 << 0)
#define BOOT_FLAG_RECOVERY (1 << 1)

/* Main routine */
int main(int argc, char* argv[])
{
	const char* image_name;
	uint64_t key_size = 0;
	struct vb2_packed_key *key_blob = NULL;
	struct vb2_gbb_header* gbb;
	vb2_error_t rv;
	int c, argsleft;
	int errorcnt = 0;
	char *e = 0;

	memset(&lkp, 0, sizeof(LoadKernelParams));
	lkp.bytes_per_lba = LBA_BYTES;
	int boot_flags = BOOT_FLAG_RECOVERY;

	/* Parse options */
	opterr = 0;
	while ((c = getopt(argc, argv, ":b:")) != -1) {
		switch (c) {
		case 'b':
			boot_flags = strtoull(optarg, &e, 0);
			if (!*optarg || (e && *e)) {
				fprintf(stderr,
					"Invalid argument to -%c: \"%s\"\n",
					c, optarg);
				errorcnt++;
			}
			break;
		case '?':
			fprintf(stderr, "Unrecognized switch: -%c\n", optopt);
			errorcnt++;
			break;
		case ':':
			fprintf(stderr, "Missing argument to -%c\n", optopt);
			errorcnt++;
			break;
		default:
			errorcnt++;
			break;
		}
	}

	/* Update argc */
	argsleft = argc - optind;

	if (errorcnt || !argsleft) {
		fprintf(stderr,
			"usage: %s [options] <drive_image> [<sign_key>]\n",
			argv[0]);
		fprintf(stderr, "\noptions:\n");
		/* These cases are because uint64_t isn't necessarily the same
		   as ULL. */
		fprintf(stderr, "  -b NUM     boot flag bits (default %d):\n",
			BOOT_FLAG_RECOVERY);
		fprintf(stderr, "               %d = developer mode on\n",
			BOOT_FLAG_DEVELOPER);
		fprintf(stderr, "               %d = recovery mode on\n",
			BOOT_FLAG_RECOVERY);
		return 1;
	}

	image_name = argv[optind];

	/* Read header signing key blob */
	if (argsleft > 1) {
		key_blob = (struct vb2_packed_key *)
			ReadFile(argv[optind+1], &key_size);
		if (!key_blob) {
			fprintf(stderr, "Unable to read key file %s\n",
				argv[optind+1]);
			return 1;
		}
		printf("Read %" PRIu64 " bytes of key from %s\n", key_size,
		       argv[optind+1]);
		if (key_size > 16*1024*1024) {
			fprintf(stderr, "Key blob size=%" PRIu64
				" is ridiculous.\n", key_size);
			free(key_blob);
			return 1;
		}
	}

	/* Initialize the GBB */
	uint32_t gbb_size = sizeof(struct vb2_gbb_header) + key_size;
	gbb = (struct vb2_gbb_header*)malloc(gbb_size);
	memset(gbb, 0, gbb_size);
	memcpy(gbb->signature, VB2_GBB_SIGNATURE, VB2_GBB_SIGNATURE_SIZE);
	gbb->major_version = VB2_GBB_MAJOR_VER;
	gbb->minor_version = VB2_GBB_MINOR_VER;
	gbb->header_size = sizeof(struct vb2_gbb_header);
	/* Fill in the given key, if any, for both root and recovery */
	if (key_blob) {
		gbb->rootkey_offset = gbb->header_size;
		gbb->rootkey_size = key_size;
		memcpy((uint8_t*)gbb + gbb->rootkey_offset, key_blob, key_size);

		gbb->recovery_key_offset = gbb->rootkey_offset;
		gbb->recovery_key_size = key_size;
	}

	printf("bootflags = %d\n", boot_flags);
	lkp.boot_flags = boot_flags;

	/* Get image size */
	printf("Reading from image: %s\n", image_name);
	image_file = fopen(image_name, "rb");
	if (!image_file) {
		fprintf(stderr, "Unable to open image file %s\n", image_name);
		return 1;
	}
	fseek(image_file, 0, SEEK_END);
	lkp.streaming_lba_count = (ftell(image_file) / LBA_BYTES);
	lkp.gpt_lba_count = lkp.streaming_lba_count;
	rewind(image_file);
	printf("Streaming LBA count: %" PRIu64 "\n", lkp.streaming_lba_count);

	/* Allocate a buffer for the kernel */
	lkp.kernel_buffer = malloc(KERNEL_BUFFER_SIZE);
	if(!lkp.kernel_buffer) {
		fprintf(stderr, "Unable to allocate kernel buffer.\n");
		return 1;
	}
	lkp.kernel_buffer_size = KERNEL_BUFFER_SIZE;

	/* Set up vboot context. */
	if (vb2api_init(&workbuf, sizeof(workbuf), &ctx)) {
		fprintf(stderr, "Can't initialize workbuf\n");
		return 1;
	}
	memset(&shared_data, 0, sizeof(shared_data));
	sd = vb2_get_sd(ctx);
	sd->vbsd = shared;

	/* Copy kernel subkey to VBSD, if any */
	if (key_blob) {
		struct vb2_packed_key *dst = (struct vb2_packed_key *)
			(shared_data +
			 vb2_wb_round_up(sizeof(VbSharedDataHeader)));
		shared->kernel_subkey.key_offset =
			(uintptr_t)dst - (uintptr_t)&shared->kernel_subkey;
		shared->kernel_subkey.key_size = key_blob->key_size;
		shared->kernel_subkey.algorithm = key_blob->algorithm;
		shared->kernel_subkey.key_version = key_blob->key_version;
		memcpy(vb2_packed_key_data_mutable(dst),
		       vb2_packed_key_data(key_blob), key_blob->key_size);
	}

	/* Free the key blob, now that we're done with it */
	free(key_blob);

	/* No need to initialize ctx->nvdata[]; defaults are fine */
	/* TODO(chromium:441893): support dev-mode flag and external gpt flag */
	if (boot_flags & BOOT_FLAG_RECOVERY)
		ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	if (boot_flags & BOOT_FLAG_DEVELOPER)
		ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;

	/* Call LoadKernel() */
	rv = LoadKernel(ctx, &lkp);
	printf("LoadKernel() returned %d\n", rv);

	if (VB2_SUCCESS == rv) {
		printf("Partition number:   %u\n", lkp.partition_number);
		printf("Bootloader address: %" PRIu64 "\n",
		       lkp.bootloader_address);
		printf("Bootloader size:    %u\n", lkp.bootloader_size);
		printf("Partition guid:	    "
		       "%02x%02x%02x%02x-%02x%02x-%02x%02x"
		       "-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
		       lkp.partition_guid[3],
		       lkp.partition_guid[2],
		       lkp.partition_guid[1],
		       lkp.partition_guid[0],
		       lkp.partition_guid[5],
		       lkp.partition_guid[4],
		       lkp.partition_guid[7],
		       lkp.partition_guid[6],
		       lkp.partition_guid[8],
		       lkp.partition_guid[9],
		       lkp.partition_guid[10],
		       lkp.partition_guid[11],
		       lkp.partition_guid[12],
		       lkp.partition_guid[13],
		       lkp.partition_guid[14],
		       lkp.partition_guid[15]);
	}

	fclose(image_file);
	free(lkp.kernel_buffer);
	return rv != VB2_SUCCESS;
}
