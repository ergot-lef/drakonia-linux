/*
 * Copyright © 2026 ergot-lef
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Driver for the Sharkoon Drakonia (Holtek, USB 04d9:a067).
 *
 * The protocol was reverse-engineered from USB captures of the vendor
 * software. See https://github.com/ergot-lef/drakonia-linux for the full
 * description. In short:
 *
 * - Settings are 8-byte feature reports with ID 0x07 on the interface that
 *   exposes that report. Bytes 1..7 are scrambled (see drakonia_scramble()).
 * - The button table (64 bytes) and the macro area (1536 bytes) are sent as
 *   8-byte output reports (no report ID) on the vendor-only interface.
 *   Byte 0 of each chunk is sent in clear, bytes 1..7 are scrambled.
 * - The device cannot be read back, so this driver is write-only and starts
 *   from the factory configuration.
 */

#include "config.h"

#include <errno.h>
#include <linux/hidraw.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "libratbag-hidraw.h"
#include "libratbag-private.h"
#include "libratbag-util.h"
#include "shared-macro.h"

#define DRAKONIA_REPORT_ID		0x07
#define DRAKONIA_REPORT_SIZE		8
#define DRAKONIA_DELAY_MS		16

#define DRAKONIA_BLOCK_HIDRAW		1

#define DRAKONIA_NUM_PROFILES		1
#define DRAKONIA_NUM_RESOLUTIONS	1
#define DRAKONIA_NUM_BUTTONS		11
#define DRAKONIA_NUM_LEDS		1

#define DRAKONIA_BUTTON_TABLE_SIZE	64
#define DRAKONIA_MACRO_SLOT_SIZE	96
#define DRAKONIA_MACRO_SLOTS		16
#define DRAKONIA_MACRO_AREA_SIZE	(DRAKONIA_MACRO_SLOT_SIZE * DRAKONIA_MACRO_SLOTS)
/* two header bytes, then (flag, usage) pairs */
#define DRAKONIA_MACRO_MAX_EVENTS	((DRAKONIA_MACRO_SLOT_SIZE - 2) / 2)

#define DRAKONIA_MACRO_PRESS		0x00
#define DRAKONIA_MACRO_RELEASE		0x80
/* inside a macro, 0xf0 is the left mouse button */
#define DRAKONIA_MACRO_LEFT_BUTTON	0xf0

enum drakonia_command {
	DRAKONIA_CMD_REPORT_RATE = 0x01,
	DRAKONIA_CMD_BUTTONS = 0x04,
	DRAKONIA_CMD_MACROS = 0x05,
	DRAKONIA_CMD_DPI = 0x09,
	DRAKONIA_CMD_COLOR = 0x0a,
	DRAKONIA_CMD_LED_MODE = 0x0b,
	DRAKONIA_CMD_LED_BRIGHTNESS = 0x0c,
};

enum drakonia_led_mode {
	DRAKONIA_LED_STEADY = 1,
	DRAKONIA_LED_PULSE_SLOW = 2,
	DRAKONIA_LED_PULSE_MEDIUM = 3,
	DRAKONIA_LED_PULSE_FAST = 4,
};

enum drakonia_led_brightness {
	DRAKONIA_LED_OFF = 1,
	DRAKONIA_LED_LOW = 2,
	DRAKONIA_LED_MEDIUM = 3,
	DRAKONIA_LED_HIGH = 4,
};

static const unsigned int drakonia_dpis[] = { 500, 1000, 1500, 2000, 3000, 4000, 5000 };
static const unsigned int drakonia_report_rates[] = { 125, 250, 500, 1000 };

/* Factory button table, in the order of the physical buttons:
 * left, right, middle, forward, back, wheel tilt left, wheel tilt right,
 * DPI+, DPI-, right side button (LED color cycle), left side button
 * (double click, macro slot 10). */
static const uint8_t drakonia_factory_buttons[DRAKONIA_NUM_BUTTONS][4] = {
	{ 0x00, 0xf0, 0x00, 0x00 },
	{ 0x00, 0xf1, 0x00, 0x00 },
	{ 0x00, 0xf2, 0x00, 0x00 },
	{ 0x00, 0xf4, 0x00, 0x00 },
	{ 0x00, 0xf3, 0x00, 0x00 },
	{ 0x04, 0x00, 0x00, 0xf3 },
	{ 0x04, 0x00, 0x00, 0xf4 },
	{ 0x71, 0x01, 0x00, 0x00 },
	{ 0x71, 0x02, 0x00, 0x00 },
	{ 0x72, 0x01, 0x00, 0x00 },
	{ 0x53, 0x00, 0x0a, 0x0a },
};

