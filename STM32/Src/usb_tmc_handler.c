#include "usb_tmc.h"
#include "usb_tmc_commands.h"
#include "version.h"
#include "str_util.h"
#include "uuid.h"
#include "pl.h"
#include "pl_flash.h"
#include "main.h"
#include <string.h>
#include <stdbool.h>

#define IDN_VENDOR_PRODUCT VENDOR "," PRODUCT ","

static char idn_buff[] = IDN_VENDOR_PRODUCT "****************," VERSION;
static const char* idn_ptr;

// Initialize device identification string
static void idn_init(void)
{
	char* sn_buff = idn_buff + STRZ_LEN(IDN_VENDOR_PRODUCT);
	u32_to_hex(UUID[0] + UUID[2], sn_buff);
	u32_to_hex(UUID[1], sn_buff + 8);
	idn_ptr = idn_buff;
}

//
// TMC request context
//
bool     tmc_pending;      // have pending request
bool     tmc_reply_rdy;    // ready to reply
bool     tmc_reply_req;    // reply requested
unsigned tmc_reply_len;    // the reply length in bytes
unsigned tmc_reply_max_len;// the maximum reply length reported by host
uint8_t  tmc_reply_tag;    // the reply tag received in host message
unsigned tmc_reply_cnt;    // total number of replies so far

// The asynchronous processing handler
// Will be called in the main loop context
// We need it since USB messages are delivered in ISR context
typedef void (*tmc_handler_t)(void);
tmc_handler_t tmc_handler;

static inline void tmc_ready_to_reply(void)
{
	tmc_reply_rdy = true;
}

static inline void tmc_schedule_reply(unsigned len)
{
	tmc_pending = true;
	tmc_reply_len = len;
	tmc_ready_to_reply();
}

static void tmc_schedule_reply_buff(uint8_t const* buff, unsigned len)
{
	memcpy(USB_TMC_TxDataBuffer(), buff, len);
	tmc_schedule_reply(len);
}

static inline void tmc_schedule_handler(tmc_handler_t h)
{
	tmc_pending = true;
	tmc_handler = h;
}

// *IDN? async handler
static void tmc_idn_handler(void)
{
	if (!idn_ptr)
		idn_init();
	tmc_schedule_reply_buff((uint8_t const*)idn_ptr, STRZ_LEN(idn_buff));
}

// Error counters for debug
unsigned tmc_wr_ignored;
unsigned tmc_rd_empty;
unsigned tmc_rd_truncated;
unsigned tmc_overrun;

// Standard commands handler
static void tmc_rx_std_command(uint8_t const* pbuf, unsigned len)
{
	if (PREFIX_MATCHED(CMD_IDN, pbuf, len)) {
		tmc_schedule_handler(tmc_idn_handler);
		return;
	}
	// unhandled command
	++tmc_wr_ignored;
}

// Flash transaction size
static unsigned tmc_pl_flash_tx_sz;
// Flash wait timeout
static unsigned tmc_pl_flash_wait;

// FLASH:RD/WR async handler
static void tmc_pl_flash_tx_handler(void)
{
	pl_flash_tx(USB_TMC_TxDataBuffer(), tmc_pl_flash_tx_sz);
	tmc_ready_to_reply();
}

// FLASH:WAit async handler
static void tmc_pl_flash_wait_handler(void)
{
	USB_TMC_TxDataBuffer()[0] = pl_flash_wait(tmc_pl_flash_wait);
	tmc_schedule_reply(1);
}

// FLASH:PRog async handler
static void tmc_pl_flash_prog_handler(void)
{
	uint8_t wr_en_cmd = 0x06;
	uint8_t *buff = USB_TMC_TxDataBuffer();
	uint8_t status = pl_flash_wait(tmc_pl_flash_wait);
	if (!(status & FLASH_STATUS_BUSY)) {
		pl_flash_tx(&wr_en_cmd, 1);
		pl_flash_tx(buff, tmc_pl_flash_tx_sz);
	}
	buff[0] = status;
	tmc_schedule_reply(1);
}

// FLASH:RD/WR command handler
static void tmc_pl_flash_tx(uint8_t const* pbuf, unsigned len, unsigned rd_len, bool rd_reply)
{
	if (rd_reply && len + rd_len > USB_TMC_TX_MAX_DATA_SZ) {
		// unhandled command
		++tmc_wr_ignored;
		return;
	}
	memcpy(USB_TMC_TxDataBuffer(), pbuf, len);
	tmc_pl_flash_tx_sz = len + rd_len;
	tmc_reply_len = rd_reply ? tmc_pl_flash_tx_sz : 0;
	tmc_schedule_handler(tmc_pl_flash_tx_handler);
}

// FLASH:PRog command handler
static void tmc_pl_flash_prog(uint8_t const* pbuf, unsigned len, unsigned wait)
{
	memcpy(USB_TMC_TxDataBuffer(), pbuf, len);
	tmc_pl_flash_tx_sz = len;
	tmc_pl_flash_wait = wait;
	tmc_schedule_handler(tmc_pl_flash_prog_handler);
}

