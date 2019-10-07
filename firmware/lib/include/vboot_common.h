/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Common functions between firmware and kernel verified boot.
 */

#ifndef VBOOT_REFERENCE_VBOOT_COMMON_H_
#define VBOOT_REFERENCE_VBOOT_COMMON_H_

#include "2api.h"
#include "2struct.h"
#include "vboot_struct.h"

/**
 * Initialize a public key to refer to [key_data].
 */
void PublicKeyInit(struct vb2_packed_key *key,
		   uint8_t *key_data, uint64_t key_size);

/**
 * Copy a public key from [src] to [dest].
 *
 * Returns 0 if success, non-zero if error.
 */
int PublicKeyCopy(struct vb2_packed_key *dest,
		  const struct vb2_packed_key *src);

#endif  /* VBOOT_REFERENCE_VBOOT_COMMON_H_ */