static const struct ratbag_button_action drakonia_factory_actions[DRAKONIA_NUM_BUTTONS] = {
	BUTTON_ACTION_BUTTON(1),
	BUTTON_ACTION_BUTTON(2),
	BUTTON_ACTION_BUTTON(3),
	BUTTON_ACTION_BUTTON(5),
	BUTTON_ACTION_BUTTON(4),
	BUTTON_ACTION_SPECIAL(RATBAG_BUTTON_ACTION_SPECIAL_WHEEL_LEFT),
	BUTTON_ACTION_SPECIAL(RATBAG_BUTTON_ACTION_SPECIAL_WHEEL_RIGHT),
	BUTTON_ACTION_SPECIAL(RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_UP),
	BUTTON_ACTION_SPECIAL(RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_DOWN),
	BUTTON_ACTION_UNKNOWN, /* LED color cycle, no libratbag equivalent */
	BUTTON_ACTION_SPECIAL(RATBAG_BUTTON_ACTION_SPECIAL_DOUBLECLICK),
};

/* ------------------------------------------------------------------ */
/* Scrambling: inverse of the vendor's EncodeSecrecy_V11.              */

static void
drakonia_scramble(uint8_t data[7])
{
	static const uint8_t key[7] = { 'o', 'N', 'g', 't', 'E', 'n', 'g' };
	uint64_t v = 0;
	uint8_t x[7];

	for (size_t i = 0; i < 7; i++) {
		uint8_t k = (uint8_t)((key[i] << 4) | (key[i] >> 4));
		v = (v << 8) | (uint8_t)(data[i] + k);
	}

	/* rotate the 56 bits left by 3 */
	v = ((v << 3) | (v >> 53)) & ((1ULL << 56) - 1);

	for (size_t i = 0; i < 7; i++)
		x[i] = (uint8_t)(v >> (8 * (6 - i)));

	/* swap 0<->3, 2<->6, 4<->5 */
	data[0] = x[3]; data[3] = x[0];
	data[1] = x[1];
	data[2] = x[6]; data[6] = x[2];
	data[4] = x[5]; data[5] = x[4];
}

/* ------------------------------------------------------------------ */
/* Encoding of the button table and the macro area (no I/O).           */

static int
drakonia_button_code(unsigned int button, uint8_t code[4])
{
	if (button < 1 || button > 5)
		return -EINVAL;

	code[0] = 0x00;
	code[1] = (uint8_t)(0xf0 + button - 1);
	code[2] = 0x00;
	code[3] = 0x00;
	return 0;
}

static int
drakonia_special_code(enum ratbag_button_action_special special, uint8_t code[4])
{
	static const struct {
		enum ratbag_button_action_special special;
		uint8_t code[4];
	} map[] = {
		{ RATBAG_BUTTON_ACTION_SPECIAL_WHEEL_LEFT,      { 0x04, 0x00, 0x00, 0xf3 } },
		{ RATBAG_BUTTON_ACTION_SPECIAL_WHEEL_RIGHT,     { 0x04, 0x00, 0x00, 0xf4 } },
		{ RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_UP,   { 0x71, 0x01, 0x00, 0x00 } },
		{ RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_DOWN, { 0x71, 0x02, 0x00, 0x00 } },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(map); i++) {
		if (map[i].special == special) {
			memcpy(code, map[i].code, 4);
			return 0;
		}
	}
	return -EINVAL;
}

/* Appends one (flag, usage) pair to a macro slot. */
static int
drakonia_macro_add(uint8_t *slot, unsigned int *nevents, uint8_t flag, uint8_t usage)
{
	if (*nevents >= DRAKONIA_MACRO_MAX_EVENTS)
		return -E2BIG;

	slot[2 + 2 * *nevents] = flag;
	slot[3 + 2 * *nevents] = usage;
	(*nevents)++;
	return 0;
}

