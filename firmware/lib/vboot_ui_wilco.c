/* Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Common code used by both vboot_ui and Wilco-specific
 * feature support for vboot_ui
 */

#include "2common.h"
#include "2nvstorage.h"
#include "2sysincludes.h"
#include "vboot_api.h"
#include "vboot_display.h"
#include "vboot_ui_common.h"
#include "vboot_ui_wilco.h"

static inline int is_vowel(uint32_t key)
{
	return key == 'A' || key == 'E' || key == 'I' ||
	       key == 'O' || key == 'U';
}

static int vendor_data_length(char *data_value)
{
	for (int len = 0; len <= VENDOR_DATA_LENGTH; len++) {
		if (data_value[len] == '\0')
			return len;
	}

	return VENDOR_DATA_LENGTH;
}

/*
 * Prompt the user to enter the serial number
 */
static vb2_error_t vb2_enter_vendor_data_ui(struct vb2_context *ctx,
					    char *data_value)
{
	int len = vendor_data_length(data_value);
	VbScreenData data = {.vendor_data = {data_value, 1}};

	VbDisplayScreen(ctx, VB_SCREEN_SET_VENDOR_DATA, 1, &data);

	/* We'll loop until the user decides what to do */
	do {
		uint32_t key = VbExKeyboardRead();

		if (vb2_want_shutdown(ctx, key)) {
			VB2_DEBUG("Vendor Data UI - shutdown requested!\n");
			return VBERROR_SHUTDOWN_REQUESTED;
		}
		switch (key) {
		case 0:
			/* Nothing pressed */
			break;
		case VB_KEY_ESC:
			/* Escape pressed - return to developer screen */
			VB2_DEBUG("Vendor Data UI - user pressed Esc: "
				  "exit to Developer screen\n");
			data_value[0] = '\0';
			return VB2_SUCCESS;
		case 'a'...'z':
			key = toupper(key);
			__attribute__ ((fallthrough));
		case '0'...'9':
		case 'A'...'Z':
			if ((len > 0 && is_vowel(key)) ||
			     len >= VENDOR_DATA_LENGTH) {
				vb2_error_beep(VB_BEEP_NOT_ALLOWED);
			} else {
				data_value[len++] = key;
				data_value[len] = '\0';
				VbDisplayScreen(ctx, VB_SCREEN_SET_VENDOR_DATA,
						1, &data);
			}

			VB2_DEBUG("Vendor Data UI - vendor_data: %s\n",
				  data_value);
			break;
		case VB_KEY_BACKSPACE:
			if (len > 0) {
				data_value[--len] = '\0';
				VbDisplayScreen(ctx, VB_SCREEN_SET_VENDOR_DATA,
						1, &data);
			}

			VB2_DEBUG("Vendor Data UI - vendor_data: %s\n",
				  data_value);
			break;
		case VB_KEY_ENTER:
			if (len == VENDOR_DATA_LENGTH) {
				/* Enter pressed - confirm input */
				VB2_DEBUG("Vendor Data UI - user pressed "
					  "Enter: confirm vendor data\n");
				return VB2_SUCCESS;
			} else {
				vb2_error_beep(VB_BEEP_NOT_ALLOWED);
			}
			break;
		default:
			VB2_DEBUG("Vendor Data UI - pressed key %#x\n", key);
			VbCheckDisplayKey(ctx, key, &data);
			break;
		}
		VbExSleepMs(KEY_DELAY_MS);
	} while (1);

	return VB2_SUCCESS;
}

/*
 * Prompt the user to confirm the serial number and write to memory
 */
