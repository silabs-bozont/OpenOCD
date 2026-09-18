// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Silicon Labs SiWx917 common-flash support                             *
 *                                                                         *
 *   The M4 cannot program common QSPI flash directly.  Programming is     *
 *   performed by streaming an RPS image to the NWP bootloader through     *
 *   its memory-mapped host interface.                                     *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"

#include <helper/binarybuffer.h>
#include <helper/time_support.h>

#define SIWX917_FLASH_BASE			0x08201000
#define SIWX917_FLASH_SIZE			0x005ff000
#define SIWX917_PAGE_SIZE			0x1000

#define SIWX917_TA_RESET			0x22000004
#define SIWX917_HOST_INTF_IN			0x41050034
#define SIWX917_HOST_INTF_OUT			0x4105003c
#define SIWX917_PING_BUFFER			0x00018000
#define SIWX917_PONG_BUFFER			0x00019000

#define SIWX917_HOST_VALID			0xab00
#define SIWX917_BOARD_READY			0xab11
#define SIWX917_BOOT_VALID			0xa000
#define SIWX917_IMAGE_NWP			0x0000
#define SIWX917_IMAGE_M4			0x0100

#define SIWX917_BURN_NWP			'B'
#define SIWX917_UPGRADE_M4			'4'
#define SIWX917_ERASE_COMMON_FLASH		'M'
#define SIWX917_SEND_RPS			'2'
#define SIWX917_PING_AVAILABLE			'I'
#define SIWX917_PONG_AVAILABLE			'O'
#define SIWX917_END_OF_FILE			'E'
#define SIWX917_UPGRADE_SUCCESS			'S'

#define SIWX917_RPS_TYPE_MAX			1
#define SIWX917_RPS_MAGIC			0x900d900d

#define SIWX917_BOARD_READY_TIMEOUT_MS		5000
#define SIWX917_COMMAND_TIMEOUT_MS		5000
#define SIWX917_CHUNK_TIMEOUT_MS		60000
#define SIWX917_FINAL_TIMEOUT_MS		120000
#define SIWX917_ERASE_TIMEOUT_MS		120000

struct siwx917_flash_bank {
	bool probed;
};

static const char *siwx917_nwp_error(uint16_t value)
{
	switch (value & 0xff) {
	case 0xf1:
		return "boot configuration not saved";
	case 0xf2:
		return "boot configuration checksum failed";
	case 0xf3:
		return "invalid bootloader option";
	case 0xcc:
		return "image checksum failed";
	case 0x4c:
		return "image address/checksum invalid";
	case 0x23:
		return "valid firmware not present";
	default:
		return NULL;
	}
}

static int siwx917_read_nwp(struct flash_bank *bank, uint16_t *value)
{
	uint32_t word;
	int retval = target_read_u32(bank->target, SIWX917_HOST_INTF_OUT, &word);

	if (retval == ERROR_OK)
		*value = word;

	return retval;
}

static int siwx917_write_nwp(struct flash_bank *bank, uint16_t value)
{
	return target_write_u32(bank->target, SIWX917_HOST_INTF_IN, value);
}

/* Reports a timeout without logging, so callers that retry can stay quiet. */
static int siwx917_poll_nwp(struct flash_bank *bank, uint16_t expected,
		uint32_t timeout_ms, const char *operation, uint16_t *last)
{
	int64_t end = timeval_ms() + timeout_ms;
	uint16_t value = 0;

	do {
		int retval = siwx917_read_nwp(bank, &value);
		if (retval != ERROR_OK)
			return retval;

		if (value == expected) {
			*last = value;
			return ERROR_OK;
		}

		const char *error = siwx917_nwp_error(value);
		if (error) {
			LOG_ERROR("SiWx917 NWP rejected %s: %s (0x%04x)",
				operation, error, value);
			*last = value;
			return ERROR_FLASH_OPERATION_FAILED;
		}

		alive_sleep(1);
		keep_alive();
	} while (timeval_ms() < end);

	*last = value;
	return ERROR_TIMEOUT_REACHED;
}

static int siwx917_wait_nwp(struct flash_bank *bank, uint16_t expected,
		uint32_t timeout_ms, const char *operation)
{
	uint16_t value = 0;
	int retval = siwx917_poll_nwp(bank, expected, timeout_ms, operation, &value);

	if (retval == ERROR_TIMEOUT_REACHED)
		LOG_ERROR("timed out waiting for SiWx917 NWP during %s "
			"(expected 0x%04x, last 0x%04x)", operation, expected, value);

	return retval;
}