// PL:FLASH commands handler
static void tmc_rx_pl_flash_sub_command(uint8_t const* pbuf, unsigned len)
{
	if (pl_status != pl_inactive) {
		// Wrong FPGA status
		++tmc_wr_ignored;
		return;
	}
	unsigned skip, skip_arg, arg;
	if (PREFIX_MATCHED(CMD_WR, pbuf, len)) {
		tmc_pl_flash_tx(pbuf + STRZ_LEN(CMD_WR), len - STRZ_LEN(CMD_WR), 0, false);
		return;
	}
	if (PREFIX_MATCHED(CMD_RD, pbuf, len)
		&& (skip = skip_through('#', pbuf, len))
		&& (skip_arg = scan_u(pbuf + skip, len - skip, &arg))
		&& len > skip + skip_arg && pbuf[skip + skip_arg] == '#'
	) {
		tmc_pl_flash_tx(pbuf + skip + skip_arg + 1, len - skip - skip_arg - 1, arg, true);
		return;
	}
	if (PREFIX_MATCHED(CMD_WAIT, pbuf, len)
		&& (skip = skip_through('#', pbuf, len))
		&& scan_u(pbuf + skip, len - skip, &arg)
	) {
		tmc_pl_flash_wait = arg;
		tmc_schedule_handler(tmc_pl_flash_wait_handler);
		return;
	}
	if (PREFIX_MATCHED(CMD_PROG, pbuf, len)
		&& (skip = skip_through('#', pbuf, len))
		&& (skip_arg = scan_u(pbuf + skip, len - skip, &arg))
		&& len > skip + skip_arg && pbuf[skip + skip_arg] == '#'
	) {
		tmc_pl_flash_prog(pbuf + skip + skip_arg + 1, len - skip - skip_arg - 1, arg);
		return;
	}
	// unhandled command
	++tmc_wr_ignored;
}

// PL:ACTIVE? async handler
static inline void tmc_pl_report_status(void)
{
	uint8_t resp = '0' + pl_status;
	tmc_schedule_reply_buff(&resp, 1);
}

// PL:TX async handler
static void tmc_pl_tx_handler(void)
{
	if (!pl_tx(USB_TMC_TxDataBuffer(), tmc_reply_len))
		tmc_reply_len = 0;
	tmc_ready_to_reply();
}

// PL:TX command handler
static void tmc_pl_tx(uint8_t const* pbuf, unsigned len)
{
	memcpy(USB_TMC_TxDataBuffer(), pbuf, len);
	tmc_reply_len = len;
	tmc_schedule_handler(tmc_pl_tx_handler);
}

// DCMI data pull request parameters
unsigned tmc_pull_data_len;
unsigned tmc_pull_req_len;

static void pl_pull_done(bool success)
{
	if (!success) {
		pl_stop_pull();
		tmc_reply_len = 0;
	}
	tmc_ready_to_reply();
}

// PL:PULL async completion handler
static void tmc_pl_pull_complete_handler(void)
{
	pl_pull_status_t sta = pl_get_pull_status();
	if (sta == pl_pull_busy) {
		tmc_schedule_handler(tmc_pl_pull_complete_handler);
		return;
	}
	pl_pull_done(sta == pl_pull_ready);
}

// PL:PULL async handler
static void tmc_pl_pull_handler(void)
{
	uint8_t* buff = USB_TMC_TxDataBuffer();
	unsigned align = 3 & (unsigned)(buff + tmc_pull_req_len);
	if (align)
		align = 4 - align;
	tmc_reply_len = tmc_pull_req_len + align + tmc_pull_data_len;
	if (!pl_start_pull(buff + tmc_pull_req_len + align, tmc_pull_data_len)) {
		tmc_reply_len = 0;
		tmc_ready_to_reply();
		return;
	}
	if (!pl_tx(buff, tmc_pull_req_len)) {
		pl_pull_done(false);
		return;
	}
	tmc_schedule_handler(tmc_pl_pull_complete_handler);
}

// PL:PULL command handler
static void tmc_pl_pull(uint8_t const* pbuf, unsigned len, unsigned pull_len)
{
	pull_len *= 4; // It is specified in 32 bit words
	// Make sure data fit in the buffer even taking alignment into account
	if (len + 3 + pull_len > USB_TMC_TX_MAX_DATA_SZ) {
		// unhandled command
		++tmc_wr_ignored;
		return;
	}
	memcpy(USB_TMC_TxDataBuffer(), pbuf, len);
	tmc_pull_req_len = len;
	tmc_pull_data_len = pull_len;
	tmc_schedule_handler(tmc_pl_pull_handler);
}

