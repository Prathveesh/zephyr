/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * v1: button-triggered suspend/resume only (REQ-2, REQ-4). CLI trigger
 * (REQ-3, REQ-5) is deferred to a later release (Requirements/
 * REQUIREMENTS.md REQ-20), so this sample has no shell commands yet --
 * the pwr_profile_mgr_stm32f4 backend's button ISR/k_work wiring
 * (REQ-15) drives the whole demo on its own once initialized.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	printk("state_manager: ready. Press the user button to suspend/resume.\n");

	return 0;
}