static int
drakonia_macro_from_events(const struct ratbag_macro *macro, uint8_t *slot,
			   unsigned int *nevents)
{
	int rc;

	for (size_t i = 0; i < MAX_MACRO_EVENTS; i++) {
		const struct ratbag_macro_event *ev = &macro->events[i];
		uint8_t usage;

		if (ev->type == RATBAG_MACRO_EVENT_NONE ||
		    ev->type == RATBAG_MACRO_EVENT_INVALID)
			break;

		/* The delay encoding is unknown, delays are dropped. */
		if (ev->type == RATBAG_MACRO_EVENT_WAIT)
			continue;

		usage = (uint8_t)ratbag_hidraw_get_keyboard_usage_from_keycode(NULL, ev->event.key);
		if (usage == 0)
			return -EINVAL;

		rc = drakonia_macro_add(slot, nevents,
					ev->type == RATBAG_MACRO_EVENT_KEY_PRESSED ?
					DRAKONIA_MACRO_PRESS : DRAKONIA_MACRO_RELEASE,
					usage);
		if (rc)
			return rc;
	}
	return 0;
}

/*
 * Builds the 64-byte button table and the 1536-byte macro area.
 * The macro of button n (0-based) always lives in macro slot n, like the
 * vendor software does. raw[] holds the codes to keep for buttons whose
 * action libratbag cannot express (RATBAG_BUTTON_ACTION_TYPE_UNKNOWN).
 * Returns 0, or a negative errno with *bad_button set.
 */
static int
drakonia_build_config(const struct ratbag_button_action actions[DRAKONIA_NUM_BUTTONS],
		      const uint8_t raw[DRAKONIA_NUM_BUTTONS][4],
		      uint8_t table[DRAKONIA_BUTTON_TABLE_SIZE],
		      uint8_t macros[DRAKONIA_MACRO_AREA_SIZE],
		      unsigned int *bad_button)
{
	memset(table, 0, DRAKONIA_BUTTON_TABLE_SIZE);
	memset(macros, 0, DRAKONIA_MACRO_AREA_SIZE);

	for (unsigned int i = 0; i < DRAKONIA_NUM_BUTTONS; i++) {
		const struct ratbag_button_action *action = &actions[i];
		uint8_t *code = &table[i * 4];
		uint8_t *slot = &macros[i * DRAKONIA_MACRO_SLOT_SIZE];
		unsigned int nevents = 0;
		bool is_macro = false;
		int rc = 0;

		*bad_button = i;

		switch (action->type) {
		case RATBAG_BUTTON_ACTION_TYPE_NONE:
			break;
		case RATBAG_BUTTON_ACTION_TYPE_BUTTON:
			rc = drakonia_button_code(action->action.button, code);
			break;
		case RATBAG_BUTTON_ACTION_TYPE_SPECIAL:
			if (action->action.special == RATBAG_BUTTON_ACTION_SPECIAL_DOUBLECLICK) {
				is_macro = true;
				for (int n = 0; n < 2 && rc == 0; n++) {
					rc = drakonia_macro_add(slot, &nevents, DRAKONIA_MACRO_PRESS,
								DRAKONIA_MACRO_LEFT_BUTTON);
					if (rc == 0)
						rc = drakonia_macro_add(slot, &nevents, DRAKONIA_MACRO_RELEASE,
									DRAKONIA_MACRO_LEFT_BUTTON);
				}
			} else {
				rc = drakonia_special_code(action->action.special, code);
			}
			break;
		case RATBAG_BUTTON_ACTION_TYPE_KEY: {
			uint8_t usage = (uint8_t)ratbag_hidraw_get_keyboard_usage_from_keycode(NULL, action->action.key);

			if (usage == 0) {
				rc = -EINVAL;
				break;
			}
			is_macro = true;
			rc = drakonia_macro_add(slot, &nevents, DRAKONIA_MACRO_PRESS, usage);
			if (rc == 0)
				rc = drakonia_macro_add(slot, &nevents, DRAKONIA_MACRO_RELEASE, usage);
			break;
		}
		case RATBAG_BUTTON_ACTION_TYPE_MACRO:
			if (!action->macro) {
				rc = -EINVAL;
				break;
			}
			is_macro = true;
			rc = drakonia_macro_from_events(action->macro, slot, &nevents);
			break;
		case RATBAG_BUTTON_ACTION_TYPE_UNKNOWN:
		default:
			memcpy(code, raw[i], 4);
			break;
		}

		if (rc)
			return rc;

		if (is_macro) {
			slot[1] = 0x01;
			code[0] = 0x53;
			code[1] = 0x00;
			code[2] = (uint8_t)i;
			code[3] = (uint8_t)(2 * nevents + 2);
		}
	}

	return 0;
}

