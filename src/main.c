#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_hid.h>

#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>

#include "usbd_init.h"

#define LOG_LEVEL 4
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main);

#define KEYMAP_NODE		DT_ALIAS(keymap)
static const struct device *const keymap_dev = DEVICE_DT_GET(KEYMAP_NODE);

#define ENCODER_NODE		DT_ALIAS(encoder)
static const struct device *const encoder_dev = DEVICE_DT_GET(ENCODER_NODE);

#define UNDERGLOW_NODE		DT_ALIAS(underglow_strip)
static const struct device *const underglow_strip = DEVICE_DT_GET(UNDERGLOW_NODE);

#define NUMLOCK_LED_NODE	DT_ALIAS(numlock_led)
static const struct device *const numlock_led = DEVICE_DT_GET(NUMLOCK_LED_NODE);

#if DT_NODE_HAS_PROP(DT_ALIAS(underglow_strip), chain_length)
#define UNDERGLOW_NUM_PIXELS	DT_PROP(DT_ALIAS(underglow_strip), chain_length)
#else
#error Unable to determine length of LED strip
#endif

static struct led_rgb numlock_led_on = {
	.r = 4, .g = 1, .b = 1
};
static struct led_rgb numlock_led_off = {
	.r = 0, .g = 0, .b = 0
};

static struct led_rgb underglow_idle = {
	.r = 60, .g = 40, .b = 220
};
static struct led_rgb underglow_numlock_on = {
	.r = 255, .g = 0, .b = 0
};
static struct led_rgb pixels[UNDERGLOW_NUM_PIXELS];

enum color_channel {
	CHANNEL_R,
	CHANNEL_G,
	CHANNEL_B,
};
enum color_channel edit_channel = CHANNEL_R;
struct led_rgb *edit_color = &underglow_idle;

static const uint8_t hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

enum kb_leds_idx {
	KB_LED_NUMLOCK = 0,
	KB_LED_CAPSLOCK,
	KB_LED_SCROLLLOCK,
	KB_LED_COUNT,
};

enum kb_report_idx {
	KB_MOD_KEY = 0,
	KB_RESERVED,
	KB_KEY_CODE1,
	KB_KEY_CODE2,
	KB_KEY_CODE3,
	KB_KEY_CODE4,
	KB_KEY_CODE5,
	KB_KEY_CODE6,
	KB_REPORT_COUNT,
};

struct kb_event {
	uint8_t type;
	uint16_t code;
	int32_t value;
};

K_MSGQ_DEFINE(kb_msgq, sizeof(struct kb_event), 2, 1);

UDC_STATIC_BUF_DEFINE(report, KB_REPORT_COUNT);
static uint32_t kb_duration;
static bool kb_ready;
static bool numlock_on;

static void kb_input_cb(struct input_event *evt, void *user_data)
{
	struct kb_event kb_evt;

	ARG_UNUSED(user_data);

	kb_evt.type = evt->type;
	kb_evt.code = evt->code;
	kb_evt.value = evt->value;
	if (k_msgq_put(&kb_msgq, &kb_evt, K_NO_WAIT) != 0) {
		LOG_ERR("Failed to put new input event");
	}
}
INPUT_CALLBACK_DEFINE(keymap_dev, kb_input_cb, NULL);

static void enc_input_cb(struct input_event *evt, void *user_data)
{
	struct kb_event kb_evt;

	ARG_UNUSED(user_data);

	kb_evt.type = evt->type;
	kb_evt.code = evt->code;
	kb_evt.value = evt->value;
	if (k_msgq_put(&kb_msgq, &kb_evt, K_NO_WAIT) != 0) {
		LOG_ERR("Failed to put new input event");
	}
}
INPUT_CALLBACK_DEFINE(encoder_dev, enc_input_cb, NULL);

static void kb_iface_ready(const struct device *dev, const bool ready)
{
	LOG_INF("HID device %s interface is %s",
		dev->name, ready ? "ready" : "not ready");
	kb_ready = ready;
}

