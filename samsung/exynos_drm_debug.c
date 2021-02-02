// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * DPU Event log file for Samsung EXYNOS DPU driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/pm_runtime.h>
#include <linux/time.h>
#include <video/mipi_display.h>
#include <drm/drm_print.h>

#include <cal_config.h>
#include <dt-bindings/soc/google/gs101-devfreq.h>
#include <soc/google/exynos-devfreq.h>

#include "exynos_drm_decon.h"
#include "exynos_drm_dsim.h"
#include "exynos_drm_writeback.h"

/* Default is 1024 entries array for event log buffer */
static unsigned int dpu_event_log_max = 1024;
static unsigned int dpu_event_print_max = 512;

module_param_named(event_log_max, dpu_event_log_max, uint, 0);
module_param_named(event_print_max, dpu_event_print_max, uint, 0600);
MODULE_PARM_DESC(event_log_max, "entry count of event log buffer array");
MODULE_PARM_DESC(event_print_max, "print entry count of event log buffer");

/* If event are happened continuously, then ignore */
static bool dpu_event_ignore
	(enum dpu_event_type type, struct decon_device *decon)
{
	int latest = atomic_read(&decon->d.event_log_idx) % dpu_event_log_max;
	int idx, offset;

	if (IS_ERR_OR_NULL(decon->d.event_log))
		return true;

	for (offset = 0; offset < DPU_EVENT_KEEP_CNT; ++offset) {
		idx = (latest + dpu_event_log_max - offset) % dpu_event_log_max;
		if (type != decon->d.event_log[idx].type)
			return false;
	}

	return true;
}

static void dpu_event_save_freqs(struct dpu_log_freqs *freqs)
{
	freqs->mif_freq = exynos_devfreq_get_domain_freq(DEVFREQ_MIF);
	freqs->int_freq = exynos_devfreq_get_domain_freq(DEVFREQ_INT);
	freqs->disp_freq = exynos_devfreq_get_domain_freq(DEVFREQ_DISP);
}

/* ===== EXTERN APIs ===== */

/*
 * DPU_EVENT_LOG() - store information to log buffer by common API
 * @type: event type
 * @index: event log index
 * @priv: pointer to DECON, DSIM or DPP device structure
 *
 * Store information related to DECON, DSIM or DPP. Each DECON has event log
 * So, DECON id is used as @index
 */
