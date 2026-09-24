/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * STM32F407 Discovery backend. PWR_PROFILE_SLEEP maps to real STM32 Stop
 * mode via Zephyr's own PM subsystem (PM_STATE_SUSPEND_TO_IDLE ->
 * soc/st/stm32/stm32f4x/power.c's LL_PWR_MODE_STOP_LPREGU), not RM0090's
 * lighter "Sleep mode" (REQ-17).
 *
 * enter_sleep() only arms the next idle entry (pm_state_force()); it does
 * not itself block until wake. The actual WFI/Stop-mode halt happens later,
 * naturally, once nothing else is runnable and the kernel's own idle
 * thread picks it up -- and Zephyr's pm_state_exit_post_ops() restores
 * clocks automatically as part of that same idle-exit path, before any
 * wake-source ISR (button EXTI0 here) runs. So exit_sleep() has nothing
 * left to do.
 *
 * v1 wires only the button (REQ-2/REQ-4) as a trigger source; CLI trigger
 * (REQ-3/REQ-5) is deferred (Requirements/REQUIREMENTS.md REQ-20).
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/state.h>

#include "pwr_profile_backend.h"
#include "pwr_profile_mgr.h"

LOG_MODULE_REGISTER(pwr_profile_mgr_stm32f4, CONFIG_PWR_PROFILE_MGR_LOG_LEVEL);

#define LED_PERIOD_MS 300U

/* Active: led0/led1/led3 (green/orange/blue) blink together.
 * Sleep: led2 (red) solid on, the other three dark. REQ-21.
 */
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_orange = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static struct k_timer led_timer;
static bool led_on;
static bool led_running;
static uint32_t led_remaining_ms;

static struct gpio_callback button_cb;
static struct k_work button_work;

static const struct pm_state_info *stop_state;

/* --- LED driver: suspend/resume/rollback (REQ-9, REQ-21) --- */

static void active_leds_set(int value)
{
	gpio_pin_set_dt(&led_green, value);
	gpio_pin_set_dt(&led_orange, value);
	gpio_pin_set_dt(&led_blue, value);
}

static void led_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	led_on = !led_on;
	active_leds_set(led_on);
}

static int led_suspend(k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	if (led_running) {
		led_remaining_ms = k_timer_remaining_get(&led_timer);
		k_timer_stop(&led_timer);
		led_running = false;
	}

	active_leds_set(0);
	gpio_pin_set_dt(&led_red, 1);

	return 0;
}

static int led_resume(k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	if (!led_running) {
		uint32_t first_ms = (led_remaining_ms > 0) ? led_remaining_ms : LED_PERIOD_MS;

		gpio_pin_set_dt(&led_red, 0);
		active_leds_set(led_on);
		k_timer_start(&led_timer, K_MSEC(first_ms), K_MSEC(LED_PERIOD_MS));
		led_running = true;
	}

	return 0;
}

static int led_rollback(enum pwr_profile_state target, k_timeout_t timeout)
{
	/* Undo a failed transition towards @p target: heading to SLEEP means
	 * restore the running-LED (ACTIVE-like) state; heading to ACTIVE
	 * means restore the paused-LED (SLEEP-like) state. Idempotent either
	 * way, since suspend()/resume() are themselves idempotent.
	 */
	return (target == PWR_PROFILE_SLEEP) ? led_resume(timeout) : led_suspend(timeout);
}

static const struct pwr_profile_drv drivers[] = {
	{
		.name = "led",
		.suspend = led_suspend,
		.resume = led_resume,
		.rollback = led_rollback,
	},
};

/* --- Stop-mode entry/exit --- */

static int stm32f4_enter_sleep(k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	if (stop_state == NULL) {
		return -EIO;
	}

	pm_state_force(0U, stop_state);

	return 0;
}

static int stm32f4_exit_sleep(k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	return 0;
}

const struct pwr_profile_backend_ops pwr_profile_backend = {
	.enter_sleep = stm32f4_enter_sleep,
	.exit_sleep = stm32f4_exit_sleep,
	.drivers = drivers,
	.num_drivers = ARRAY_SIZE(drivers),
};

/* --- Button trigger: ISR defers to k_work, thread context only (REQ-15) --- */

static void button_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int rc = (pwr_profile_get_state() == PWR_PROFILE_ACTIVE) ? pwr_profile_suspend()
								   : pwr_profile_resume();

	if (rc != 0) {
		LOG_WRN("button-triggered transition failed (%d)", rc);
	}
}

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_submit(&button_work);
}

static int pwr_profile_mgr_stm32f4_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&led_green) || !gpio_is_ready_dt(&led_orange) ||
	    !gpio_is_ready_dt(&led_red) || !gpio_is_ready_dt(&led_blue) ||
	    !gpio_is_ready_dt(&button)) {
		LOG_ERR("LED or button GPIO device not ready");
		return -ENODEV;
	}

	const struct gpio_dt_spec *leds[] = {&led_green, &led_orange, &led_red, &led_blue};

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		rc = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
		if (rc != 0) {
			return rc;
		}
	}

	rc = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}

	rc = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (rc != 0) {
		return rc;
	}

	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	rc = gpio_add_callback_dt(&button, &button_cb);
	if (rc != 0) {
		return rc;
	}

	k_work_init(&button_work, button_work_handler);

	stop_state = pm_state_get(0U, PM_STATE_SUSPEND_TO_IDLE, 0U);
	if (stop_state == NULL) {
		LOG_ERR("PM_STATE_SUSPEND_TO_IDLE not available on cpu0");
		return -ENOTSUP;
	}

	k_timer_init(&led_timer, led_timer_handler, NULL);
	led_on = true;
	active_leds_set(1);
	k_timer_start(&led_timer, K_MSEC(LED_PERIOD_MS), K_MSEC(LED_PERIOD_MS));
	led_running = true;

	return 0;
}

SYS_INIT(pwr_profile_mgr_stm32f4_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