// PL commands handler
static void tmc_rx_pl_sub_command(uint8_t const* pbuf, unsigned len)
{
	unsigned skip, skip_arg, arg;
	if (PREFIX_MATCHED(CMD_ACTIVE, pbuf, len)) {
		if (len > STRZ_LEN(CMD_ACTIVE) && pbuf[STRZ_LEN(CMD_ACTIVE)] == '?') {
			tmc_pl_report_status();
			return;
		}
		if ((skip = skip_through('#', pbuf, len)) && scan_u(pbuf + skip, len - skip, &arg)) {
			pl_enable(arg != 0);
			return;
		}
	}

	if (PREFIX_MATCHED(CMD_TX, pbuf, len)) {
		tmc_pl_tx(pbuf + STRZ_LEN(CMD_TX), len - STRZ_LEN(CMD_TX));
		return;
	}

	if (PREFIX_MATCHED(CMD_PULL, pbuf, len)
		&& (skip = skip_through('#', pbuf, len))
		&& (skip_arg = scan_u(pbuf + skip, len - skip, &arg))
		&& len > skip + skip_arg && pbuf[skip + skip_arg] == '#'
	) {
		tmc_pl_pull(pbuf + skip + skip_arg + 1, len - skip - skip_arg - 1, arg);
		return;
	}

	if (PREFIX_MATCHED(CMD_FLASH, pbuf, len) && (skip = skip_through(':', pbuf, len))) {
		tmc_rx_pl_flash_sub_command(pbuf + skip, len - skip);
		return;
	}
	// unhandled command
	++tmc_wr_ignored;
}

// TEST:ECHO command handler
static void tmc_rx_test_echo(uint8_t const* pbuf, unsigned len)
{
	if (len > USB_TMC_TX_MAX_DATA_SZ)
		len = USB_TMC_TX_MAX_DATA_SZ;
	tmc_schedule_reply_buff(pbuf, len);
}

// TEST commands handler
static void tmc_rx_test_sub_command(uint8_t const* pbuf, unsigned len)
{
	if (PREFIX_MATCHED(CMD_ECHO, pbuf, len)) {
		tmc_rx_test_echo(pbuf + STRZ_LEN(CMD_ECHO), len - STRZ_LEN(CMD_ECHO));
		return;
	}
	// unhandled command
	++tmc_wr_ignored;
}

// Device-specific commands handler
static void tmc_rx_dev_command(uint8_t const* pbuf, unsigned len)
{
	unsigned skip;
	if (PREFIX_MATCHED(CMD_PL, pbuf, len) && (skip = skip_through(':', pbuf, len))) {
		tmc_rx_pl_sub_command(pbuf + skip, len - skip);
		return;
	}
	if (PREFIX_MATCHED(CMD_TEST, pbuf, len) && (skip = skip_through(':', pbuf, len))) {
		tmc_rx_test_sub_command(pbuf + skip, len - skip);
		return;
	}
	// unhandled command
	++tmc_wr_ignored;
}

// Handle message received over USB. Called in USB ISR context.
void USB_TMC_Receive(uint8_t const* pbuf, unsigned len)
{
	if (tmc_pending) {
		++tmc_overrun;
	}
	if (!len) {
		++tmc_wr_ignored;
		return;
	}
	if (*pbuf == '*') {
		--len;
		++pbuf;
		tmc_rx_std_command(pbuf, len);
		return;
	}
	if (*pbuf == ':') {
		--len;
		++pbuf;
	}
	tmc_rx_dev_command(pbuf, len);
}

// Send reply over USB
static void tmc_reply(void)
{
	unsigned len = tmc_reply_len;
	if (len > tmc_reply_max_len) {
		len = tmc_reply_max_len;
		++tmc_rd_truncated;
	}
	tmc_reply_rdy = false;
	tmc_reply_req = false;
	tmc_pending = false;
	USB_TMC_Reply(len, tmc_reply_tag);
	++tmc_reply_cnt;
}

// Input response requested. Called from USB ISR context.
void USB_TMC_RequestResponse(uint8_t tag, unsigned max_len)
{
	if (!tmc_pending) {
		USB_TMC_Reply(0, tag);
		++tmc_rd_empty;
		return;
	}
	tmc_reply_tag = tag;
	tmc_reply_max_len = max_len;
	tmc_reply_req = true;
}

// Asynchronous processing routine.
// Called periodically in main() loop.
void USB_TMC_Process(void)
{
	tmc_handler_t h = tmc_handler;
	if (h) {
		tmc_handler = 0;
		h();
	}
	if (tmc_reply_rdy && tmc_reply_req)
		tmc_reply();
}

extern uint8_t USBD_HS_DeviceDesc[];

// Initialize device data
// The TMC implemented as patched CDC auto-generated by CubeMX.
// This function applies patches to device descriptor that cannot be applied to the auto-generated source.
void USB_TMC_init(void)
{
	USBD_HS_DeviceDesc[3] = 0x00; /*bDeviceClass: This is an Interface Class Defined Device*/
	USBD_HS_DeviceDesc[4] = 0x00; /*bDeviceSubClass*/
}