void DPU_EVENT_LOG(enum dpu_event_type type, int index, void *priv)
{
	struct decon_device *decon = NULL;
	struct dpp_device *dpp = NULL;
	struct dpu_log *log;
	struct drm_crtc_state *crtc_state;
	unsigned long flags;
	int idx;
	bool skip_excessive = true;

	if (index < 0) {
		DRM_ERROR("%s: decon id is not valid(%d)\n", __func__, index);
		return;
	}

	decon = get_decon_drvdata(index);
	if (IS_ERR_OR_NULL(decon->d.event_log))
		return;

	switch (type) {
	case DPU_EVT_DECON_FRAMESTART:
		decon->d.auto_refresh_frames++;
	case DPU_EVT_DECON_FRAMEDONE:
	case DPU_EVT_DPP_FRAMEDONE:
	case DPU_EVT_DSIM_FRAMEDONE:
		if (decon->d.auto_refresh_frames > 3)
			return;
		break;
	case DPU_EVT_TE_INTERRUPT:
		break;
	case DPU_EVT_DSIM_UNDERRUN:
		decon->d.underrun_cnt++;
		break;
	case DPU_EVT_DSIM_CRC:
		decon->d.crc_cnt++;
		break;
	case DPU_EVT_DSIM_ECC:
		decon->d.ecc_cnt++;
		break;
	default:
		skip_excessive = false;
		break;
	}

	/*
	 * If the same event occurs DPU_EVENT_KEEP_CNT times
	 * continuously, it will be skipped.
	 */
	if (skip_excessive && dpu_event_ignore(type, decon))
		return;

	spin_lock_irqsave(&decon->d.event_lock, flags);
	idx = atomic_inc_return(&decon->d.event_log_idx) % dpu_event_log_max;
	log = &decon->d.event_log[idx];
	spin_unlock_irqrestore(&decon->d.event_lock, flags);

	log->time = ktime_get();
	log->type = type;

	switch (type) {
	case DPU_EVT_DPP_FRAMEDONE:
		dpp = (struct dpp_device *)priv;
		log->data.dpp.id = dpp->id;
		break;
	case DPU_EVT_DMA_RECOVERY:
		dpp = (struct dpp_device *)priv;
		log->data.dpp.id = dpp->id;
		log->data.dpp.comp_src = dpp->comp_src;
		log->data.dpp.recovery_cnt = dpp->recovery_cnt;
		break;
	case DPU_EVT_DECON_RSC_OCCUPANCY:
		pm_runtime_get_sync(decon->dev);
		log->data.rsc.rsc_ch = decon_reg_get_rsc_ch(decon->id);
		log->data.rsc.rsc_win = decon_reg_get_rsc_win(decon->id);
		pm_runtime_put_sync(decon->dev);
		break;
	case DPU_EVT_ENTER_HIBERNATION_IN:
	case DPU_EVT_ENTER_HIBERNATION_OUT:
	case DPU_EVT_EXIT_HIBERNATION_IN:
	case DPU_EVT_EXIT_HIBERNATION_OUT:
		log->data.pd.rpm_active = pm_runtime_active(decon->dev);
		break;
	case DPU_EVT_PLANE_UPDATE:
	case DPU_EVT_PLANE_DISABLE:
		dpp = (struct dpp_device *)priv;
		log->data.win.win_idx = dpp->win_id;
		log->data.win.plane_idx = dpp->id;
		break;
	case DPU_EVT_REQ_CRTC_INFO_OLD:
	case DPU_EVT_REQ_CRTC_INFO_NEW:
		crtc_state = (struct drm_crtc_state *)priv;
		log->data.crtc_info.enable = crtc_state->enable;
		log->data.crtc_info.active = crtc_state->active;
		log->data.crtc_info.planes_changed = crtc_state->planes_changed;
		log->data.crtc_info.mode_changed = crtc_state->mode_changed;
		log->data.crtc_info.active_changed = crtc_state->active_changed;
		break;
	case DPU_EVT_BTS_RELEASE_BW:
	case DPU_EVT_BTS_UPDATE_BW:
		dpu_event_save_freqs(&log->data.freqs);
		break;
	case DPU_EVT_BTS_CALC_BW:
		dpu_event_save_freqs(&log->data.bts_event.freqs);
		log->data.bts_event.value = decon->bts.max_disp_freq;
		break;
	case DPU_EVT_DSIM_UNDERRUN:
		dpu_event_save_freqs(&log->data.bts_event.freqs);
		log->data.bts_event.value = decon->d.underrun_cnt;
		break;
	case DPU_EVT_DSIM_CRC:
		log->data.value = decon->d.crc_cnt;
		break;
	case DPU_EVT_DSIM_ECC:
		log->data.value = decon->d.ecc_cnt;
		break;
	default:
		break;
	}
}

/*
 * DPU_EVENT_LOG_ATOMIC_COMMIT() - store all windows information
 * @index: event log index
 *
 * Store all windows information which includes window id, DVA, source and
 * destination coordinates, connected DPP and so on
 */
void DPU_EVENT_LOG_ATOMIC_COMMIT(int index)
{
	struct decon_device *decon;
	struct dpu_log *log;
	unsigned long flags;
	int idx, i, dpp_ch;

	if (index < 0) {
		DRM_ERROR("%s: decon id is not valid(%d)\n", __func__, index);
		return;
	}

	decon = get_decon_drvdata(index);

	if (IS_ERR_OR_NULL(decon->d.event_log))
		return;

	spin_lock_irqsave(&decon->d.event_lock, flags);
	idx = atomic_inc_return(&decon->d.event_log_idx) % dpu_event_log_max;
	log = &decon->d.event_log[idx];
	spin_unlock_irqrestore(&decon->d.event_lock, flags);

	log->type = DPU_EVT_ATOMIC_COMMIT;
	log->time = ktime_get();

	decon->d.auto_refresh_frames = 0;

	for (i = 0; i < MAX_WIN_PER_DECON; ++i) {
		memcpy(&log->data.atomic.win_config[i].win,
				&decon->bts.win_config[i],
				sizeof(struct dpu_bts_win_config));

		if (decon->bts.win_config[i].state == DPU_WIN_STATE_BUFFER) {
			dpp_ch = decon->bts.win_config[i].dpp_ch;

			log->data.atomic.win_config[i].dma_addr =
				decon->dpp[dpp_ch]->dbg_dma_addr;
		}
	}
}

extern void *return_address(int);

/*
 * DPU_EVENT_LOG_CMD() - store DSIM command information
 * @index: event log index
 * @dsim: pointer to dsim device structure
 * @type : DSIM command id
 * @d0: data0 in command buffer
 * @len: length of payload
 *
 * Stores command id and first data in command buffer and return addresses
 * in callstack which lets you know who called this function.
 */