static vb2_error_t vb2_confirm_vendor_data_ui(struct vb2_context *ctx,
					      char* data_value, VbScreenData *data)
{
	VbDisplayScreen(ctx, VB_SCREEN_CONFIRM_VENDOR_DATA, 1, data);
	/* We'll loop until the user decides what to do */
	do {
		uint32_t key_confirm = VbExKeyboardRead();

		if (vb2_want_shutdown(ctx, key_confirm)) {
			VB2_DEBUG("Confirm Vendor Data UI "
				  "- shutdown requested!\n");
			return VBERROR_SHUTDOWN_REQUESTED;
		}
		switch (key_confirm) {
		case 0:
			/* Nothing pressed */
			break;
		case VB_KEY_ESC:
			/* Escape pressed - return to developer screen */
			VB2_DEBUG("Confirm Vendor Data UI - user "
				  "pressed Esc: exit to Developer screen\n");
			return VB2_SUCCESS;
		case VB_KEY_RIGHT:
		case VB_KEY_LEFT:
			data->vendor_data.selected_index =
				data->vendor_data.selected_index ^ 1;
			VbDisplayScreen(ctx, VB_SCREEN_CONFIRM_VENDOR_DATA,
					1, data);
			VB2_DEBUG("selected_index:%d\n",
				  data->vendor_data.selected_index);
			break;
		case VB_KEY_ENTER:
			/* Enter pressed - write vendor data */
			if (data->vendor_data.selected_index == 0) {
				VB2_DEBUG("Confirm Vendor Data UI - user "
					  "selected YES: "
					  "write vendor data (%s) to VPD\n",
					  data_value);
				vb2_error_t ret = VbExSetVendorData(data_value);

				if (ret == VB2_SUCCESS) {
					vb2_nv_set(ctx,
						   VB2_NV_DISABLE_DEV_REQUEST,
						   1);
					return VBERROR_REBOOT_REQUIRED;
				} else {
					vb2_error_notify(
						"ERROR: Vendor data was not "
						"set.\n"
						"System will now shutdown\n",
						NULL, VB_BEEP_FAILED);
					VbExSleepMs(5000);
					return VBERROR_SHUTDOWN_REQUESTED;
				}
			} else {
				VB2_DEBUG("Confirm Vendor Data UI - user "
					  "selected NO: "
					  "Returning to set screen\n");
				return VB2_SUCCESS;
			}
		default:
			VB2_DEBUG("Confirm Vendor Data UI - pressed "
				  "key %#x\n", key_confirm);
			VbCheckDisplayKey(ctx, key_confirm, data);
			break;
		}
		VbExSleepMs(KEY_DELAY_MS);
	} while (1);
	return VB2_SUCCESS;
}

vb2_error_t vb2_vendor_data_ui(struct vb2_context *ctx)
{
	char data_value[VENDOR_DATA_LENGTH + 1];

	VbScreenData data = {.vendor_data = {data_value, 0}};
	VbDisplayScreen(ctx, VB_COMPLETE_VENDOR_DATA, 0, NULL);

	do {
		uint32_t key_set = VbExKeyboardRead();

		if (vb2_want_shutdown(ctx, key_set)) {
			VB2_DEBUG("Vendor Data UI - shutdown requested!\n");
			return VBERROR_SHUTDOWN_REQUESTED;
		}

		switch (key_set) {
		case 0:
			/* Nothing pressed - do nothing. */
			break;
		case VB_KEY_ESC:
			/* ESC pressed - boot normally */
			VB2_DEBUG("Vendor Data UI - boot normally\n");
			return VB2_SUCCESS;
			break;
		case VB_KEY_ENTER:
			data_value[0] = '\0';
			do {
				/* ENTER pressed -
				   enter vendor data set screen */
				VB2_DEBUG("Vendor Data UI - Enter VD set "
					  "screen\n");
				vb2_error_t ret = vb2_enter_vendor_data_ui(
					ctx, data_value);

				if (ret)
					return ret;

				/* Vendor data was not entered just return */
				if (vendor_data_length(data_value) == 0) {
					return VB2_SUCCESS;
				}

				/* Reset confirmation answer to YES */
				data.vendor_data.selected_index = 0;

				ret = vb2_confirm_vendor_data_ui(
					ctx, data_value, &data);

				if (ret)
					return ret;

				/* Break if vendor data confirmed */
				if (data.vendor_data.selected_index == 0)
					return VB2_SUCCESS;
			} while (1);
			break;
		default:
			break;
        }
	} while (1);
    return VB2_SUCCESS;
}

