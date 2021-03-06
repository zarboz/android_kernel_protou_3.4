/* Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/cpufreq.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>

#include <mach/irqs.h>
#include <mach/msm_iomap.h>

#include <trace/events/power.h>

#include "msm_cpr.h"

#define MODULE_NAME "msm-cpr"

#define TIMER_COUNT(freq, delay) ((freq * delay) / 1000)
#define ALL_CPR_IRQ 0x3F
#define STEP_QUOT_MAX 25
#define STEP_QUOT_MIN 14

#define VMAX_BREACH_CNT 15

void __iomem *virt_start_ptr;

static struct platform_device *cpr_pdev;

static bool enable = 1;
static bool disable_cpr = true;
static int nom_Vmin;
static int turbo_Vmin;
static int max_quot;

module_param(enable, bool, 0644);
module_param(nom_Vmin, int, 0644);
module_param(turbo_Vmin, int, 0644);
module_param(max_quot, int, 0644);

MODULE_PARM_DESC(enable, "CPR Enable");
MODULE_PARM_DESC(nom_Vmin, "Nominal VMin");
MODULE_PARM_DESC(turbo_Vmin, "Turbo VMin");
MODULE_PARM_DESC(max_quot, "Max Quot");

extern struct regulator *ncp6335d_handle;

static int msm_cpr_debug_mask;
module_param_named(
	debug_mask, msm_cpr_debug_mask, int, S_IRUGO | S_IWUSR
);

enum {
	
	MSM_CPR_DEBUG_CONFIG = BIT(0),
	
	MSM_CPR_DEBUG_STEPS = BIT(1),
	
	MSM_CPR_DEBUG_FREQ_TRANS = BIT(2),
};\

#define msm_cpr_debug(mask, message, ...) \
	do { \
		if ((mask) & msm_cpr_debug_mask) \
			pr_info(message, ##__VA_ARGS__); \
	} while (0)

struct msm_cpr {
	int curr_osc;
	uint32_t floor;
	uint32_t ceiling;
	bool max_volt_set;
	void __iomem *base;
	unsigned int irq;
	uint32_t cur_Vmin;
	uint32_t cur_Vmax;
	uint32_t prev_volt_uV;
	struct mutex cpr_mutex;
	spinlock_t cpr_lock;
	struct regulator *vreg_cx;
	struct msm_cpr_config *config;
	struct notifier_block freq_transition;
	uint32_t step_size;
};

static struct msm_cpr_reg cpr_save_state;
static struct msm_cpr *msm_cpr;
static int max_volt_count;

static inline
void cpr_write_reg(struct msm_cpr *cpr, u32 offset, u32 value)
{
	writel_relaxed(value, cpr->base + offset);
}

static inline u32 cpr_read_reg(struct msm_cpr *cpr, u32 offset)
{
	return readl_relaxed(cpr->base + offset);
}

static
void cpr_modify_reg(struct msm_cpr *cpr, u32 offset, u32 mask, u32 value)
{
	u32 reg_val;

	reg_val = readl_relaxed(cpr->base + offset);
	reg_val &= ~mask;
	reg_val |= value;
	writel_relaxed(reg_val, cpr->base + offset);
}

#ifdef DEBUG
static void cpr_regs_dump_all(struct msm_cpr *cpr)
{
	pr_debug("RBCPR_GCNT_TARGET(%d): 0x%x\n",
		cpr->curr_osc, readl_relaxed(cpr->base +
		RBCPR_GCNT_TARGET(cpr->curr_osc)));
	pr_debug("RBCPR_TIMER_INTERVAL: 0x%x\n",
		readl_relaxed(cpr->base + RBCPR_TIMER_INTERVAL));
	pr_debug("RBIF_TIMER_ADJUST: 0x%x\n",
		readl_relaxed(cpr->base + RBIF_TIMER_ADJUST));
	pr_debug("RBIF_LIMIT: 0x%x\n",
		readl_relaxed(cpr->base + RBIF_LIMIT));
	pr_debug("RBCPR_STEP_QUOT: 0x%x\n",
		readl_relaxed(cpr->base + RBCPR_STEP_QUOT));
	pr_debug("RBIF_SW_VLEVEL: 0x%x\n",
		readl_relaxed(cpr->base + RBIF_SW_VLEVEL));
	pr_debug("RBCPR_DEBUG1: 0x%x\n",
		readl_relaxed(cpr->base + RBCPR_DEBUG1));
	pr_debug("RBCPR_RESULT_0: 0x%x\n",
		readl_relaxed(cpr->base + RBCPR_RESULT_0));
	pr_debug("RBCPR_RESULT_1: 0x%x\n",
		readl_relaxed(cpr->base + RBCPR_RESULT_1));
	pr_debug("RBCPR_QUOT_AVG: 0x%x\n",
		readl_relaxed(cpr->base + RBCPR_QUOT_AVG));
	pr_debug("RBCPR_CTL: 0x%x\n",
		readl_relaxed(cpr->base + RBCPR_CTL));
	pr_debug("RBIF_IRQ_EN(0): 0x%x\n",
		cpr_read_reg(cpr, RBIF_IRQ_EN(cpr->config->irq_line)));
	pr_debug("RBIF_IRQ_STATUS: 0x%x\n",
		cpr_read_reg(cpr, RBIF_IRQ_STATUS));
}
#endif

static void cpr_enable(struct msm_cpr *cpr)
{
	spin_lock(&cpr->cpr_lock);
	cpr_modify_reg(cpr, RBCPR_CTL, LOOP_EN_M, ENABLE_CPR);
	spin_unlock(&cpr->cpr_lock);
}

static void cpr_disable(struct msm_cpr *cpr)
{
	spin_lock(&cpr->cpr_lock);
	cpr_modify_reg(cpr, RBCPR_CTL, LOOP_EN_M, DISABLE_CPR);
	spin_unlock(&cpr->cpr_lock);
}

static int32_t cpr_poll_result(struct msm_cpr *cpr)
{
	uint32_t val = 0;
	int8_t rc = 0;

	rc = readl_poll_timeout(cpr->base + RBCPR_RESULT_0, val, ~val & BUSY_M,
				10, 1000);
	if (rc)
		pr_err("RBCPR_RESULT_0 read error: %d\n", rc);
	return rc;
}

static int32_t cpr_poll_result_done(struct msm_cpr *cpr)
{
	uint32_t val = 0;
	int8_t rc = 0;

	rc = readl_poll_timeout(cpr->base + RBIF_IRQ_STATUS, val, val & 0x1,
				10, 1000);
	if (rc)
		pr_err("RBCPR_IRQ_STATUS read error: %d\n", rc);
	return rc;
}

static int
cpr_2pt_kv_analysis(struct msm_cpr *cpr, struct msm_cpr_mode *chip_data)
{
	int32_t level_uV = 0, rc;
	uint32_t quot1, quot2;

	if (chip_data->step_quot != ~0)
		goto out_2pt_kv;

	level_uV = chip_data->turbo_Vmax -
		(chip_data->tgt_volt_offset * cpr->step_size);
	msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
		"tgt_volt_uV = %d\n", level_uV);

	
	rc = regulator_set_voltage(cpr->vreg_cx, level_uV, level_uV);
	if (rc) {
		pr_err("Initial voltage set at %duV failed\n", level_uV);
		return rc;
	}

	rc = regulator_enable(cpr->vreg_cx);
	if (rc) {
		pr_err("failed to enable %s, rc=%d\n", "vdd_cx", rc);
		return rc;
	}

	

	
	cpr_modify_reg(cpr, RBCPR_CTL, HW_TO_PMIC_EN_M, SW_MODE);

	
	cpr_modify_reg(cpr, RBCPR_CTL, LOOP_EN_M, ENABLE_CPR);

	
	rc = cpr_poll_result_done(cpr);
	if (rc) {
		pr_err("Quot1: Exiting due to INT_DONE poll timeout\n");
		goto err_poll_result_done;
	}

	rc = cpr_poll_result(cpr);
	if (rc) {
		pr_err("Quot1: Exiting due to BUSY poll timeout\n");
		goto err_poll_result;
	}

	quot1 = (cpr_read_reg(cpr, RBCPR_DEBUG1) & QUOT_SLOW_M) >> 12;

	
	level_uV -= 4 * cpr->step_size;
	msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
		"tgt_volt_uV = %d\n", level_uV);

	cpr_modify_reg(cpr, RBCPR_CTL, LOOP_EN_M, DISABLE_CPR);
	
	rc = regulator_set_voltage(cpr->vreg_cx, level_uV, level_uV);
	if (rc) {
		pr_err("Voltage set at %duV failed\n", level_uV);
		goto err_set_voltage;
	}

	cpr_modify_reg(cpr, RBCPR_CTL, HW_TO_PMIC_EN_M, SW_MODE);
	cpr_modify_reg(cpr, RBCPR_CTL, LOOP_EN_M, ENABLE_CPR);

	rc = cpr_poll_result_done(cpr);
	if (rc) {
		pr_err("Quot2: Exiting due to INT_DONE poll timeout\n");
		goto err_poll_result_done;
	}
	
	rc = cpr_poll_result(cpr);
	if (rc) {
		pr_err("Quot2: Exiting due to BUSY poll timeout\n");
		goto err_poll_result;
	}
	quot2 = (cpr_read_reg(cpr, RBCPR_DEBUG1) & QUOT_SLOW_M) >> 12;
	chip_data->step_quot = ((quot1 - quot2) / 4) + 3;
	if (chip_data->step_quot < (STEP_QUOT_MIN / chip_data->step_div) ||
		chip_data->step_quot > (STEP_QUOT_MAX / chip_data->step_div))
		chip_data->step_quot = STEP_QUOT_MAX / chip_data->step_div;

	msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
		"Step Quot is %d\n", chip_data->step_quot);
	
	cpr_modify_reg(cpr, RBCPR_CTL, LOOP_EN_M, DISABLE_CPR);

out_2pt_kv:
	
	cpr_write_reg(cpr, RBCPR_STEP_QUOT, (chip_data->step_quot & 0xFF));
	return 0;
err_set_voltage:
err_poll_result:
err_poll_result_done:
	regulator_disable(cpr->vreg_cx);
	return rc;
}

static inline
void cpr_irq_clr_and_nack(struct msm_cpr *cpr)
{
	cpr_write_reg(cpr, RBIF_IRQ_CLEAR, ALL_CPR_IRQ);
	cpr_write_reg(cpr, RBIF_CONT_NACK_CMD, 0x1);
}

static void cpr_irq_set(struct msm_cpr *cpr, uint32_t irq, bool enable_irq)
{
	uint32_t irq_enabled;

	irq_enabled = cpr_read_reg(cpr, RBIF_IRQ_EN(cpr->config->irq_line));
	if (enable_irq == 1)
		irq_enabled |= irq;
	else
		irq_enabled &= ~irq;
	cpr_modify_reg(cpr, RBIF_IRQ_EN(cpr->config->irq_line),
			INT_MASK, irq_enabled);
}

static void
cpr_up_event_handler(struct msm_cpr *cpr, uint32_t new_volt)
{
	int set_volt_uV, rc;
	struct msm_cpr_mode *chip_data;

	chip_data = cpr->config->cpr_mode_data;

	
	msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
		"current Vmin=%d Vmax=%d\n", cpr->cur_Vmin, cpr->cur_Vmax);
	set_volt_uV = (new_volt < cpr->cur_Vmax ? new_volt
				: cpr->cur_Vmax);

	
	*(uint32_t *)(virt_start_ptr + 0x0) = set_volt_uV;
	msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
		"Value of MSC2 recommended voltage saved at 0x%x: %duV\n",
		(MSM8625_NON_CACHE_MEM + 0x0),
		*(uint32_t *)(virt_start_ptr + 0x0));

	if (cpr->prev_volt_uV == set_volt_uV)
		rc = regulator_sync_voltage(cpr->vreg_cx);
	else
		rc = regulator_set_voltage(cpr->vreg_cx, set_volt_uV,
							set_volt_uV);
	if (rc) {
		pr_err("Unable to set_voltage = %d, rc(%d)\n", set_volt_uV, rc);
		cpr_irq_clr_and_nack(cpr);
		return;
	}

	msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
		"(railway_voltage: %d uV)\n", set_volt_uV);
	cpr->prev_volt_uV = set_volt_uV;

	cpr->max_volt_set = (set_volt_uV == cpr->cur_Vmax) ? 1 : 0;

	
	cpr_write_reg(cpr, RBIF_IRQ_CLEAR, ALL_CPR_IRQ);

	
	cpr_modify_reg(cpr, RBCPR_CTL, SW_AUTO_CONT_NACK_DN_EN_M, 0);

	cpr_irq_set(cpr, DOWN_INT, 1);

	cpr_write_reg(cpr, RBIF_CONT_NACK_CMD, 0x1);
}

static void
cpr_dn_event_handler(struct msm_cpr *cpr, uint32_t new_volt, uint32_t err_step)
{
	int set_volt_uV, rc;
	struct msm_cpr_mode *chip_data;

	chip_data = cpr->config->cpr_mode_data;

	
	set_volt_uV = (new_volt > cpr->cur_Vmin ? new_volt
				: cpr->cur_Vmin);

	
	*(uint32_t *)(virt_start_ptr + 0x0) = set_volt_uV;
	msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
		"Value of MSC2 recommended voltage saved at 0x%x: %duV\n",
		(MSM8625_NON_CACHE_MEM + 0x0),
		*(uint32_t *)(virt_start_ptr + 0x0));

	if (cpr->prev_volt_uV == set_volt_uV)
		rc = regulator_sync_voltage(cpr->vreg_cx);
	else
		rc = regulator_set_voltage(cpr->vreg_cx, set_volt_uV,
							set_volt_uV);
	if (rc) {
		pr_err("Unable to set_voltage = %d, rc(%d)\n", set_volt_uV, rc);
		cpr_irq_clr_and_nack(cpr);
		return;
	}

	msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
		"(railway_voltage: %d uV)\n", set_volt_uV);
	cpr->prev_volt_uV = set_volt_uV;

	cpr->max_volt_set = 0;

	
	cpr_write_reg(cpr, RBIF_IRQ_CLEAR, ALL_CPR_IRQ);

	if (new_volt <= cpr->cur_Vmin) {
		cpr_modify_reg(cpr, RBCPR_CTL,
				SW_AUTO_CONT_NACK_DN_EN_M,
				SW_AUTO_CONT_NACK_DN_EN);
		cpr_irq_set(cpr, DOWN_INT, 0);
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
				"DOWN_INT disabled\n");
	}

	if (err_step == 1)
		cpr_write_reg(cpr, RBIF_CONT_ACK_CMD, 0x1);
	else
		cpr_write_reg(cpr, RBIF_CONT_NACK_CMD, 0x1);
}

static void cpr_set_vdd(struct msm_cpr *cpr, enum cpr_action action)
{
	uint32_t curr_volt, new_volt, error_step;
	struct msm_cpr_mode *chip_data;

	chip_data = cpr->config->cpr_mode_data;
	error_step = cpr_read_reg(cpr, RBCPR_RESULT_0) >> 2;

	msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
		"RBCPR_RESULT_0 17:6=%d\n", (cpr_read_reg(cpr,
				RBCPR_RESULT_0) >> 6) & 0xFFF);
	msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
		"RBCPR_RESULT_0 Busy_b19=%d\n", (cpr_read_reg(cpr,
				RBCPR_RESULT_0) >> 19) & 0x1);

	error_step &= 0xF;
	curr_volt = regulator_get_voltage(cpr->vreg_cx);
	msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
		"Current voltage=%d\n", curr_volt);

	if (action == UP) {
		
		if (cpr->max_volt_set == 1) {
			if (++max_volt_count == VMAX_BREACH_CNT) {
				
				cpr_disable(cpr);
			}
			cpr_irq_clr_and_nack(cpr);
			return;
		}

		if (error_step < (cpr->config->up_threshold +
					cpr->config->up_margin)) {
			msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
				"UP_INT error step too small to set\n");
			cpr_irq_clr_and_nack(cpr);
			return;
		}

		error_step += 1;

		
		new_volt = curr_volt + (error_step * cpr->step_size);
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"UP_INT: new_volt: %d, error_step=%d\n",
					new_volt, error_step);
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"Current RBCPR_GCNT_TARGET(%d): = 0x%x\n",
			cpr->curr_osc, readl_relaxed(cpr->base +
			RBCPR_GCNT_TARGET(cpr->curr_osc)) & TARGET_M);
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"(UP Voltage recommended by CPR: %d uV)\n", new_volt);
		cpr_up_event_handler(cpr, new_volt);
		trace_cpr_data(new_volt, curr_volt, error_step);

	} else if (action == DOWN) {
		if (error_step < (cpr->config->dn_threshold +
					cpr->config->dn_margin)) {
			msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
				"DOWN_INT error_step=%d is too small to set\n",
								error_step);
			cpr_irq_clr_and_nack(cpr);
			return;
		}

		error_step -= 2;

		
		if (error_step > 2)
			error_step = 2;

		
		new_volt = curr_volt - (error_step * cpr->step_size);
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"DOWN_INT: new_volt: %d, error_step=%d\n",
			new_volt, error_step);
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"Current RBCPR_GCNT_TARGET(%d): = 0x%x\n",
			cpr->curr_osc, readl_relaxed(cpr->base +
			RBCPR_GCNT_TARGET(cpr->curr_osc)) & TARGET_M);
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"(DN Voltage recommended by CPR: %d uV)\n", new_volt);

		cpr_dn_event_handler(cpr, new_volt, error_step);
		trace_cpr_data(new_volt, curr_volt, -error_step);
	}
}

static irqreturn_t cpr_irq0_handler(int irq, void *dev_id)
{
	struct msm_cpr *cpr = dev_id;
	uint32_t reg_val, ctl_reg;

	reg_val = cpr_read_reg(cpr, RBIF_IRQ_STATUS);
	ctl_reg = cpr_read_reg(cpr, RBCPR_CTL);

	
	if (reg_val & BIT(4)) {
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"CPR:IRQ %d occured for UP Flag\n", irq);
		cpr_set_vdd(cpr, UP);

	} else if ((reg_val & BIT(2)) && !(ctl_reg & SW_AUTO_CONT_NACK_DN_EN)) {
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"CPR:IRQ %d occured for Down Flag\n", irq);
		cpr_set_vdd(cpr, DOWN);

	} else if (reg_val & BIT(1)) {
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"CPR:IRQ %d occured for Min Flag\n", irq);
		cpr_irq_clr_and_nack(cpr);

	} else if (reg_val & BIT(5)) {
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"CPR:IRQ %d occured for MAX Flag\n", irq);
		cpr_irq_clr_and_nack(cpr);

	} else if (reg_val & BIT(3)) {
		
		msm_cpr_debug(MSM_CPR_DEBUG_STEPS,
			"CPR:IRQ %d occured for Mid Flag\n", irq);
	}
	return IRQ_HANDLED;
}

static int cpr_config(struct msm_cpr *cpr)
{
	uint32_t delay_count, cnt = 0, rc;
	struct msm_cpr_mode *chip_data;

	chip_data = cpr->config->cpr_mode_data;

	
	cpr_modify_reg(cpr, RBIF_SW_VLEVEL, SW_VLEVEL_M,
			cpr->config->sw_vlevel);

	
	cpr->floor =  cpr->config->floor;
	cpr->ceiling = cpr->config->ceiling;

	
	cpr_modify_reg(cpr, RBIF_LIMIT, (CEILING_M | FLOOR_M),
					((cpr->ceiling << 6) | cpr->floor));

	
	cpr_modify_reg(cpr, RBCPR_CTL, UP_THRESHOLD_M | DN_THRESHOLD_M,
			cpr->config->up_threshold << 24 |
			cpr->config->dn_threshold << 28);

	cpr->curr_osc = chip_data->ring_osc;
	chip_data->ring_osc_data[cpr->curr_osc].quot =
		cpr->config->max_quot;

	while (cnt < NUM_OSC) {
		msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
			"Prog:cnt(%d) gcnt=0x%x quot=0x%x\n", cnt,
			chip_data->ring_osc_data[cnt].gcnt,
			chip_data->ring_osc_data[cnt].quot);
		cpr_modify_reg(cpr, RBCPR_GCNT_TARGET(cnt),
				(GCNT_M | TARGET_M),
				(chip_data->ring_osc_data[cnt].gcnt << 12 |
				chip_data->ring_osc_data[cnt].quot));
		msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
			"RBCPR_GCNT_TARGET(%d): = 0x%x\n", cnt,
			readl_relaxed(cpr->base + RBCPR_GCNT_TARGET(cnt)));
		cnt++;
	}

	
	rc = cpr_2pt_kv_analysis(cpr, chip_data);
	if (rc) {
		pr_err("CPR: step quot configure failed%d\n", rc);
		return rc;
	}

	
	rc = regulator_set_voltage(cpr->vreg_cx, chip_data->calibrated_uV,
					chip_data->calibrated_uV);
	if (rc) {
		pr_err("CPR: voltage set failed %d\n", rc);
		return rc;
	}

	delay_count = TIMER_COUNT(cpr->config->ref_clk_khz,
					cpr->config->delay_us);
	cpr_write_reg(cpr, RBCPR_TIMER_INTERVAL, delay_count);

	
	cpr_write_reg(cpr, RBIF_TIMER_ADJUST, (0x2 << RBIF_CONS_DN_SHIFT));
	msm_cpr_debug(MSM_CPR_DEBUG_CONFIG, "RBIF_TIMER_ADJUST: 0x%x\n",
		readl_relaxed(cpr->base + RBIF_TIMER_ADJUST));

	
	cpr_modify_reg(cpr, RBCPR_CTL, TIMER_M, ENABLE_TIMER);

	
	cpr_modify_reg(cpr, RBCPR_CTL, SW_AUTO_CONT_ACK_EN_M,
			SW_AUTO_CONT_ACK_EN);
	return 0;
}

static int
cpr_freq_transition(struct notifier_block *nb, unsigned long val,
				void *data)
{
	struct msm_cpr *cpr = container_of(nb, struct msm_cpr, freq_transition);
	struct cpufreq_freqs *freqs = data;
	uint32_t quot, new_freq, ctl_reg;

	switch (val) {
	case CPUFREQ_PRECHANGE:
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"pre freq change notification to cpr\n");
		
		cpr_disable(cpr);
		
		cpr_irq_set(cpr, INT_MASK & ~MID_INT, 0);
		disable_irq(cpr->irq);
		cpr_write_reg(cpr, RBIF_IRQ_CLEAR, ALL_CPR_IRQ);

		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"RBCPR_CTL: 0x%x\n",
			readl_relaxed(cpr->base + RBCPR_CTL));
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"RBIF_IRQ_STATUS: 0x%x\n",
			cpr_read_reg(cpr, RBIF_IRQ_STATUS));
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"RBIF_IRQ_EN(0): 0x%x\n",
			cpr_read_reg(cpr, RBIF_IRQ_EN(cpr->config->irq_line)));

		break;

	case CPUFREQ_POSTCHANGE:
		pr_debug("post freq change notification to cpr\n");
		ctl_reg = cpr_read_reg(cpr, RBCPR_CTL);
		if (freqs->new > cpr->config->max_nom_freq) {
			new_freq = freqs->new;
			cpr->cur_Vmin = cpr->config->cpr_mode_data->turbo_Vmin;
			cpr->cur_Vmax = cpr->config->cpr_mode_data->turbo_Vmax;
		} else {
			new_freq = cpr->config->max_nom_freq;
			cpr->cur_Vmin = cpr->config->cpr_mode_data->nom_Vmin;
			cpr->cur_Vmax = cpr->config->cpr_mode_data->nom_Vmax;
		}

		
		quot = cpr->config->get_quot(cpr->config->max_quot,
						cpr->config->max_freq / 1000,
						new_freq / 1000);
		cpr_modify_reg(cpr, RBCPR_GCNT_TARGET(cpr->curr_osc), TARGET_M,
				quot);
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"RBCPR_GCNT_TARGET(%d): = 0x%x\n", cpr->curr_osc,
			readl_relaxed(cpr->base +
					RBCPR_GCNT_TARGET(cpr->curr_osc)));
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"new_freq: %d, quot_freq: %d, quot: %d\n",
			freqs->new, new_freq, quot);
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"PVS Voltage setting is: %d\n",
			regulator_get_voltage(cpr->vreg_cx));

		
		*(uint32_t *)(virt_start_ptr + 0x4) = freqs->new;
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"Freq before volt change by CPR saved at 0x%x: %dkHz\n",
			(MSM8625_NON_CACHE_MEM + 0x4),
			*(uint32_t *)(virt_start_ptr + 0x4));

		enable_irq(cpr->irq);
		cpr_irq_set(cpr, INT_MASK & ~MID_INT, 1);

		if (ctl_reg & SW_AUTO_CONT_NACK_DN_EN)
			cpr_modify_reg(cpr, RBCPR_CTL,
				 SW_AUTO_CONT_NACK_DN_EN_M, 0);
		if (cpr->max_volt_set)
			cpr->max_volt_set = 0;

		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"RBIF_IRQ_EN(0): 0x%x\n",
			cpr_read_reg(cpr, RBIF_IRQ_EN(cpr->config->irq_line)));
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"RBCPR_CTL: 0x%x\n",
			readl_relaxed(cpr->base + RBCPR_CTL));
		msm_cpr_debug(MSM_CPR_DEBUG_FREQ_TRANS,
			"RBIF_IRQ_STATUS: 0x%x\n",
			cpr_read_reg(cpr, RBIF_IRQ_STATUS));

		
		cpr_write_reg(cpr, RBIF_IRQ_CLEAR, ALL_CPR_IRQ);

		max_volt_count = 0;
		cpr_enable(cpr);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

#ifdef CONFIG_PM
static int msm_cpr_resume(void)
{
	struct msm_cpr *cpr = msm_cpr;
	int osc_num = cpr->config->cpr_mode_data->ring_osc;

	cpr->config->clk_enable();

	cpr_write_reg(cpr, RBCPR_TIMER_INTERVAL,
		cpr_save_state.rbif_timer_interval);
	cpr_write_reg(cpr, RBIF_IRQ_EN(cpr->config->irq_line),
		cpr_save_state.rbif_int_en);
	cpr_write_reg(cpr, RBIF_LIMIT,
		cpr_save_state.rbif_limit);
	cpr_write_reg(cpr, RBIF_TIMER_ADJUST,
		cpr_save_state.rbif_timer_adjust);
	cpr_write_reg(cpr, RBCPR_GCNT_TARGET(osc_num),
		cpr_save_state.rbcpr_gcnt_target);
	cpr_write_reg(cpr, RBCPR_STEP_QUOT,
		cpr_save_state.rbcpr_step_quot);
	cpr_write_reg(cpr, RBIF_SW_VLEVEL,
		cpr_save_state.rbif_sw_level);
	cpr_write_reg(cpr, RBCPR_CTL,
		cpr_save_state.rbcpr_ctl);

	
	cpr_write_reg(cpr, RBIF_IRQ_CLEAR, ALL_CPR_IRQ);

	enable_irq(cpr->irq);
	cpr_enable(cpr);

	return 0;
}

static int msm_cpr_suspend(void)
{
	struct msm_cpr *cpr = msm_cpr;
	int osc_num = cpr->config->cpr_mode_data->ring_osc;

	
	cpr_disable(cpr);
	disable_irq(cpr->irq);

	
	cpr_write_reg(cpr, RBIF_IRQ_CLEAR, ALL_CPR_IRQ);

	cpr_save_state.rbif_timer_interval =
		cpr_read_reg(cpr, RBCPR_TIMER_INTERVAL);
	cpr_save_state.rbif_int_en =
		cpr_read_reg(cpr, RBIF_IRQ_EN(cpr->config->irq_line));
	cpr_save_state.rbif_limit =
		cpr_read_reg(cpr, RBIF_LIMIT);
	cpr_save_state.rbif_timer_adjust =
		cpr_read_reg(cpr, RBIF_TIMER_ADJUST);
	cpr_save_state.rbcpr_gcnt_target =
		cpr_read_reg(cpr, RBCPR_GCNT_TARGET(osc_num));
	cpr_save_state.rbcpr_step_quot =
		cpr_read_reg(cpr, RBCPR_STEP_QUOT);
	cpr_save_state.rbif_sw_level =
		cpr_read_reg(cpr, RBIF_SW_VLEVEL);
	cpr_save_state.rbcpr_ctl =
		cpr_read_reg(cpr, RBCPR_CTL);

	return 0;
}

void msm_cpr_pm_resume(void)
{
	if (!enable || disable_cpr)
		return;

	msm_cpr_resume();
}
EXPORT_SYMBOL(msm_cpr_pm_resume);

void msm_cpr_pm_suspend(void)
{
	if (!enable || disable_cpr)
		return;

	msm_cpr_suspend();
}
EXPORT_SYMBOL(msm_cpr_pm_suspend);
#else
#define msm_cpr_suspend_core NULL
#define msm_cpr_resume_core NULL
#endif

void msm_cpr_disable(void)
{
	struct msm_cpr *cpr;

	if (!enable || disable_cpr)
		return;

	cpr = platform_get_drvdata(cpr_pdev);

	cpr_disable(cpr);
}
EXPORT_SYMBOL(msm_cpr_disable);

void msm_cpr_enable(void)
{
	struct msm_cpr *cpr;

	if (!enable || disable_cpr)
		return;

	cpr = platform_get_drvdata(cpr_pdev);

	cpr_enable(cpr);
}
EXPORT_SYMBOL(msm_cpr_enable);

static int __devinit msm_cpr_probe(struct platform_device *pdev)
{
	int res, irqn, irq_enabled;
	struct msm_cpr *cpr;
	struct msm_cpr_config *pdata = pdev->dev.platform_data;
	void __iomem *base;
	struct resource *mem;
	struct msm_cpr_mode *chip_data;

	if (!enable)
		return -EPERM;

	if (!pdata) {
		pr_err("CPR: Platform data is not available\n");
		enable = false;
		return -EIO;
	}

	if (pdata->disable_cpr == true) {
		pr_err("CPR disabled by modem\n");
		return -EPERM;
	}

	cpr = devm_kzalloc(&pdev->dev, sizeof(struct msm_cpr), GFP_KERNEL);
	if (!cpr) {
		enable = false;
		return -ENOMEM;
	}

	virt_start_ptr = ioremap_nocache(MSM8625_NON_CACHE_MEM, SZ_2K);
	msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
		"virt_start_ptr = %x\n", (uint32_t) virt_start_ptr);
	memset(virt_start_ptr, 0x0, SZ_2K);

	
	if (!pdata->clk_enable) {
		pr_err("CPR: Invalid clk_enable hook\n");
		return -EFAULT;
	}

	pdata->clk_enable();

	
	cpr->config = pdata;

	if (nom_Vmin != 0)
		cpr->config->cpr_mode_data->nom_Vmin = nom_Vmin;
	if (turbo_Vmin != 0)
		cpr->config->cpr_mode_data->turbo_Vmin = turbo_Vmin;
	if (max_quot != 0)
		cpr->config->max_quot = max_quot;

	
	cpr->cur_Vmin = cpr->config->cpr_mode_data->turbo_Vmin;
	cpr->cur_Vmax = cpr->config->cpr_mode_data->turbo_Vmax;

	cpr_pdev = pdev;
	msm_cpr = cpr;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem || !mem->start) {
		pr_err("CPR: get resource failed\n");
		res = -ENXIO;
		goto out;
	}

	base = ioremap_nocache(mem->start, resource_size(mem));
	if (!base) {
		pr_err("CPR: ioremap failed\n");
		res = -ENOMEM;
		goto out;
	}

	if (cpr->config->irq_line < 0) {
		pr_err("CPR: Invalid IRQ line specified\n");
		res = -ENXIO;
		goto err_ioremap;
	}
	irqn = platform_get_irq(pdev, cpr->config->irq_line);
	if (irqn < 0) {
		pr_err("CPR: Unable to get irq\n");
		res = -ENXIO;
		goto err_ioremap;
	}

	cpr->irq = irqn;

	cpr->base = base;

	cpr->step_size = pdata->step_size;

	spin_lock_init(&cpr->cpr_lock);

	
	cpr->vreg_cx = ncp6335d_handle;

	if (IS_ERR(cpr->vreg_cx)) {
		res = PTR_ERR(cpr->vreg_cx);
		pr_err("could not get regulator: %d\n", res);
		goto err_reg_get;
	}

	
	res = cpr_config(cpr);
	if (res) {
		pr_err("CPR: configuration failed %d\n", res);
		goto err_cpr_config;
	}

	platform_set_drvdata(pdev, cpr);

	chip_data = cpr->config->cpr_mode_data;
	msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
		"CPR Platform Data (upside_steps: %d) (downside_steps: %d))",
		cpr->config->up_threshold, cpr->config->dn_threshold);
	msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
		"(default calibrated voltage: %duV)\n",
		cpr->config->cpr_mode_data->calibrated_uV);
	msm_cpr_debug(MSM_CPR_DEBUG_CONFIG,
		"(gcnt_target: %d) (quot: %d)\n",
		chip_data->ring_osc_data[chip_data->ring_osc].gcnt,
		chip_data->ring_osc_data[chip_data->ring_osc].quot);

	
	res = msm_cpr_debug_init(cpr->base);
	if (res) {
		pr_err("CPR: Debugfs Creation Failed\n");
		goto err_ioremap;
	}

	
	res = request_threaded_irq(irqn, NULL, cpr_irq0_handler,
			IRQF_TRIGGER_RISING, "msm-cpr-irq0", cpr);
	if (res) {
		pr_err("CPR: request irq failed for IRQ %d\n", irqn);
		goto err_ioremap;
	}

	irq_enabled = INT_MASK & ~MID_INT;
	cpr_modify_reg(cpr, RBIF_IRQ_EN(cpr->config->irq_line),
			INT_MASK, irq_enabled);

	
	cpr_modify_reg(cpr, RBCPR_CTL, LOOP_EN_M, ENABLE_CPR);

	cpr->freq_transition.notifier_call = cpr_freq_transition;
	cpufreq_register_notifier(&cpr->freq_transition,
					CPUFREQ_TRANSITION_NOTIFIER);

	disable_cpr = false;
	pr_info("CPR: driver registered successfully\n");

	return res;

err_cpr_config:
err_reg_get:
	free_irq(irqn, cpr);
err_ioremap:
	iounmap(base);
out:
	enable = false;

	
	if (pdata->cpr_reset)
		pdata->cpr_reset();

	return res;
}

static int __devexit msm_cpr_remove(struct platform_device *pdev)
{
	struct msm_cpr *cpr = platform_get_drvdata(pdev);

	cpufreq_unregister_notifier(&cpr->freq_transition,
					CPUFREQ_TRANSITION_NOTIFIER);

	regulator_disable(cpr->vreg_cx);
	regulator_put(cpr->vreg_cx);
	free_irq(cpr->irq, cpr);
	iounmap(cpr->base);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver msm_cpr_driver = {
	.probe = msm_cpr_probe,
	.remove = __devexit_p(msm_cpr_remove),
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init msm_init_cpr(void)
{
	return platform_driver_register(&msm_cpr_driver);
}

module_init(msm_init_cpr);

static void __exit msm_exit_cpr(void)
{
	platform_driver_unregister(&msm_cpr_driver);
}

module_exit(msm_exit_cpr);

MODULE_DESCRIPTION("MSM CPR Driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL v2");