void
DPU_EVENT_LOG_CMD(struct dsim_device *dsim, u8 type, u8 d0, u16 len)
{
	struct decon_device *decon = (struct decon_device *)dsim_get_decon(dsim);
	struct dpu_log *log;
	unsigned long flags;
	int idx, i;

	if (!decon) {
		pr_err("%s: invalid decon\n", __func__);
		return;
	}

	if (IS_ERR_OR_NULL(decon->d.event_log))
		return;

	spin_lock_irqsave(&decon->d.event_lock, flags);
	idx = atomic_inc_return(&decon->d.event_log_idx) % dpu_event_log_max;
	log = &decon->d.event_log[idx];
	spin_unlock_irqrestore(&decon->d.event_lock, flags);

	log->type = DPU_EVT_DSIM_COMMAND;
	log->time = ktime_get();

	log->data.cmd.id = type;
	log->data.cmd.d0 = d0;
	log->data.cmd.len = len;

	for (i = 0; i < DPU_CALLSTACK_MAX; i++)
		log->data.cmd.caller[i] =
			(void *)((size_t)return_address(i + 1));
}

static void dpu_print_log_atomic(struct dpu_log_atomic *atomic,
						struct drm_printer *p)
{
	int i;
	struct dpu_bts_win_config *win;
	char *str_state[3] = {"DISABLED", "COLOR", "BUFFER"};
	const char *str_comp;
	const struct dpu_fmt *fmt;
	char buf[128];
	int len;

	for (i = 0; i < MAX_WIN_PER_DECON; ++i) {
		win = &atomic->win_config[i].win;

		if (win->state == DPU_WIN_STATE_DISABLED)
			continue;

		fmt = dpu_find_fmt_info(win->format);

		len = scnprintf(buf, sizeof(buf),
				"\t\t\t\t\tWIN%d: %s[0x%llx] SRC[%d %d %d %d] ",
				i, str_state[win->state],
				(win->state == DPU_WIN_STATE_BUFFER) ?
				atomic->win_config[i].dma_addr : 0,
				win->src_x, win->src_y, win->src_w, win->src_h);
		len += scnprintf(buf + len, sizeof(buf) - len,
				"DST[%d %d %d %d] ",
				win->dst_x, win->dst_y, win->dst_w, win->dst_h);
		if (win->state == DPU_WIN_STATE_BUFFER)
			len += scnprintf(buf + len, sizeof(buf) - len, "CH%d ",
					win->dpp_ch);

		str_comp = get_comp_src_name(win->comp_src);
		drm_printf(p, "%s %s %s\n", buf, fmt->name, str_comp);
	}
}

static void dpu_print_log_rsc(char *buf, int len, struct dpu_log_rsc_occupancy *rsc)
{
	int i, len_chs, len_wins;
	char str_chs[128];
	char str_wins[128];
	bool using_ch, using_win;

	len_chs = sprintf(str_chs, "CHs: ");
	len_wins = sprintf(str_wins, "WINs: ");

	for (i = 0; i < MAX_PLANE; ++i) {
		using_ch = is_decon_using_ch(0, rsc->rsc_ch, i);
		len_chs += sprintf(str_chs + len_chs, "%d[%c] ", i,
				using_ch ? 'O' : 'X');

		using_win = is_decon_using_win(0, rsc->rsc_win, i);
		len_wins += sprintf(str_wins + len_wins, "%d[%c] ", i,
				using_win ? 'O' : 'X');
	}

	sprintf(buf + len, "\t%s\t%s", str_chs, str_wins);
}

#define LOG_BUF_SIZE	128
static int dpu_print_log_freqs(char *buf, int len, struct dpu_log_freqs *freqs)
{
	return scnprintf(buf + len, LOG_BUF_SIZE - len,
			"\tmif(%lu) int(%lu) disp(%lu)",
			freqs->mif_freq, freqs->int_freq, freqs->disp_freq);
}