static int kb_get_report(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 uint8_t *const buf)
{
	LOG_WRN("Get Report not implemented, Type %u ID %u", type, id);

	return 0;
}

static int set_underglow_rgb(const struct led_rgb *color)
{
	for (int i = 0; i < UNDERGLOW_NUM_PIXELS; i++)
		pixels[i] = *color;

	return led_strip_update_rgb(underglow_strip, pixels, UNDERGLOW_NUM_PIXELS);
}

static void update_leds()
{
	if (numlock_on) {
		led_strip_update_rgb(numlock_led, &numlock_led_off, 1);
		set_underglow_rgb(&underglow_idle);
	} else {
		led_strip_update_rgb(numlock_led, &numlock_led_on, 1);
		set_underglow_rgb(&underglow_numlock_on);
	}
}

static int kb_set_report(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 const uint8_t *const buf)
{
	if (type != HID_REPORT_TYPE_OUTPUT) {
		LOG_WRN("Unsupported report type");
		return -ENOTSUP;
	}

	if (buf[0] & BIT(0)) {
		numlock_on = false;
	} else {
		numlock_on = true;
	}

	update_leds();

	return 0;
}

/* Idle duration is stored but not used to calculate idle reports. */
static void kb_set_idle(const struct device *dev,
			const uint8_t id, const uint32_t duration)
{
	LOG_INF("Set Idle %u to %u", id, duration);
	kb_duration = duration;
}

static uint32_t kb_get_idle(const struct device *dev, const uint8_t id)
{
	LOG_INF("Get Idle %u to %u", id, kb_duration);
	return kb_duration;
}

static void kb_set_protocol(const struct device *dev, const uint8_t proto)
{
	LOG_INF("Protocol changed to %s",
		proto == 0U ? "Boot Protocol" : "Report Protocol");
}

static void kb_output_report(const struct device *dev, const uint16_t len,
			     const uint8_t *const buf)
{
	LOG_HEXDUMP_DBG(buf, len, "o.r.");
	kb_set_report(dev, HID_REPORT_TYPE_OUTPUT, 0U, len, buf);
}

struct hid_device_ops kb_ops = {
	.iface_ready = kb_iface_ready,
	.get_report = kb_get_report,
	.set_report = kb_set_report,
	.set_idle = kb_set_idle,
	.get_idle = kb_get_idle,
	.set_protocol = kb_set_protocol,
	.output_report = kb_output_report,
};

static void msg_cb(struct usbd_context *const usbd_ctx,
		   const struct usbd_msg *const msg)
{
	LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

	if (msg->type == USBD_MSG_CONFIGURATION) {
		LOG_INF("\tConfiguration value %d", msg->status);
	}

	if (usbd_can_detect_vbus(usbd_ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(usbd_ctx)) {
				LOG_ERR("Failed to enable device support");
			}
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(usbd_ctx)) {
				LOG_ERR("Failed to disable device support");
			}
		}
	}
}