static uint8_t
drakonia_encode_dpi(unsigned int dpi)
{
	for (size_t i = 0; i < ARRAY_LENGTH(drakonia_dpis); i++)
		if (drakonia_dpis[i] == dpi)
			return (uint8_t)(i + 1);
	return 3; /* 1500 dpi */
}

static uint8_t
drakonia_encode_report_rate(unsigned int hz)
{
	/* the device takes the polling interval in ms */
	if (hz == 0 || hz > 1000)
		return 1;
	return (uint8_t)(1000 / hz);
}

static void
drakonia_encode_led(const struct ratbag_led *led, uint8_t *mode, uint8_t *brightness)
{
	*mode = DRAKONIA_LED_STEADY;

	switch (led->mode) {
	case RATBAG_LED_OFF:
		*brightness = DRAKONIA_LED_OFF;
		return;
	case RATBAG_LED_BREATHING:
		if (led->ms <= 1500)
			*mode = DRAKONIA_LED_PULSE_FAST;
		else if (led->ms <= 3000)
			*mode = DRAKONIA_LED_PULSE_MEDIUM;
		else
			*mode = DRAKONIA_LED_PULSE_SLOW;
		break;
	default:
		break;
	}

	if (led->brightness == 0)
		*brightness = DRAKONIA_LED_OFF;
	else if (led->brightness <= 85)
		*brightness = DRAKONIA_LED_LOW;
	else if (led->brightness <= 170)
		*brightness = DRAKONIA_LED_MEDIUM;
	else
		*brightness = DRAKONIA_LED_HIGH;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                  */

static int
drakonia_send_command(struct ratbag_device *device, const uint8_t *args, size_t len)
{
	uint8_t buf[DRAKONIA_REPORT_SIZE] = { DRAKONIA_REPORT_ID };
	int rc;

	assert(len <= DRAKONIA_REPORT_SIZE - 1);
	memcpy(&buf[1], args, len);
	drakonia_scramble(&buf[1]);

	rc = ratbag_hidraw_set_feature_report(device, DRAKONIA_REPORT_ID, buf, sizeof(buf));
	msleep(DRAKONIA_DELAY_MS);
	if (rc < 0)
		return rc;
	if (rc != (int)sizeof(buf))
		return -EIO;
	return 0;
}

#define drakonia_command(device_, ...) \
	drakonia_send_command(device_, (const uint8_t[]){ __VA_ARGS__ }, \
			      sizeof((const uint8_t[]){ __VA_ARGS__ }))

static int
drakonia_send_block(struct ratbag_device *device, const uint8_t *data, size_t len)
{
	int fd = device->hidraw[DRAKONIA_BLOCK_HIDRAW].fd;

	if (fd < 0)
		return -ENODEV;

	for (size_t off = 0; off < len; off += 8) {
		/* report number 0 (no report ID), then 8 bytes */
		uint8_t pkt[9] = { 0 };
		ssize_t rc;

		memcpy(&pkt[1], &data[off], 8);
		drakonia_scramble(&pkt[2]);

		rc = write(fd, pkt, sizeof(pkt));
		msleep(DRAKONIA_DELAY_MS);
		if (rc < 0)
			return -errno;
		if (rc != (ssize_t)sizeof(pkt))
			return -EIO;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Probing                                                              */

static int
drakonia_test_hidraw(struct ratbag_device *device)
{
	return ratbag_hidraw_has_report(device, DRAKONIA_REPORT_ID);
}

/* The block interface has a vendor usage page (0xff00) and no report IDs. */
static bool
drakonia_is_block_interface(int fd)
{
	struct hidraw_report_descriptor desc = { 0 };
	int size = 0;
	bool vendor = false;

	if (ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0 || size <= 0)
		return false;

	desc.size = (uint32_t)size;
	if (ioctl(fd, HIDIOCGRDESC, &desc) < 0)
		return false;

	for (uint32_t i = 0; i < desc.size;) {
		uint8_t prefix = desc.value[i];
		uint32_t dlen;
		uint32_t value = 0;

		if (prefix == 0xfe) { /* long item */
			if (i + 1 >= desc.size)
				break;
			i += 3 + desc.value[i + 1];
			continue;
		}

		dlen = (prefix & 0x03) == 3 ? 4 : (prefix & 0x03);
		if (i + 1 + dlen > desc.size)
			break;
		for (uint32_t j = 0; j < dlen; j++)
			value |= (uint32_t)desc.value[i + 1 + j] << (8 * j);

		switch (prefix & 0xfc) {
		case 0x84: /* Report ID */
			return false;
		case 0x04: /* Usage Page */
			if (value == 0xff00)
				vendor = true;
			break;
		default:
			break;
		}
		i += 1 + dlen;
	}

	return vendor;
}

static int
drakonia_open_block_interface(struct ratbag_device *device)
{
	for (int endpoint = 1; endpoint < 8; endpoint++) {
		if (ratbag_open_hidraw_index(device, endpoint, DRAKONIA_BLOCK_HIDRAW))
			continue;

		if (drakonia_is_block_interface(device->hidraw[DRAKONIA_BLOCK_HIDRAW].fd))
			return 0;

		ratbag_close_hidraw_index(device, DRAKONIA_BLOCK_HIDRAW);
	}
	return -ENODEV;
}

struct drakonia_data {
	uint8_t raw_buttons[DRAKONIA_NUM_BUTTONS][4];
};

static int
drakonia_probe(struct ratbag_device *device)
{
	struct ratbag_profile *profile;
	struct ratbag_resolution *resolution;
	struct ratbag_button *button;
	struct ratbag_led *led;
	struct drakonia_data *drv_data;
	int rc;

	rc = ratbag_find_hidraw(device, drakonia_test_hidraw);
	if (rc)
		return rc;

	rc = drakonia_open_block_interface(device);
	if (rc) {
		log_error(device->ratbag, "%s: vendor interface not found\n", device->name);
		ratbag_close_hidraw(device);
		return rc;
	}

	drv_data = zalloc(sizeof(*drv_data));
	memcpy(drv_data->raw_buttons, drakonia_factory_buttons, sizeof(drv_data->raw_buttons));
	ratbag_set_drv_data(device, drv_data);

	ratbag_device_init_profiles(device,
				    DRAKONIA_NUM_PROFILES,
				    DRAKONIA_NUM_RESOLUTIONS,
				    DRAKONIA_NUM_BUTTONS,
				    DRAKONIA_NUM_LEDS);

	/* The device cannot be read: start from the factory configuration. */
	ratbag_device_for_each_profile(device, profile) {
		profile->is_active = true;
		ratbag_profile_set_cap(profile, RATBAG_PROFILE_CAP_WRITE_ONLY);
		ratbag_profile_set_report_rate_list(profile, drakonia_report_rates,
						    ARRAY_LENGTH(drakonia_report_rates));
		profile->hz = 500;

		ratbag_profile_for_each_resolution(profile, resolution) {
			ratbag_resolution_set_dpi_list(resolution, drakonia_dpis,
						       ARRAY_LENGTH(drakonia_dpis));
			ratbag_resolution_set_resolution(resolution, 1500, 1500);
			resolution->is_active = true;
			resolution->is_default = true;
		}

		ratbag_profile_for_each_button(profile, button) {
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_NONE);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_BUTTON);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_SPECIAL);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_KEY);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_MACRO);
			ratbag_button_set_action(button, &drakonia_factory_actions[button->index]);
		}

		ratbag_profile_for_each_led(profile, led) {
			led->colordepth = RATBAG_LED_COLORDEPTH_RGB_888;
			ratbag_led_set_mode_capability(led, RATBAG_LED_OFF);
			ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
			ratbag_led_set_mode_capability(led, RATBAG_LED_BREATHING);
			led->mode = RATBAG_LED_BREATHING;
			led->ms = 1000;
			led->brightness = 255;
			led->color.red = 0x00;
			led->color.green = 0xff;
			led->color.blue = 0x00;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Commit                                                               */

static int
drakonia_write_buttons(struct ratbag_device *device, struct ratbag_profile *profile)
{
	struct drakonia_data *drv_data = ratbag_get_drv_data(device);
	struct ratbag_button_action actions[DRAKONIA_NUM_BUTTONS] = { 0 };
	struct ratbag_button *button;
	uint8_t table[DRAKONIA_BUTTON_TABLE_SIZE];
	uint8_t macros[DRAKONIA_MACRO_AREA_SIZE];
	unsigned int bad_button = 0;
	int rc;

	ratbag_profile_for_each_button(profile, button)
		actions[button->index] = button->action;

	rc = drakonia_build_config(actions, (const uint8_t (*)[4])drv_data->raw_buttons,
				   table, macros, &bad_button);
	if (rc) {
		log_error(device->ratbag, "%s: unsupported action on button %u (%s)\n",
			  device->name, bad_button, strerror(-rc));
		return rc;
	}

	rc = drakonia_command(device, DRAKONIA_CMD_BUTTONS, 0x01, DRAKONIA_BUTTON_TABLE_SIZE);
	if (rc == 0)
		rc = drakonia_send_block(device, table, sizeof(table));
	if (rc == 0)
		rc = drakonia_command(device, DRAKONIA_CMD_MACROS, 0x01, 0x00, DRAKONIA_MACRO_AREA_SIZE / 256);
	if (rc == 0)
		rc = drakonia_send_block(device, macros, sizeof(macros));
	return rc;
}

static int
drakonia_write_profile(struct ratbag_device *device, struct ratbag_profile *profile)
{
	struct ratbag_resolution *resolution;
	struct ratbag_button *button;
	struct ratbag_led *led;
	bool buttons_dirty = false;
	int rc;

	if (profile->rate_dirty) {
		rc = drakonia_command(device, DRAKONIA_CMD_REPORT_RATE,
				      drakonia_encode_report_rate(profile->hz));
		if (rc)
			return rc;
	}

	ratbag_profile_for_each_button(profile, button)
		buttons_dirty |= button->dirty;

	if (buttons_dirty) {
		rc = drakonia_write_buttons(device, profile);
		if (rc)
			return rc;
	}

	ratbag_profile_for_each_led(profile, led) {
		uint8_t mode, brightness;

		if (!led->dirty)
			continue;

		drakonia_encode_led(led, &mode, &brightness);
		rc = drakonia_command(device, DRAKONIA_CMD_COLOR,
				      led->color.red, led->color.green, led->color.blue);
		if (rc == 0)
			rc = drakonia_command(device, DRAKONIA_CMD_LED_MODE, mode);
		if (rc == 0)
			rc = drakonia_command(device, DRAKONIA_CMD_LED_BRIGHTNESS, brightness);
		if (rc)
			return rc;
	}

	ratbag_profile_for_each_resolution(profile, resolution) {
		if (!resolution->dirty)
			continue;

		rc = drakonia_command(device, DRAKONIA_CMD_DPI,
				      drakonia_encode_dpi(resolution->dpi_x));
		if (rc)
			return rc;
	}

	return 0;
}

static int
drakonia_commit(struct ratbag_device *device)
{
	struct ratbag_profile *profile;
	int rc;

	ratbag_device_for_each_profile(device, profile) {
		if (!profile->dirty)
			continue;

		rc = drakonia_write_profile(device, profile);
		if (rc) {
			log_error(device->ratbag, "%s: failed to write the profile: %s\n",
				  device->name, strerror(-rc));
			return RATBAG_ERROR_DEVICE;
		}
	}

	return RATBAG_SUCCESS;
}

static void
drakonia_remove(struct ratbag_device *device)
{
	ratbag_close_hidraw_index(device, DRAKONIA_BLOCK_HIDRAW);
	ratbag_close_hidraw(device);
	free(ratbag_get_drv_data(device));
}

struct ratbag_driver sharkoon_drakonia_driver = {
	.name = "Sharkoon Drakonia",
	.id = "sharkoon_drakonia",
	.probe = drakonia_probe,
	.remove = drakonia_remove,
	.commit = drakonia_commit,
};
