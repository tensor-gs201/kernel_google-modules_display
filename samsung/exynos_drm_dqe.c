// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Samsung Electronics Co.Ltd
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/of_address.h>
#include <linux/device.h>
#include <drm/drm_drv.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_atomic_helper.h>

#include <dqe_cal.h>
#include <decon_cal.h>
#include <regs-dqe.h>

#include "exynos_drm_decon.h"

static void
exynos_atc_update(struct exynos_dqe *dqe, struct exynos_dqe_state *state)
{
	pr_debug("%s: en(%d), dirty(%d)\n", __func__,
			dqe->force_atc_config.en, dqe->force_atc_config.dirty);

	if (dqe->force_atc_config.dirty) {
		if (dqe->force_atc_config.en) {
			dqe_reg_set_atc(&dqe->force_atc_config);
			dqe->force_atc_config.dirty = false;
		} else {
			dqe_reg_set_atc(NULL);
		}
	}

	if (dqe->verbose_atc)
		dqe_reg_print_atc();
}

static void __exynos_dqe_update(struct exynos_dqe *dqe,
		struct exynos_dqe_state *state, u32 width, u32 height)
{
	const struct decon_device *decon;
	struct dither_config dither_config;

	pr_debug("%s: enabled(%d) +\n", __func__, state->enabled);

	dqe->state.enabled = state->enabled && !dqe->force_disabled;

	decon_reg_set_dqe_enable(0, dqe->state.enabled);
	if (!dqe->state.enabled)
		return;

	if (!dqe->initialized) {
		dqe_reg_init(width, height);
		dqe->initialized = true;
	}

	if (dqe->force_lm)
		state->linear_matrix = &dqe->force_linear_matrix;

	if (dqe->force_gm)
		state->gamma_matrix = &dqe->force_gamma_matrix;

	if (dqe->cgc_dither_override.force_en) {
		dqe_reg_set_cgc_dither(&dqe->cgc_dither_override.val);
		dqe->state.cgc_dither_config = &dqe->cgc_dither_override.val;
	} else if (dqe->state.cgc_dither_config != state->cgc_dither_config) {
		dqe_reg_set_cgc_dither(state->cgc_dither_config);
		dqe->state.cgc_dither_config = state->cgc_dither_config;
	}

	if (dqe->cgc_dither_override.verbose)
		dqe_reg_print_dither(CGC_DITHER);

	if (dqe->disp_dither_override.force_en) {
		dqe_reg_set_disp_dither(&dqe->disp_dither_override.val);
		dqe->state.disp_dither_config = &dqe->disp_dither_override.val;
	} else if (!state->disp_dither_config) {
		decon = dqe->decon;
		memset(&dither_config, 0, sizeof(dither_config));
		if (decon->config.in_bpc == 10 && decon->config.out_bpc == 8)
			dither_config.en = DITHER_EN(1);
		else
			dither_config.en = DITHER_EN(0);

		dqe_reg_set_disp_dither(&dither_config);
		dqe->state.disp_dither_config = NULL;
	} else if (dqe->state.disp_dither_config != state->disp_dither_config) {
		dqe_reg_set_disp_dither(state->disp_dither_config);
		dqe->state.disp_dither_config = state->disp_dither_config;
	}

	if (dqe->disp_dither_override.verbose)
		dqe_reg_print_dither(DISP_DITHER);

	if (dqe->state.degamma_lut != state->degamma_lut) {
		dqe_reg_set_degamma_lut(state->degamma_lut);
		dqe->state.degamma_lut = state->degamma_lut;
	}

	if (dqe->state.cgc_lut != state->cgc_lut) {
		dqe_reg_set_cgc_lut(state->cgc_lut);
		dqe->state.cgc_lut = state->cgc_lut;
		dqe->cgc_first_write = true;
	} else if (dqe->cgc_first_write) {
		dqe_reg_set_cgc_lut(dqe->state.cgc_lut);
		dqe->cgc_first_write = false;
	}

	if (dqe->state.linear_matrix != state->linear_matrix) {
		dqe_reg_set_linear_matrix(state->linear_matrix);
		dqe->state.linear_matrix = state->linear_matrix;
	}

	if (dqe->state.gamma_matrix != state->gamma_matrix) {
		dqe_reg_set_gamma_matrix(state->gamma_matrix);
		dqe->state.gamma_matrix = state->gamma_matrix;
	}

	if (dqe->state.regamma_lut != state->regamma_lut) {
		dqe_reg_set_regamma_lut(state->regamma_lut);
		dqe->state.regamma_lut = state->regamma_lut;
	}

	exynos_atc_update(dqe, state);