static int siwx917_select_option(struct flash_bank *bank, uint8_t option,
		uint32_t timeout_ms)
{
	/*
	 * AN1497 rsi_select_option() uses image number 0 for NWP firmware and
	 * image number 1 for M4 and common-flash commands.
	 */
	uint16_t image = option == SIWX917_BURN_NWP ?
		SIWX917_IMAGE_NWP : SIWX917_IMAGE_M4;
	uint16_t command = SIWX917_BOOT_VALID | image | option;
	int retval = siwx917_write_nwp(bank, command);
	if (retval != ERROR_OK)
		return retval;

	uint16_t response = SIWX917_HOST_VALID | option;
	if (option == SIWX917_UPGRADE_M4 || option == SIWX917_BURN_NWP)
		response = SIWX917_HOST_VALID | SIWX917_SEND_RPS;

	return siwx917_wait_nwp(bank, response, timeout_ms, "command selection");
}

static int siwx917_prepare(struct flash_bank *bank, uint8_t option,
		uint32_t timeout_ms)
{
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("SiWx917 target must be halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	uint16_t value = 0;
	int retval = siwx917_read_nwp(bank, &value);
	if (retval != ERROR_OK)
		return retval;

	uint16_t expected = SIWX917_HOST_VALID | option;
	if (option == SIWX917_UPGRADE_M4 || option == SIWX917_BURN_NWP)
		expected = SIWX917_HOST_VALID | SIWX917_SEND_RPS;

	/* A retry may re-enter after the command was already accepted. */
	if (value == expected)
		return ERROR_OK;

	/*
	 * Board-ready is posted during a short window after power-on.  Wait for it
	 * because SWD attachment can complete just before the NWP publishes the
	 * mailbox value.
	 */
	retval = siwx917_poll_nwp(bank, SIWX917_BOARD_READY,
		SIWX917_BOARD_READY_TIMEOUT_MS, "board-ready", &value);
	if (retval == ERROR_TIMEOUT_REACHED) {
		LOG_ERROR("SiWx917 NWP is not in its bootloader (mailbox 0x%04x, "
			"expected 0x%04x); power-cycle the device and reattach",
			value, SIWX917_BOARD_READY);
		return retval;
	}
	if (retval != ERROR_OK)
		return retval;

	/* Restart the TA bootloader before submitting the selected command. */
	retval = target_write_u32(bank->target, SIWX917_TA_RESET, 1);
	if (retval != ERROR_OK)
		return retval;

	alive_sleep(50);

	retval = target_write_u32(bank->target, SIWX917_TA_RESET, 0);
	if (retval != ERROR_OK)
		return retval;

	/* Clear the response mailbox before selecting the requested option. */
	retval = target_write_u32(bank->target, SIWX917_HOST_INTF_OUT, 0);
	if (retval != ERROR_OK)
		return retval;

	return siwx917_select_option(bank, option, timeout_ms);
}

static int siwx917_erase(struct flash_bank *bank, unsigned int first,
		unsigned int last)
{
	(void)first;
	(void)last;

	LOG_INFO("erasing SiWx917 common flash (whole-chip operation)");
	int retval = siwx917_prepare(bank, SIWX917_ERASE_COMMON_FLASH,
		SIWX917_ERASE_TIMEOUT_MS);
	if (retval != ERROR_OK)
		return retval;

	for (unsigned int i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_erased = 1;

	return retval;
}

static int siwx917_validate_rps(const uint8_t *buffer, uint32_t count,
		uint8_t *option)
{
	if (count < 64) {
		LOG_ERROR("SiWx917 image is too short to contain an RPS header");
		return ERROR_FLASH_BANK_INVALID;
	}

	uint32_t version = le_to_h_u32(buffer);
	uint32_t magic = le_to_h_u32(buffer + 4);
	uint32_t declared_size = le_to_h_u32(buffer + 8);

	if (version > SIWX917_RPS_TYPE_MAX || magic != SIWX917_RPS_MAGIC) {
		LOG_ERROR("SiWx917 programming requires an M4 or NWP RPS image "
			"(version 0x%08" PRIx32 ", magic 0x%08" PRIx32 ")",
			version, magic);
		return ERROR_FLASH_BANK_INVALID;
	}

	if (declared_size > count || count - declared_size > 64) {
		LOG_ERROR("SiWx917 RPS size mismatch: header=%" PRIu32
			", file=%" PRIu32, declared_size, count);
		return ERROR_FLASH_BANK_INVALID;
	}

	*option = version & 1 ? SIWX917_UPGRADE_M4 : SIWX917_BURN_NWP;

	return ERROR_OK;
}

static int siwx917_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	if (offset != 0) {
		LOG_ERROR("SiWx917 RPS stream must start at flash-bank offset zero");
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	uint8_t option;
	int retval = siwx917_validate_rps(buffer, count, &option);
	if (retval != ERROR_OK)
		return retval;

	/* Leaves the NWP asking for the RPS stream, so no command selection. */
	retval = siwx917_prepare(bank, option,
		SIWX917_COMMAND_TIMEOUT_MS);
	if (retval != ERROR_OK)
		return retval;

	LOG_INFO("SiWx917 %s RPS programming started",
		option == SIWX917_BURN_NWP ? "NWP" : "M4");

	uint8_t *page = malloc(SIWX917_PAGE_SIZE);
	if (!page)
		return ERROR_FAIL;

	uint32_t position = 0;
	bool use_ping = true;
	while (position < count) {
		uint32_t chunk = MIN(count - position, SIWX917_PAGE_SIZE);
		target_addr_t buffer_address = use_ping ?
			SIWX917_PING_BUFFER : SIWX917_PONG_BUFFER;
		uint16_t notify = SIWX917_HOST_VALID | (use_ping ?
			SIWX917_PING_AVAILABLE : SIWX917_PONG_AVAILABLE);
		uint16_t expected = SIWX917_HOST_VALID | (use_ping ?
			SIWX917_PONG_AVAILABLE : SIWX917_PING_AVAILABLE);

		memset(page, 0xff, SIWX917_PAGE_SIZE);
		memcpy(page, buffer + position, chunk);

		retval = target_write_buffer(bank->target, buffer_address,
			SIWX917_PAGE_SIZE, page);
		if (retval != ERROR_OK)
			break;

		retval = siwx917_write_nwp(bank, notify);
		if (retval != ERROR_OK)
			break;

		retval = siwx917_wait_nwp(bank, expected,
			SIWX917_CHUNK_TIMEOUT_MS, "RPS page programming");
		if (retval != ERROR_OK)
			break;

		position += chunk;
		use_ping = !use_ping;
		if ((position % (SIWX917_PAGE_SIZE * 8) == 0) || position == count)
			LOG_INFO("SiWx917 RPS upload: %" PRIu32 "/%" PRIu32 " bytes",
				position, count);
	}

	free(page);
	if (retval != ERROR_OK)
		return retval;

	LOG_INFO("Waiting for image validation...");

	retval = siwx917_write_nwp(bank,
		SIWX917_HOST_VALID | SIWX917_END_OF_FILE);
	if (retval != ERROR_OK)
		return retval;

	LOG_INFO("Waiting for the bootloader to perform the update...");

	retval = siwx917_wait_nwp(bank,
		SIWX917_HOST_VALID | SIWX917_UPGRADE_SUCCESS,
		SIWX917_FINAL_TIMEOUT_MS, "RPS finalization");
	if (retval == ERROR_OK)
		LOG_INFO("SiWx917 RPS programming completed successfully");

	return retval;
}

static int siwx917_probe(struct flash_bank *bank)
{
	struct siwx917_flash_bank *info = bank->driver_priv;
	if (info->probed)
		return ERROR_OK;

	bank->base = SIWX917_FLASH_BASE;
	bank->size = SIWX917_FLASH_SIZE;
	/*
	 * Common flash only supports whole-chip erase through the NWP.  Keep one
	 * logical sector, and leave write alignment at one because the RPS stream
	 * itself is padded to 4 KiB by siwx917_write().
	 */
	bank->num_sectors = 1;
	bank->write_start_alignment = 1;
	bank->write_end_alignment = 1;
	bank->minimal_write_gap = FLASH_WRITE_CONTINUOUS;
	bank->sectors = alloc_block_array(0, bank->size, bank->num_sectors);
	if (!bank->sectors)
		return ERROR_FAIL;

	info->probed = true;
	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(siwx917_flash_bank_command)
{
	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct siwx917_flash_bank *info = calloc(1, sizeof(*info));
	if (!info)
		return ERROR_FAIL;

	bank->driver_priv = info;
	return ERROR_OK;
}

static int siwx917_info(struct flash_bank *bank,
		struct command_invocation *cmd)
{
	(void)bank;
	command_print_sameline(cmd,
		"SiWx917 common QSPI flash via NWP RPS bootloader");
	return ERROR_OK;
}

const struct flash_driver siwx917_flash = {
	.name = "siwx917",
	.flash_bank_command = siwx917_flash_bank_command,
	.erase = siwx917_erase,
	.write = siwx917_write,
	.read = default_flash_read,
	.probe = siwx917_probe,
	.auto_probe = siwx917_probe,
	.erase_check = default_flash_blank_check,
	.info = siwx917_info,
	.free_driver_priv = default_flash_free_driver_priv,
};