static const char *get_event_name(enum dpu_event_type type)
{
	static const char events[][32] = {
		"NONE",				"DECON_ENABLED",
		"DECON_DISABLED",		"DECON_FRAMEDONE",
		"DECON_FRAMESTART",		"DECON_RSC_OCCUPANCY",
		"DECON_TRIG_MASK",		"DSIM_ENABLED",
		"DSIM_DISABLED",		"DSIM_COMMAND",
		"DSIM_UNDERRUN",		"DSIM_FRAMEDONE",
		"DPP_FRAMEDONE",		"DMA_RECOVERY",
		"ATOMIC_COMMIT",		"TE_INTERRUPT",
		"ENTER_HIBERNATION_IN",		"ENTER_HIBERNATION_OUT",
		"EXIT_HIBERNATION_IN",		"EXIT_HIBERNATION_OUT",
		"ATOMIC_BEGIN",			"ATOMIC_FLUSH",
		"WB_ENABLE",			"WB_DISABLE",
		"WB_ATOMIC_COMMIT",		"WB_FRAMEDONE",
		"WB_ENTER_HIBERNATION",		"WB_EXIT_HIBERNATION",
		"PLANE_UPDATE",			"PLANE_DISABLE",
		"REQ_CRTC_INFO_OLD",		"REQ_CRTC_INFO_NEW",
		"FRAMESTART_TIMEOUT",
		"BTS_RELEASE_BW",		"BTS_CALC_BW",
		"BTS_UPDATE_BW",		"DSIM_CRC",
		"DSIM_ECC",			"VBLANK_ENABLE",
		"VBLANK_DISABLE",		"DIMMING_START",
		"DIMMING_END",
	};

	if (type >= DPU_EVT_MAX)
		return NULL;

	return events[type];
}

static void dpu_event_log_print(const struct decon_device *decon, struct drm_printer *p,
				size_t max_logs)
{
	int idx = atomic_read(&decon->d.event_log_idx);
	struct dpu_log *log;
	int latest = idx % dpu_event_log_max;
	struct timespec64 ts;
	const char *str_comp;
	char buf[LOG_BUF_SIZE];
	int len;

	if (IS_ERR_OR_NULL(decon->d.event_log))
		return;

	drm_printf(p, "----------------------------------------------------\n");
	drm_printf(p, "%14s  %20s  %20s\n", "Time", "Event ID", "Remarks");
	drm_printf(p, "----------------------------------------------------\n");

	/* Seek a oldest from current index */
	if (max_logs > dpu_event_log_max)
		max_logs = dpu_event_log_max;

	if (idx < max_logs)
		idx = 0;
	else
		idx = (idx - max_logs) % dpu_event_log_max;

	do {
		if (++idx >= dpu_event_log_max)
			idx = 0;

		/* Seek a index */
		log = &decon->d.event_log[idx];

		/* TIME */
		ts = ktime_to_timespec64(log->time);

		/* If there is no timestamp, then exit directly */
		if (!ts.tv_sec)
			break;

		len = scnprintf(buf, sizeof(buf), "[%6lld.%06ld] %20s", ts.tv_sec,
				ts.tv_nsec / NSEC_PER_USEC, get_event_name(log->type));

		switch (log->type) {
		case DPU_EVT_DECON_RSC_OCCUPANCY:
			dpu_print_log_rsc(buf, len, &log->data.rsc);
			break;
		case DPU_EVT_DSIM_COMMAND:
			scnprintf(buf + len, sizeof(buf) - len,
					"\tCMD_ID: 0x%x\tDATA[0]: 0x%x len: %d",
					log->data.cmd.id, log->data.cmd.d0,
					log->data.cmd.len);
			break;
		case DPU_EVT_DPP_FRAMEDONE:
			scnprintf(buf + len, sizeof(buf) - len,
					"\tID:%d", log->data.dpp.id);
			break;
		case DPU_EVT_DMA_RECOVERY:
			str_comp = get_comp_src_name(log->data.dpp.comp_src);
			scnprintf(buf + len, sizeof(buf) - len,
					"\tID:%d SRC:%s COUNT:%d",
					log->data.dpp.id, str_comp,
					log->data.dpp.recovery_cnt);
			break;
		case DPU_EVT_ENTER_HIBERNATION_IN:
		case DPU_EVT_ENTER_HIBERNATION_OUT:
		case DPU_EVT_EXIT_HIBERNATION_IN:
		case DPU_EVT_EXIT_HIBERNATION_OUT:
			scnprintf(buf + len, sizeof(buf) - len,
					"\tDPU POWER %s",
					log->data.pd.rpm_active ? "ON" : "OFF");
			break;
		case DPU_EVT_PLANE_UPDATE:
		case DPU_EVT_PLANE_DISABLE:
			scnprintf(buf + len, sizeof(buf) - len,
					"\tCH:%d, WIN:%d",
					log->data.win.plane_idx,
					log->data.win.win_idx);
			break;
		case DPU_EVT_REQ_CRTC_INFO_OLD:
		case DPU_EVT_REQ_CRTC_INFO_NEW:
			scnprintf(buf + len, sizeof(buf) - len,
				"\tenable(%d) active(%d) [p:%d m:%d a:%d]",
					log->data.crtc_info.enable,
					log->data.crtc_info.active,
					log->data.crtc_info.planes_changed,
					log->data.crtc_info.mode_changed,
					log->data.crtc_info.active_changed);
			break;
		case DPU_EVT_BTS_RELEASE_BW:
		case DPU_EVT_BTS_UPDATE_BW:
			dpu_print_log_freqs(buf, len, &log->data.freqs);
			break;
		case DPU_EVT_BTS_CALC_BW:
			scnprintf(buf + len, sizeof(buf) - len,
					"\tcalculated disp(%u)",
					log->data.bts_event.value);
			break;
		case DPU_EVT_DSIM_UNDERRUN:
			scnprintf(buf + len, sizeof(buf) - len,
					"\tunderrun count(%u)",
					log->data.bts_event.value);
			break;
		case DPU_EVT_DSIM_CRC:
			scnprintf(buf + len, sizeof(buf) - len,
					"\tcrc count(%u)",
					log->data.value);
			break;
		case DPU_EVT_DSIM_ECC:
			scnprintf(buf + len, sizeof(buf) - len,
					"\tecc count(%u)",
					log->data.value);
			break;
		default:
			break;
		}

		drm_printf(p, "%s\n", buf);

		switch (log->type) {
		case DPU_EVT_ATOMIC_COMMIT:
			dpu_print_log_atomic(&log->data.atomic, p);
			break;
		default:
			break;
		}
	} while (latest != idx);

	drm_printf(p, "----------------------------------------------------\n");
}