	/*
	 * Currently, the parameter of this function is fixed to zero because
	 * DECON0 only supports DQE. If other DECONs support DQE in the future,
	 * it needs to be modified.
	 */
	decon_reg_update_req_dqe(0);

	pr_debug("%s -\n", __func__);
}

static const struct exynos_dqe_funcs dqe_funcs = {
	.update = __exynos_dqe_update,
};

void exynos_dqe_update(struct exynos_dqe *dqe, struct exynos_dqe_state *state,
		u32 width, u32 height)
{
	dqe->funcs->update(dqe, state, width, height);
}

void exynos_dqe_reset(struct exynos_dqe *dqe)
{
	dqe->initialized = false;
	dqe->state.gamma_matrix = NULL;
	dqe->state.degamma_lut = NULL;
	dqe->state.linear_matrix = NULL;
	dqe->state.cgc_lut = NULL;
	dqe->state.regamma_lut = NULL;
	dqe->state.disp_dither_config = NULL;
	dqe->state.cgc_dither_config = NULL;
	dqe->cgc_first_write = false;
	dqe->force_atc_config.dirty = true;
}

void exynos_dqe_save_lpd_data(struct exynos_dqe *dqe)
{
	if (!dqe)
		return;

	if (dqe->force_atc_config.en)
		dqe_reg_save_lpd_atc(dqe->lpd_atc_regs);
}

void exynos_dqe_restore_lpd_data(struct exynos_dqe *dqe)
{
	if (!dqe)
		return;

	if (dqe->force_atc_config.en)
		dqe_reg_restore_lpd_atc(dqe->lpd_atc_regs);
}

static void set_default_atc_config(struct exynos_atc *atc)
{
	atc->dirty = true;
	atc->lt = 0x80;
	atc->ns = 0x80;
	atc->st = 0x80;
	atc->dither = false;
	atc->pl_w1 = 0xA;
	atc->pl_w2 = 0xE;
	atc->ctmode = 0x2;
	atc->pp_en = true;
	atc->upgrade_on = 0;
	atc->tdr_max = 0x384;
	atc->tdr_min = 0x100;
	atc->ambient_light = 0x8C;
	atc->back_light = 0xFF;
	atc->dstep = 0x4;
	atc->scale_mode = 0x1;
	atc->threshold_1 = 0x1;
	atc->threshold_2 = 0x1;
	atc->threshold_3 = 0x1;
	atc->gain_limit = 0x1FF;
	atc->lt_calc_ab_shift = 0x1;
}

static ssize_t
atc_u8_store(struct exynos_dqe *dqe, u8 *val, const char *buf, size_t count)
{
	int ret;

	ret = kstrtou8(buf, 0, val);
	if (ret)
		return ret;

	dqe->force_atc_config.dirty = true;

	return count;
}

static ssize_t
atc_u16_store(struct exynos_dqe *dqe, u16 *val, const char *buf, size_t count)
{
	int ret;

	ret = kstrtou16(buf, 0, val);
	if (ret)
		return ret;

	dqe->force_atc_config.dirty = true;

	return count;
}

static ssize_t
atc_bool_store(struct exynos_dqe *dqe, bool *val, const char *buf, size_t count)
{
	if (kstrtobool(buf, val))
		return -EINVAL;

	dqe->force_atc_config.dirty = true;

	return count;
}

#define DQE_ATC_ATTR_RW(_name, _save, _fmt)	\
static ssize_t _name##_store(struct device *dev,	\
		struct device_attribute *attr, const char *buf, size_t count) \
{	\
	struct exynos_dqe *dqe = dev_get_drvdata(dev);	\
	return _save(dqe, &dqe->force_atc_config._name, buf, count);	\
}	\
static ssize_t _name##_show(struct device *dev,	\
		struct device_attribute *attr, char *buf)	\
{	\
	struct exynos_dqe *dqe = dev_get_drvdata(dev);	\
	return snprintf(buf, PAGE_SIZE, _fmt "\n",	\
			dqe->force_atc_config._name);	\
}	\
static DEVICE_ATTR_RW(_name)

#define DQE_ATC_ATTR_U8_RW(_name) DQE_ATC_ATTR_RW(_name, atc_u8_store, "%u")
#define DQE_ATC_ATTR_U16_RW(_name) DQE_ATC_ATTR_RW(_name, atc_u16_store, "%u")
#define DQE_ATC_ATTR_BOOL_RW(_name) DQE_ATC_ATTR_RW(_name, atc_bool_store, "%d")