vb2_error_t vb2_check_diagnostic_key(struct vb2_context *ctx,
				     uint32_t key) {
	if (DIAGNOSTIC_UI && (key == VB_KEY_CTRL('C') || key == VB_KEY_F(12))) {
		VB2_DEBUG("Diagnostic mode requested, rebooting\n");
		vb2_nv_set(ctx, VB2_NV_DIAG_REQUEST, 1);

		return VBERROR_REBOOT_REQUIRED;
	}

	return VB2_SUCCESS;
}

vb2_error_t vb2_diagnostics_ui(struct vb2_context *ctx)
{
	int active = 1;
	int power_button_was_released = 0;
	int power_button_was_pressed = 0;
	vb2_error_t result = VBERROR_REBOOT_REQUIRED;
	int action_confirmed = 0;
	uint64_t start_time_us;

	VbDisplayScreen(ctx, VB_SCREEN_CONFIRM_DIAG, 0, NULL);

	start_time_us = VbExGetTimer();

	/* We'll loop until the user decides what to do */
	do {
		uint32_t key = VbExKeyboardRead();
		/*
		 * VbExIsShutdownRequested() is almost an adequate substitute
		 * for adding a new flag to VbExGetSwitches().  The main
		 * issue is that the former doesn't consult the power button
		 * on detachables, and this function wants to see for itself
		 * that the power button isn't currently pressed.
		 */
		if (VbExGetSwitches(VB_SWITCH_FLAG_PHYS_PRESENCE_PRESSED)) {
			/* Wait for a release before registering a press. */
			if (power_button_was_released)
				power_button_was_pressed = 1;
		} else {
			power_button_was_released = 1;
			if (power_button_was_pressed) {
				VB2_DEBUG("vb2_diagnostics_ui() - power released\n");
				action_confirmed = 1;
				active = 0;
				break;
			}
		}

		/* Check the lid and ignore the power button. */
		if (vb2_want_shutdown(ctx, 0) & ~VB_SHUTDOWN_REQUEST_POWER_BUTTON) {
			VB2_DEBUG("vb2_diagnostics_ui() - shutdown request\n");
			result = VBERROR_SHUTDOWN_REQUESTED;
			active = 0;
			break;
		}

		switch (key) {
		case 0:
			/* Nothing pressed */
			break;
		case VB_KEY_ESC:
			/* Escape pressed - reboot */
			VB2_DEBUG("vb2_diagnostics_ui() - user pressed Esc\n");
			active = 0;
			break;
		default:
			VB2_DEBUG("vb2_diagnostics_ui() - pressed key %#x\n",
				  key);
			VbCheckDisplayKey(ctx, key, NULL);
			break;
		}
		if (VbExGetTimer() - start_time_us >= 30 * VB_USEC_PER_SEC) {
			VB2_DEBUG("vb2_diagnostics_ui() - timeout\n");
			break;
		}
		if (active) {
			VbExSleepMs(KEY_DELAY_MS);
		}
	} while (active);

	VbDisplayScreen(ctx, VB_SCREEN_BLANK, 0, NULL);

	if (action_confirmed) {
		VB2_DEBUG("Diagnostic requested, running\n");

		if (vb2ex_tpm_set_mode(VB2_TPM_MODE_DISABLED) !=
			   VB2_SUCCESS) {
			VB2_DEBUG("Failed to disable TPM\n");
			vb2api_fail(ctx, VB2_RECOVERY_TPM_DISABLE_FAILED, 0);
		} else {
			vb2_try_altfw(ctx, 1, VB_ALTFW_DIAGNOSTIC);
			VB2_DEBUG("Diagnostic failed to run\n");
			/*
			 * Assuming failure was due to bad hash, though
			 * the rom could just be missing or invalid.
			 */
			vb2api_fail(ctx, VB2_RECOVERY_ALTFW_HASH_FAILED, 0);
		}
	}

	return result;
}