static void handle_kb_event(const struct device *hid_dev,
			    uint16_t code, int32_t value)
{
	int ret;
	int16_t hid_code;

	hid_code = input_to_hid_code(code);
	if (hid_code == -1) {
		LOG_INF("Unrecognized input code %u value %d", code, value);
		return;
	}

	switch (code) {
	case INPUT_KEY_KP7:
		edit_channel = CHANNEL_R;
		edit_color = &underglow_idle;
		return;
	case INPUT_KEY_KP8:
		edit_channel = CHANNEL_G;
		edit_color = &underglow_idle;
		return;
	case INPUT_KEY_KP9:
		edit_channel = CHANNEL_B;
		edit_color = &underglow_idle;
		return;

	case INPUT_KEY_KP4:
		edit_channel = CHANNEL_R;
		edit_color = &underglow_numlock_on;
		return;
	case INPUT_KEY_KP5:
		edit_channel = CHANNEL_G;
		edit_color = &underglow_numlock_on;
		return;
	case INPUT_KEY_KP6:
		edit_channel = CHANNEL_B;
		edit_color = &underglow_numlock_on;
		return;

	case INPUT_KEY_KP1:
		edit_channel = CHANNEL_R;
		edit_color = &numlock_led_off;
		return;
	case INPUT_KEY_KP2:
		edit_channel = CHANNEL_G;
		edit_color = &numlock_led_off;
		return;
	case INPUT_KEY_KP3:
		edit_channel = CHANNEL_B;
		edit_color = &numlock_led_off;
		return;

	case INPUT_KEY_BACKSPACE:
		edit_channel = CHANNEL_R;
		edit_color = &numlock_led_on;
		return;
	case INPUT_KEY_KP0:
		edit_channel = CHANNEL_G;
		edit_color = &numlock_led_on;
		return;
	case INPUT_KEY_DOT:
		edit_channel = CHANNEL_B;
		edit_color = &numlock_led_on;
		return;
	}

	if (value) {
		report[KB_KEY_CODE1] = hid_code;
		report[KB_MOD_KEY] = input_to_hid_modifier(code);
	} else {
		report[KB_KEY_CODE1] = 0;
		report[KB_MOD_KEY] = 0;
	}

	if (!kb_ready) {
		LOG_INF("USB HID device is not ready");
		return;
	}

	ret = hid_device_submit_report(hid_dev, KB_REPORT_COUNT, report);
	if (ret) {
		LOG_ERR("HID submit report error, %d", ret);
	}
}

static void handle_enc_event(const struct device *hid_dev,
			     uint16_t code, int32_t value)
{
	static int enc_events = 0;

	LOG_INF("Encoder event %d value %d", enc_events, value);
	enc_events++;

	if (value != 1 && value != -1) {
		LOG_ERR("Wrong encoder value %d", value);
		return;
	}

	switch (edit_channel) {
	case CHANNEL_R:
		edit_color->r += value;
		break;
	case CHANNEL_G:
		edit_color->g += value;
		break;
	case CHANNEL_B:
		edit_color->b += value;
		break;
	}

	LOG_INF("r: %u, g: %u, b: %u",
		edit_color->r, edit_color->g, edit_color->b);

	update_leds();
}

int main(void)
{
	const struct device *hid_dev;
	struct usbd_context *sample_usbd;
	int ret;

	if (!device_is_ready(underglow_strip)) {
		LOG_ERR("LED strip device %s is not ready", underglow_strip->name);
		return 0;
	}

	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
	if (!device_is_ready(hid_dev)) {
		LOG_ERR("HID Device is not ready");
		return -EIO;
	}

	ret = hid_device_register(hid_dev,
				  hid_report_desc, sizeof(hid_report_desc),
				  &kb_ops);
	if (ret != 0) {
		LOG_ERR("Failed to register HID Device, %d", ret);
		return ret;
	}

	sample_usbd = sample_usbd_init_device(msg_cb);
	if (sample_usbd == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return -ENODEV;
	}

	if (!usbd_can_detect_vbus(sample_usbd)) {
		/* doc device enable start */
		ret = usbd_enable(sample_usbd);
		if (ret) {
			LOG_ERR("Failed to enable device support");
			return ret;
		}
		/* doc device enable end */
	}

	LOG_INF("HID keyboard is initialized");

	while (true) {
		struct kb_event kb_evt;

		k_msgq_get(&kb_msgq, &kb_evt, K_FOREVER);

		switch (kb_evt.type) {
		case INPUT_EV_KEY:
			handle_kb_event(hid_dev, kb_evt.code, kb_evt.value);
			break;
		case INPUT_EV_REL:
			handle_enc_event(hid_dev, kb_evt.code, kb_evt.value);
			break;
		default:
			LOG_INF("Enexpected event type %u", kb_evt.type);
		}

	}

	return 0;
}