DQE_ATC_ATTR_BOOL_RW(en);
DQE_ATC_ATTR_U8_RW(lt);
DQE_ATC_ATTR_U8_RW(ns);
DQE_ATC_ATTR_U8_RW(st);
DQE_ATC_ATTR_BOOL_RW(dither);
DQE_ATC_ATTR_U8_RW(pl_w1);
DQE_ATC_ATTR_U8_RW(pl_w2);
DQE_ATC_ATTR_U8_RW(ctmode);
DQE_ATC_ATTR_BOOL_RW(pp_en);
DQE_ATC_ATTR_U8_RW(upgrade_on);
DQE_ATC_ATTR_U16_RW(tdr_max);
DQE_ATC_ATTR_U16_RW(tdr_min);
DQE_ATC_ATTR_U8_RW(ambient_light);
DQE_ATC_ATTR_U8_RW(back_light);
DQE_ATC_ATTR_U8_RW(dstep);
DQE_ATC_ATTR_U8_RW(scale_mode);
DQE_ATC_ATTR_U8_RW(threshold_1);
DQE_ATC_ATTR_U8_RW(threshold_2);
DQE_ATC_ATTR_U8_RW(threshold_3);
DQE_ATC_ATTR_U16_RW(gain_limit);
DQE_ATC_ATTR_U8_RW(lt_calc_ab_shift);

static ssize_t force_update_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct exynos_dqe *dqe = dev_get_drvdata(dev);
	struct decon_device *decon = dqe->decon;
	struct drm_crtc *crtc = &decon->crtc->base;
	struct drm_device *drm_dev = decon->drm_dev;
	struct drm_atomic_state *state;
	struct drm_crtc_state *crtc_state;
	struct drm_modeset_acquire_ctx ctx;
	int ret = 0;

	dqe->force_atc_config.dirty = true;

	state = drm_atomic_state_alloc(drm_dev);
	if (!state)
		return -ENOMEM;
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
retry:

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}
	ret = drm_atomic_commit(state);
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret ? : count;
}
static DEVICE_ATTR_WO(force_update);

static struct attribute *atc_attrs[] = {
	&dev_attr_force_update.attr,
	&dev_attr_en.attr,
	&dev_attr_lt.attr,
	&dev_attr_ns.attr,
	&dev_attr_st.attr,
	&dev_attr_dither.attr,
	&dev_attr_pl_w1.attr,
	&dev_attr_pl_w2.attr,
	&dev_attr_ctmode.attr,
	&dev_attr_pp_en.attr,
	&dev_attr_upgrade_on.attr,
	&dev_attr_tdr_max.attr,
	&dev_attr_tdr_min.attr,
	&dev_attr_ambient_light.attr,
	&dev_attr_back_light.attr,
	&dev_attr_dstep.attr,
	&dev_attr_scale_mode.attr,
	&dev_attr_threshold_1.attr,
	&dev_attr_threshold_2.attr,
	&dev_attr_threshold_3.attr,
	&dev_attr_gain_limit.attr,
	&dev_attr_lt_calc_ab_shift.attr,
	NULL,
};
ATTRIBUTE_GROUPS(atc);

extern u32 gs_chipid_get_type(void);
struct exynos_dqe *exynos_dqe_register(struct decon_device *decon)
{
	struct device *dev = decon->dev;
	struct device_node *np = dev->of_node;
	struct exynos_dqe *dqe;
	enum dqe_version dqe_version;
	int i;

	i = of_property_match_string(np, "reg-names", "dqe");
	if (i < 0) {
		pr_info("display quality enhancer is not supported\n");
		return NULL;
	}

	dqe = devm_kzalloc(dev, sizeof(struct exynos_dqe), GFP_KERNEL);
	if (!dqe)
		return NULL;

	dqe->regs = of_iomap(np, i);
	if (IS_ERR(dqe->regs)) {
		pr_err("failed to remap dqe registers\n");
		return NULL;
	}

	dqe_version = gs_chipid_get_type() ? DQE_V2 : DQE_V1;
	dqe_regs_desc_init(dqe->regs, "dqe", dqe_version);
	dqe->funcs = &dqe_funcs;
	dqe->initialized = false;
	dqe->decon = decon;

	dqe->dqe_class = class_create(THIS_MODULE, "dqe");
	if (IS_ERR(dqe->dqe_class)) {
		pr_err("failed to create dqe class\n");
		return NULL;
	}

	dqe->dqe_class->dev_groups = atc_groups;
	dqe->dev = device_create(dqe->dqe_class, dev, 0, dqe, "atc");
	if (IS_ERR(dqe->dev)) {
		pr_err("failed to create to atc sysfs device\n");
		return NULL;
	}

	set_default_atc_config(&dqe->force_atc_config);

	pr_info("display quality enhancer is supported(DQE_V%d)\n",
			dqe_version + 1);

	return dqe;
}