static int dpu_debug_event_show(struct seq_file *s, void *unused)
{
	struct decon_device *decon = s->private;
	struct drm_printer p = drm_seq_file_printer(s);

	dpu_event_log_print(decon, &p, dpu_event_log_max);
	return 0;
}

static int dpu_debug_event_open(struct inode *inode, struct file *file)
{
	return single_open(file, dpu_debug_event_show, inode->i_private);
}

static const struct file_operations dpu_event_fops = {
	.open = dpu_debug_event_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static struct dentry *exynos_debugfs_add_dqe_override(const char *name,
			struct dither_debug_override *d, struct dentry *parent)
{
	struct dentry *dent;

	dent = debugfs_create_dir(name, parent);
	if (!dent) {
		pr_err("failed to create %s dir\n", name);
		return NULL;
	}
	debugfs_create_bool("force_enable", 0664, dent, &d->force_en);
	debugfs_create_bool("verbose", 0664, dent, &d->verbose);
	debugfs_create_u32("val", 0664, dent, (u32 *)&d->val);

	return dent;
}

static int get_lut(char *lut_buf, u32 count, u32 pcount, void **lut,
						enum elem_size elem_size)
{
	int i = 0, ret = 0;
	char *token;
	u16 *plut16;
	u32 *plut32;

	if (elem_size == ELEM_SIZE_16)
		plut16 = *lut;
	else if (elem_size == ELEM_SIZE_32)
		plut32 = *lut;
	else
		return -EINVAL;

	if (!pcount || pcount > count)
		pcount = count;

	while ((token = strsep(&lut_buf, " "))) {
		if (i >= count)
			break;

		if (elem_size == ELEM_SIZE_16)
			ret = kstrtou16(token, 0, &plut16[i++]);
		else if (elem_size == ELEM_SIZE_32)
			ret = kstrtou32(token, 0, &plut32[i++]);

		if (ret)
			return -EINVAL;
	}

	return 0;
}

static int lut_show(struct seq_file *s, void *unused)
{
	struct debugfs_lut *lut = s->private;
	struct drm_printer p = drm_seq_file_printer(s);
	char buf[128] = {0};
	int len = 0;
	int i;
	u16 *plut16;
	u32 *plut32;

	if (lut->elem_size == ELEM_SIZE_16)
		plut16 = lut->lut_ptr;
	else if (lut->elem_size == ELEM_SIZE_32)
		plut32 = lut->lut_ptr;
	else
		return -EINVAL;

	if (!lut->pcount || lut->pcount > lut->count)
		lut->pcount = lut->count;

	for (i = 0; i < lut->pcount; ++i) {
		if (lut->elem_size == ELEM_SIZE_16)
			len += sprintf(buf + len, "[%2d] %4x  ", i, plut16[i]);
		else if (lut->elem_size == ELEM_SIZE_32)
			len += sprintf(buf + len, "[%2d] %4x  ", i, plut32[i]);

		if ((i % 4) == 3) {
			drm_printf(&p, "%s\n", buf);
			len = 0;
			memset(buf, 0, sizeof(buf));
		}
	}

	if (len)
		drm_printf(&p, "%s\n", buf);

	return 0;
}

static int lut_open(struct inode *inode, struct file *file)
{
	return single_open(file, lut_show, inode->i_private);
}

static ssize_t lut_write(struct file *file, const char __user *buffer,
		size_t len, loff_t *ppos)
{
	char *tmpbuf;
	struct debugfs_lut *lut =
		((struct seq_file *)file->private_data)->private;
	int ret;

	if (len == 0)
		return 0;

	tmpbuf = memdup_user_nul(buffer, len);
	if (IS_ERR(tmpbuf))
		return PTR_ERR(tmpbuf);

	pr_debug("read %d bytes from userspace\n", (int)len);

	ret = get_lut(tmpbuf, lut->count, lut->pcount, &lut->lut_ptr,
				lut->elem_size);
	if (ret)
		goto err;

	ret = len;
err:
	kfree(tmpbuf);

	return ret;
}

static const struct file_operations lut_fops = {
	.open	 = lut_open,
	.read	 = seq_read,
	.write	 = lut_write,
	.llseek	 = seq_lseek,
	.release = seq_release,
};

static void exynos_debugfs_add_lut(const char *name, umode_t mode,
		struct dentry *parent, size_t count, size_t pcount,
		void *lut_ptr, struct drm_color_lut *dlut_ptr,
		enum elem_size elem_size)
{
	struct debugfs_lut *lut = kmalloc(sizeof(struct debugfs_lut),
			GFP_KERNEL);
	if (!lut)
		return;

	if (!lut_ptr) {
		lut_ptr = kmalloc(count * (elem_size >> 3), GFP_KERNEL);
		if (!lut_ptr) {
			kfree(lut);
			return;
		}
	}

	memcpy(lut->name, name, MAX_NAME_SIZE);
	lut->lut_ptr = lut_ptr;
	lut->dlut_ptr = dlut_ptr;
	lut->elem_size = elem_size;
	lut->count = count;
	lut->pcount = pcount;

	debugfs_create_file(name, mode, parent, lut, &lut_fops);
}

static struct dentry *exynos_debugfs_add_matrix(const char *name,
		struct dentry *parent, bool *force_enable, void *coeffs,
		size_t coeffs_cnt, enum elem_size coeffs_elem_size,
		void *offsets, size_t offsets_cnt,
		enum elem_size offsets_elem_size)
{
	struct dentry *dent, *dent_matrix;

	dent = debugfs_create_dir(name, parent);
	if (!dent) {
		pr_err("failed to create %s matrix directory\n", name);
		return NULL;
	}

	debugfs_create_bool("force_enable", 0664, dent, force_enable);
	dent_matrix = debugfs_create_dir("matrix", dent);
	if (!dent_matrix) {
		pr_err("failed to create %s directory\n", name);
		debugfs_remove_recursive(dent);
		return NULL;
	}

	exynos_debugfs_add_lut("coeffs", 0664, dent_matrix, coeffs_cnt, 0,
			coeffs, NULL, coeffs_elem_size);
	exynos_debugfs_add_lut("offsets", 0664, dent_matrix, offsets_cnt, 0,
			offsets, NULL, offsets_elem_size);

	return dent;
}

static void
exynos_debugfs_add_dqe(struct exynos_dqe *dqe, struct dentry *parent)
{
	struct dentry *dent_dir;

	if (!dqe)
		return;

	dent_dir = debugfs_create_dir("dqe", parent);
	if (!dent_dir) {
		pr_err("failed to create dqe directory\n");
		return;
	}

	if (!exynos_debugfs_add_dqe_override("cgc_dither",
			&dqe->cgc_dither_override, dent_dir))
		goto err;

	if (!exynos_debugfs_add_dqe_override("disp_dither",
			&dqe->disp_dither_override, dent_dir))
		goto err;

	if (!exynos_debugfs_add_matrix("linear_matrix", dent_dir,
			&dqe->force_lm, dqe->force_linear_matrix.coeffs,
			DRM_SAMSUNG_MATRIX_DIMENS * DRM_SAMSUNG_MATRIX_DIMENS,
			ELEM_SIZE_16, dqe->force_linear_matrix.offsets,
			DRM_SAMSUNG_MATRIX_DIMENS, ELEM_SIZE_16))
		goto err;

	if (!exynos_debugfs_add_matrix("gamma_matrix", dent_dir,
			&dqe->force_gm, dqe->force_gamma_matrix.coeffs,
			DRM_SAMSUNG_MATRIX_DIMENS * DRM_SAMSUNG_MATRIX_DIMENS,
			ELEM_SIZE_16, dqe->force_gamma_matrix.offsets,
			DRM_SAMSUNG_MATRIX_DIMENS, ELEM_SIZE_16))
		goto err;

	debugfs_create_bool("force_disabled", 0664, dent_dir,
			&dqe->force_disabled);

	return;

err:
	debugfs_remove_recursive(dent_dir);
}

int dpu_init_debug(struct decon_device *decon)
{
	int i;
	u32 event_cnt;
	struct drm_crtc *crtc;
	struct exynos_dqe *dqe = decon->dqe;
	struct dentry *debug_event;
	struct dentry *urgent_dent;

	decon->d.event_log = NULL;
	event_cnt = dpu_event_log_max;

	for (i = 0; i < DPU_EVENT_LOG_RETRY; ++i) {
		event_cnt = event_cnt >> i;
		decon->d.event_log =
			vzalloc(sizeof(struct dpu_log) * event_cnt);
		if (IS_ERR_OR_NULL(decon->d.event_log)) {
			DRM_WARN("failed to alloc event log buf[%d]. retry\n",
					event_cnt);
			continue;
		}

		DRM_INFO("#%d event log buffers are allocated\n", event_cnt);
		break;
	}
	spin_lock_init(&decon->d.event_lock);
	decon->d.event_log_cnt = event_cnt;
	atomic_set(&decon->d.event_log_idx, -1);

	if (!decon->crtc)
		goto err_event_log;

	crtc = &decon->crtc->base;

	debug_event = debugfs_create_file("event", 0444, crtc->debugfs_entry,
			decon, &dpu_event_fops);
	if (!debug_event) {
		DRM_ERROR("failed to create debugfs event file\n");
		goto err_event_log;
	}

	debugfs_create_u32("underrun_cnt", 0664, crtc->debugfs_entry, &decon->d.underrun_cnt);
	debugfs_create_u32("crc_cnt", 0444, crtc->debugfs_entry, &decon->d.crc_cnt);
	debugfs_create_u32("ecc_cnt", 0444, crtc->debugfs_entry, &decon->d.ecc_cnt);

	urgent_dent = debugfs_create_dir("urgent", crtc->debugfs_entry);
	if (!urgent_dent) {
		DRM_ERROR("failed to create debugfs urgent directory\n");
		goto err_debugfs;
	}

	debugfs_create_u32("rd_en", 0664, urgent_dent, &decon->config.urgent.rd_en);
	debugfs_create_x32("rd_hi_thres", 0664, urgent_dent, &decon->config.urgent.rd_hi_thres);
	debugfs_create_x32("rd_lo_thres", 0664, urgent_dent, &decon->config.urgent.rd_lo_thres);
	debugfs_create_x32("rd_wait_cycle", 0664, urgent_dent, &decon->config.urgent.rd_wait_cycle);
	debugfs_create_u32("wr_en", 0664, urgent_dent, &decon->config.urgent.wr_en);
	debugfs_create_x32("wr_hi_thres", 0664, urgent_dent, &decon->config.urgent.wr_hi_thres);
	debugfs_create_x32("wr_lo_thres", 0664, urgent_dent, &decon->config.urgent.wr_lo_thres);
	debugfs_create_bool("dta_en", 0664, urgent_dent, &decon->config.urgent.dta_en);
	debugfs_create_x32("dta_hi_thres", 0664, urgent_dent, &decon->config.urgent.dta_hi_thres);
	debugfs_create_x32("dta_lo_thres", 0664, urgent_dent, &decon->config.urgent.dta_lo_thres);

	exynos_debugfs_add_dqe(dqe, crtc->debugfs_entry);

	return 0;

err_debugfs:
	debugfs_remove(debug_event);
err_event_log:
	vfree(decon->d.event_log);
	return -ENOENT;
}

#define PREFIX_LEN	40
#define ROW_LEN		32
void dpu_print_hex_dump(void __iomem *regs, const void *buf, size_t len)
{
	char prefix_buf[PREFIX_LEN];
	unsigned long p;
	int i, row;

	for (i = 0; i < len; i += ROW_LEN) {
		p = buf - regs + i;

		if (len - i < ROW_LEN)
			row = len - i;
		else
			row = ROW_LEN;

		snprintf(prefix_buf, sizeof(prefix_buf), "[%08lX] ", p);
		print_hex_dump(KERN_INFO, prefix_buf, DUMP_PREFIX_NONE,
				32, 4, buf + i, row, false);
	}
}

void decon_dump_all(struct decon_device *decon)
{
	struct drm_printer p = drm_info_printer(decon->dev);
	bool active = pm_runtime_active(decon->dev);

	pr_info("DPU power %s state\n", active ? "on" : "off");

	dpu_event_log_print(decon, &p, dpu_event_print_max);

	if (active)
		decon_dump(decon);
}

#if IS_ENABLED(CONFIG_EXYNOS_ITMON)
int dpu_itmon_notifier(struct notifier_block *nb, unsigned long act, void *data)
{
	struct decon_device *decon;
	struct itmon_notifier *itmon_data = data;

	decon = container_of(nb, struct decon_device, itmon_nb);

	pr_debug("%s: DECON%d +\n", __func__, decon->id);

	if (decon->itmon_notified)
		return NOTIFY_DONE;

	if (IS_ERR_OR_NULL(itmon_data))
		return NOTIFY_DONE;

	/* port is master and dest is target */
	if ((itmon_data->port &&
		(strncmp("DISP", itmon_data->port, sizeof("DISP") - 1) == 0)) ||
		(itmon_data->dest &&
		(strncmp("DISP", itmon_data->dest, sizeof("DISP") - 1) == 0))) {
		pr_info("%s: port: %s, dest: %s\n", __func__,
				itmon_data->port, itmon_data->dest);

		decon_dump_all(decon);

		decon->itmon_notified = true;
		return NOTIFY_OK;
	}

	pr_debug("%s -\n", __func__);

	return NOTIFY_DONE;
}

#endif

#ifdef CONFIG_DEBUG_FS
static int dphy_diag_text_show(struct seq_file *m, void *p)
{
	char *text = m->private;

	seq_printf(m, "%s\n", text);
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(dphy_diag_text);

static ssize_t dphy_diag_reg_write(struct file *file, const char *user_buf,
			      size_t count, loff_t *f_pos)
{
	int ret;
	uint32_t val;
	struct seq_file *m = file->private_data;
	struct dsim_dphy_diag *diag = m->private;

	ret = kstrtou32_from_user(user_buf, count, 0, &val);
	if (ret)
		return ret;

	ret = dsim_dphy_diag_set_reg(diag->private, diag, val);
	if (ret)
		return ret;

	return count;
}

static int dphy_diag_reg_show(struct seq_file *m, void *data)
{
	struct dsim_dphy_diag *diag = m->private;
	uint32_t regs[MAX_DIAG_REG_NUM];
	uint32_t ix;
	int ret;

	ret = dsim_dphy_diag_get_reg(diag->private, diag, regs);

	if (ret == 0) {
		for (ix = 0; ix < diag->num_reg; ++ix)
			seq_printf(m, "%d ", regs[ix]);
		seq_puts(m, "\n");
	}

	return ret;
}

static int dphy_diag_reg_open(struct inode *inode, struct file *file)
{
	return single_open(file, dphy_diag_reg_show, inode->i_private);
}

static const struct file_operations dphy_diag_reg_fops = {
	.owner = THIS_MODULE,
	.open = dphy_diag_reg_open,
	.write = dphy_diag_reg_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void dsim_diag_create_debugfs(struct dsim_device *dsim) {
	struct dentry *dent_dphy;
	struct dentry *dent_diag;
	struct dsim_dphy_diag *diag;
	char dir_name[32];
	int ix;

	scnprintf(dir_name, sizeof(dir_name), "dsim%d", dsim->id);
	dsim->debugfs_entry = debugfs_create_dir(
		dir_name, dsim->encoder.dev->primary->debugfs_root);
	if (!dsim->debugfs_entry) {
		pr_warn("%s: failed to create %s\n", __func__, dir_name);
		return;
	}

	if (dsim->config.num_dphy_diags == 0)
		return;

	dent_dphy = debugfs_create_dir("dphy", dsim->debugfs_entry);
	if (!dent_dphy) {
		pr_warn("%s: failed to create %s\n", __func__, dir_name);
		return;
	}

	for (ix = 0; ix < dsim->config.num_dphy_diags; ++ix) {
		diag = &dsim->config.dphy_diags[ix];
		dent_diag = debugfs_create_dir(diag->name, dent_dphy);
		if (!dent_diag) {
			pr_warn("%s: failed to create %s\n", __func__,
				diag->name);
			continue;
		}
		debugfs_create_file("desc", 0400, dent_diag, (void *)diag->desc,
				    &dphy_diag_text_fops);
		debugfs_create_file("help", 0400, dent_diag, (void *)diag->help,
				    &dphy_diag_text_fops);
		diag->private = dsim;
		debugfs_create_file("value", diag->read_only ? 0400 : 0600,
				    dent_diag, diag, &dphy_diag_reg_fops);
	}
}

void dsim_diag_remove_debugfs(struct dsim_device *dsim) {
	debugfs_remove_recursive(dsim->debugfs_entry);
	dsim->debugfs_entry = NULL;
}
#endif
