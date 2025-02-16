/*
 * Samsung Exynos5 SoC series FIMC-IS driver
 *
 * exynos5 fimc-is core functions
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <video/videonode.h>
#include <media/exynos_mc.h>
#include <asm/cacheflush.h>
#include <asm/pgtable.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/videodev2.h>
#include <linux/videodev2_exynos_camera.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <linux/pm_qos.h>
#include <linux/bug.h>
#include <linux/v4l2-mediabus.h>
#include <linux/gpio.h>
#include <linux/memblock.h>

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_gpio.h>
#endif

#include "fimc-is-core.h"
#include "fimc-is-cmd.h"
#include "fimc-is-regs.h"
#include "fimc-is-debug.h"
#include "fimc-is-hw.h"
#include "fimc-is-err.h"
#include "fimc-is-framemgr.h"
#include "fimc-is-dt.h"
#include "fimc-is-resourcemgr.h"
#include "fimc-is-clk-gate.h"
#include "fimc-is-dvfs.h"
#include "include/fimc-is-module.h"
#include "fimc-is-device-sensor.h"
#include "sensor/module_framework/fimc-is-device-sensor-peri.h"

#ifdef ENABLE_FAULT_HANDLER
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0))
#include <linux/exynos_iovmm.h>
#else
#include <plat/sysmmu.h>
#endif
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0))
#define PM_QOS_CAM_THROUGHPUT	PM_QOS_RESERVED
#endif

struct device *fimc_is_dev = NULL;

extern struct pm_qos_request exynos_isp_qos_int;
extern struct pm_qos_request exynos_isp_qos_mem;
extern struct pm_qos_request exynos_isp_qos_cam;

/* sysfs global variable for debug */
struct fimc_is_sysfs_debug sysfs_debug;

#ifndef ENABLE_IS_CORE
/* sysfs global variable for set position to actuator */
struct fimc_is_sysfs_actuator sysfs_actuator;
#ifdef FIXED_SENSOR_DEBUG
struct fimc_is_sysfs_sensor sysfs_sensor;
#endif
#endif

static struct vm_struct fimc_is_lib_vm;
static int __init fimc_is_lib_mem_alloc(char *str)
{
	ulong addr = 0;

	if (kstrtoul(str, 0, (ulong *)&addr) || !addr) {
		probe_warn("invalid fimc-is library memory address, use default");
		addr = LIB_START;
	}

	if (addr != LIB_START)
		probe_warn("use different address [reserve-fimc=0x%lx default:0x%lx]",
				addr, LIB_START);

	fimc_is_lib_vm.phys_addr = memblock_alloc(LIB_SIZE, SZ_4K);
	fimc_is_lib_vm.addr = (void *)addr;
	fimc_is_lib_vm.size = LIB_SIZE + PAGE_SIZE;

	vm_area_add_early(&fimc_is_lib_vm);

	probe_info("fimc-is library memory: 0x%lx\n", addr);

	return 0;
}
__setup("reserve-fimc=", fimc_is_lib_mem_alloc);

static int fimc_is_lib_mem_map(void)
{
	int page_size, i;
	struct page *page;
	struct page **pages;

	if (!fimc_is_lib_vm.phys_addr) {
		probe_err("There is no reserve-fimc= at bootargs.");
		return -ENOMEM;
	}

	page_size = fimc_is_lib_vm.size / PAGE_SIZE;
	pages = kzalloc(sizeof(struct page*) * page_size, GFP_KERNEL);
	page = phys_to_page(fimc_is_lib_vm.phys_addr);

	for (i = 0; i < page_size; i++)
		pages[i] = page++;

	if (map_vm_area(&fimc_is_lib_vm, PAGE_KERNEL, pages)) {
		probe_err("failed to mapping between virt and phys for binary");
		vunmap(fimc_is_lib_vm.addr);
		kfree(pages);
		return -ENOMEM;
	}

	kfree(pages);

	return 0;
}

#ifdef CONFIG_CPU_THERMAL_IPA
static int fimc_is_mif_throttling_notifier(struct notifier_block *nb,
		unsigned long val, void *v)
{
	struct fimc_is_core *core = NULL;
	struct fimc_is_device_sensor *device = NULL;
	int i;

	core = (struct fimc_is_core *)dev_get_drvdata(fimc_is_dev);
	if (!core) {
		err("core is null");
		goto exit;
	}

	for (i = 0; i < FIMC_IS_SENSOR_COUNT; i++) {
		if (test_bit(FIMC_IS_SENSOR_OPEN, &core->sensor[i].state)) {
			device = &core->sensor[i];
			break;
		}
	}

	if (device && !test_bit(FIMC_IS_SENSOR_FRONT_DTP_STOP, &device->state))
		/* Set DTP */
		set_bit(FIMC_IS_MIF_THROTTLING_STOP, &device->force_stop);
	else
		err("any sensor is not opened");

exit:
	err("MIF: cause of mif_throttling, mif_qos is [%lu]!!!\n", val);

	return NOTIFY_OK;
}

static struct notifier_block exynos_fimc_is_mif_throttling_nb = {
	.notifier_call = fimc_is_mif_throttling_notifier,
};
#endif

static int fimc_is_suspend(struct device *dev)
{
	pr_debug("FIMC_IS Suspend\n");
	return 0;
}

static int fimc_is_resume(struct device *dev)
{
	pr_debug("FIMC_IS Resume\n");
	return 0;
}

#ifdef ENABLE_FAULT_HANDLER
static void __fimc_is_fault_handler(struct device *dev)
{
	u32 i, j, k;
	struct fimc_is_core *core;
	struct fimc_is_device_sensor *sensor;
	struct fimc_is_device_ischain *ischain;
	struct fimc_is_subdev *subdev;
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_resourcemgr *resourcemgr;
	struct camera2_shot	*shot;

	core = dev_get_drvdata(dev);
	if (core) {
		resourcemgr = &core->resourcemgr;

		fimc_is_hw_fault(&core->interface);
		/* dump FW page table 1nd(~16KB), 2nd(16KB~32KB) */
		fimc_is_hw_memdump(&core->interface,
			resourcemgr->minfo.kvaddr + TTB_OFFSET, /* TTB_BASE ~ 32KB */
			resourcemgr->minfo.kvaddr + TTB_OFFSET + TTB_SIZE);
		fimc_is_hw_logdump(&core->interface);

		/* SENSOR */
		for (i = 0; i < FIMC_IS_SENSOR_COUNT; i++) {
			sensor = &core->sensor[i];
			framemgr = GET_FRAMEMGR(sensor->vctx);
			if (test_bit(FIMC_IS_SENSOR_OPEN, &sensor->state) && framemgr) {
				struct fimc_is_device_flite *flite;
				struct fimc_is_device_csi *csi;

				for (j = 0; j < framemgr->num_frames; ++j) {
					for (k = 0; k < framemgr->frames[j].planes; k++) {
						pr_err("[SS%d] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
							framemgr->frames[j].dvaddr_buffer[k],
							framemgr->frames[j].mem_state);
					}
				}

				/* vc0 */
				framemgr = GET_SUBDEV_FRAMEMGR(&sensor->ssvc0);
				if (test_bit(FIMC_IS_SUBDEV_START, &sensor->ssvc0.state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[SS%dVC0] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
									framemgr->frames[j].dvaddr_buffer[k],
									framemgr->frames[j].mem_state);
						}
					}
				}

				/* vc1 */
				framemgr = GET_SUBDEV_FRAMEMGR(&sensor->ssvc1);
				if (test_bit(FIMC_IS_SUBDEV_START, &sensor->ssvc1.state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[SS%dVC1] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
									framemgr->frames[j].dvaddr_buffer[k],
									framemgr->frames[j].mem_state);
						}
					}
				}

				/* vc2 */
				framemgr = GET_SUBDEV_FRAMEMGR(&sensor->ssvc2);
				if (test_bit(FIMC_IS_SUBDEV_START, &sensor->ssvc2.state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[SS%dVC2] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
									framemgr->frames[j].dvaddr_buffer[k],
									framemgr->frames[j].mem_state);
						}
					}
				}

				/* vc3 */
				framemgr = GET_SUBDEV_FRAMEMGR(&sensor->ssvc3);
				if (test_bit(FIMC_IS_SUBDEV_START, &sensor->ssvc3.state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[SS%dVC3] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
									framemgr->frames[j].dvaddr_buffer[k],
									framemgr->frames[j].mem_state);
						}
					}
				}

				/* csis, bns sfr dump */
				flite = (struct fimc_is_device_flite *)v4l2_get_subdevdata(sensor->subdev_flite);
				if (flite)
					flite_hw_dump(flite->base_reg);

				csi = (struct fimc_is_device_csi *)v4l2_get_subdevdata(sensor->subdev_csi);
				if (csi)
					csi_hw_dump(csi->base_reg);
			}
		}

		/* ISCHAIN */
		for (i = 0; i < FIMC_IS_STREAM_COUNT; i++) {
			ischain = &core->ischain[i];
			if (test_bit(FIMC_IS_ISCHAIN_OPEN, &ischain->state)) {
				/* 3AA */
				subdev = &ischain->group_3aa.leader;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][3XS] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][3XS] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.sourceAddress[k]);
						}
					}
				}
				/* 3AAC */
				subdev = &ischain->txc;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][3XC] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][3XC] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.txcTargetAddress[k]);
						}
					}
				}
				/* 3AAP */
				subdev = &ischain->txp;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][3XP] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][3XP] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.txpTargetAddress[k]);
						}
					}
				}
				/* ISP */
				subdev = &ischain->group_isp.leader;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][IXS] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);
						}
					}
				}
				/* ISPC */
				subdev = &ischain->ixc;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][IXC] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][IXC] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.ixcTargetAddress[k]);
						}
					}
				}
				/* ISPP */
				subdev = &ischain->ixp;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][IXP] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][IXP] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.ixpTargetAddress[k]);
						}
					}
				}
				/* DIS */
				subdev = &ischain->group_dis.leader;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][DIS] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);
						}
					}
				}
				/* SCC */
				subdev = &ischain->scc;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][SCC] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][SCC] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.sccTargetAddress[k]);
						}
					}
				}
				/* SCP */
				subdev = &ischain->scp;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][SCP] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][SCP] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.scpTargetAddress[k]);
						}
					}
				}
				/* MCS */
				subdev = &ischain->group_mcs.leader;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][MCS] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][MCS] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.scpTargetAddress[k]);
						}
					}
				}
				/* M0P */
				subdev = &ischain->m0p;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][M0P] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][M0P] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.sc0TargetAddress[k]);
						}
					}
				}
				/* M1P */
				subdev = &ischain->m1p;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][M1P] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][M1P] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.sc1TargetAddress[k]);
						}
					}
				}
				/* M2P */
				subdev = &ischain->m2p;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][M2P] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][M2P] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.sc2TargetAddress[k]);
						}
					}
				}
				/* M3P */
				subdev = &ischain->m3p;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][M3P] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][M3P] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.sc3TargetAddress[k]);
						}
					}
				}
				/* M4P */
				subdev = &ischain->m4p;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][M4P] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);

							shot = framemgr->frames[j].shot;
							if (shot)
								pr_err("[%d][M4P] BUF[%d][%d] target = 0x%08X\n", i, j, k,
									shot->uctl.scalerUd.sc4TargetAddress[k]);
						}
					}
				}
				/* VRA */
				subdev = &ischain->group_vra.leader;
				framemgr = GET_SUBDEV_FRAMEMGR(subdev);
				if (test_bit(FIMC_IS_SUBDEV_START, &subdev->state) && framemgr) {
					for (j = 0; j < framemgr->num_frames; ++j) {
						for (k = 0; k < framemgr->frames[j].planes; k++) {
							pr_err("[%d][VRA] BUF[%d][%d] = 0x%08X(0x%lX)\n", i, j, k,
								framemgr->frames[j].dvaddr_buffer[k],
								framemgr->frames[j].mem_state);
						}
					}
				}
			}
		}
	} else {
		pr_err("failed to get core\n");
	}
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0))
#define SECT_ORDER 20
#define LPAGE_ORDER 16
#define SPAGE_ORDER 12

#define lv1ent_page(sent) ((*(sent) & 3) == 1)

#define lv1ent_offset(iova) ((iova) >> SECT_ORDER)
#define lv2ent_offset(iova) (((iova) & 0xFF000) >> SPAGE_ORDER)
#define lv2table_base(sent) (*(sent) & 0xFFFFFC00)

static unsigned long *section_entry(unsigned long *pgtable, unsigned long iova)
{
	return pgtable + lv1ent_offset(iova);
}

static unsigned long *page_entry(unsigned long *sent, unsigned long iova)
{
	return (unsigned long *)__va(lv2table_base(sent)) + lv2ent_offset(iova);
}

static char *sysmmu_fault_name[SYSMMU_FAULTS_NUM] = {
	"PAGE FAULT",
	"AR MULTI-HIT FAULT",
	"AW MULTI-HIT FAULT",
	"BUS ERROR",
	"AR SECURITY PROTECTION FAULT",
	"AR ACCESS PROTECTION FAULT",
	"AW SECURITY PROTECTION FAULT",
	"AW ACCESS PROTECTION FAULT",
	"UNKNOWN FAULT"
};

static int fimc_is_fault_handler(struct device *dev, const char *mmuname,
					enum exynos_sysmmu_inttype itype,
					unsigned long pgtable_base,
					unsigned long fault_addr)
{
	unsigned long *ent;

	if ((itype >= SYSMMU_FAULTS_NUM) || (itype < SYSMMU_PAGEFAULT))
		itype = SYSMMU_FAULT_UNKNOWN;

	pr_err("%s occured at 0x%lx by '%s'(Page table base: 0x%lx)\n",
		sysmmu_fault_name[itype], fault_addr, mmuname, pgtable_base);

	ent = section_entry(__va(pgtable_base), fault_addr);
	pr_err("\tLv1 entry: 0x%lx\n", *ent);

	if (lv1ent_page(ent)) {
		ent = page_entry(ent, fault_addr);
		pr_err("\t Lv2 entry: 0x%lx\n", *ent);
	}

	__fimc_is_fault_handler(dev);

	pr_err("Generating Kernel OOPS... because it is unrecoverable.\n");

	BUG();

	return 0;
}
#else
static int __attribute__((unused)) fimc_is_fault_handler(struct iommu_domain *domain,
	struct device *dev,
	unsigned long fault_addr,
	int fault_flag,
	void *token)
{
	pr_err("<FIMC-IS FAULT HANDLER>\n");
	pr_err("Device virtual(0x%X) is invalid access\n", (u32)fault_addr);

	__fimc_is_fault_handler(dev);

	return -EINVAL;
}
#endif
#endif /* ENABLE_FAULT_HANDLER */

static ssize_t show_clk_gate_mode(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", sysfs_debug.clk_gate_mode);
}

static ssize_t store_clk_gate_mode(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
#ifdef HAS_FW_CLOCK_GATE
	switch (buf[0]) {
	case '0':
		sysfs_debug.clk_gate_mode = CLOCK_GATE_MODE_HOST;
		break;
	case '1':
		sysfs_debug.clk_gate_mode = CLOCK_GATE_MODE_FW;
		break;
	default:
		pr_debug("%s: %c\n", __func__, buf[0]);
		break;
	}
#endif
	return count;
}

static ssize_t show_en_clk_gate(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", sysfs_debug.en_clk_gate);
}

static ssize_t store_en_clk_gate(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
#ifdef ENABLE_CLOCK_GATE
	switch (buf[0]) {
	case '0':
		sysfs_debug.en_clk_gate = false;
		sysfs_debug.clk_gate_mode = CLOCK_GATE_MODE_HOST;
		break;
	case '1':
		sysfs_debug.en_clk_gate = true;
		sysfs_debug.clk_gate_mode = CLOCK_GATE_MODE_HOST;
		break;
	default:
		pr_debug("%s: %c\n", __func__, buf[0]);
		break;
	}
#endif

#ifdef ENABLE_DIRECT_CLOCK_GATE
	switch (buf[0]) {
	case '0':
		sysfs_debug.en_clk_gate = false;
		break;
	case '1':
		sysfs_debug.en_clk_gate = true;
		break;
	default:
		pr_debug("%s: %c\n", __func__, buf[0]);
		break;
	}
#endif
	return count;
}

static ssize_t show_en_dvfs(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", sysfs_debug.en_dvfs);
}

static ssize_t store_en_dvfs(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
#ifdef ENABLE_DVFS
	struct fimc_is_core *core =
		(struct fimc_is_core *)platform_get_drvdata(to_platform_device(dev));
	struct fimc_is_resourcemgr *resourcemgr;
	int i;

	BUG_ON(!core);

	resourcemgr = &core->resourcemgr;

	switch (buf[0]) {
	case '0':
		sysfs_debug.en_dvfs = false;
		/* update dvfs lever to max */
		mutex_lock(&resourcemgr->dvfs_ctrl.lock);
		for (i = 0; i < FIMC_IS_STREAM_COUNT; i++) {
			if (test_bit(FIMC_IS_ISCHAIN_OPEN, &((core->ischain[i]).state)))
				fimc_is_set_dvfs(core, &(core->ischain[i]), FIMC_IS_SN_MAX);
		}
		fimc_is_dvfs_init(resourcemgr);
		resourcemgr->dvfs_ctrl.static_ctrl->cur_scenario_id = FIMC_IS_SN_MAX;
		mutex_unlock(&resourcemgr->dvfs_ctrl.lock);
		break;
	case '1':
		/* It can not re-define static scenario */
		sysfs_debug.en_dvfs = true;
		break;
	default:
		pr_debug("%s: %c\n", __func__, buf[0]);
		break;
	}
#endif
	return count;
}

static ssize_t show_hal_debug_mode(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%lx\n", sysfs_debug.hal_debug_mode);
}

static ssize_t store_hal_debug_mode(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long long debug_mode = 0;

	ret = kstrtoull(buf, 16 /* hexa */, &debug_mode);
	if (ret < 0) {
		pr_err("%s, %s, failed for debug_mode:%llu, ret:%d", __func__, buf, debug_mode, ret);
		return 0;
	}

	sysfs_debug.hal_debug_mode = (unsigned long)debug_mode;

	return count;
}

static ssize_t show_hal_debug_delay(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u ms\n", sysfs_debug.hal_debug_delay);
}

static ssize_t store_hal_debug_delay(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;

	ret = kstrtouint(buf, 10, &sysfs_debug.hal_debug_delay);
	if (ret < 0) {
		pr_err("%s, %s, failed for debug_delay:%u, ret:%d", __func__, buf, sysfs_debug.hal_debug_delay, ret);
		return 0;
	}

	return count;
}

#ifdef ENABLE_DBG_STATE
static ssize_t show_debug_state(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct fimc_is_core *core =
		(struct fimc_is_core *)platform_get_drvdata(to_platform_device(dev));
	struct fimc_is_resourcemgr *resourcemgr;

	BUG_ON(!core);

	resourcemgr = &core->resourcemgr;

	return snprintf(buf, PAGE_SIZE, "%d\n", resourcemgr->hal_version);
}

static ssize_t store_debug_state(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct fimc_is_core *core =
		(struct fimc_is_core *)platform_get_drvdata(to_platform_device(dev));
	struct fimc_is_resourcemgr *resourcemgr;

	BUG_ON(!core);

	resourcemgr = &core->resourcemgr;

	switch (buf[0]) {
	case '0':
		break;
	case '1':
		break;
	case '7':
		break;
	default:
		pr_debug("%s: %c\n", __func__, buf[0]);
		break;
	}

	return count;
}
#endif

#ifndef ENABLE_IS_CORE
static ssize_t store_actuator_init_step(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int ret_count;
	unsigned int init_step;

	ret_count = sscanf(buf, "%u", &init_step);
	if (ret_count != 1)
		return -EINVAL;

	switch (init_step) {
		/* case number is step of set position */
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
			sysfs_actuator.init_step = init_step;
			break;
		/* default actuator setting (2step default) */
		default:
			sysfs_actuator.init_step = 0;
			break;
	}

	return count;
}

static ssize_t store_actuator_init_positions(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int i;
	int ret_count;
	int init_positions[INIT_MAX_SETTING];

	ret_count = sscanf(buf, "%d %d %d %d %d", &init_positions[0], &init_positions[1],
						&init_positions[2], &init_positions[3], &init_positions[4]);
	if (ret_count > INIT_MAX_SETTING)
		return -EINVAL;

	for (i = 0; i < ret_count; i++) {
		if (init_positions[i] >= 0 && init_positions[i] < 1024)
			sysfs_actuator.init_positions[i] = init_positions[i];
		else
			sysfs_actuator.init_positions[i] = 0;
	}

	return count;
}

static ssize_t store_actuator_init_delays(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int ret_count;
	int i;
	int init_delays[INIT_MAX_SETTING];

	ret_count = sscanf(buf, "%d %d %d %d %d", &init_delays[0], &init_delays[1],
							&init_delays[2], &init_delays[3], &init_delays[4]);
	if (ret_count > INIT_MAX_SETTING)
		return -EINVAL;

	for (i = 0; i < ret_count; i++) {
		if (init_delays[i] >= 0)
			sysfs_actuator.init_delays[i] = init_delays[i];
		else
			sysfs_actuator.init_delays[i] = 0;
	}

	return count;
}

static ssize_t show_actuator_init_step(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", sysfs_actuator.init_step);
}

static ssize_t show_actuator_init_positions(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d %d %d %d %d\n", sysfs_actuator.init_positions[0],
						sysfs_actuator.init_positions[1], sysfs_actuator.init_positions[2],
						sysfs_actuator.init_positions[3], sysfs_actuator.init_positions[4]);
}

static ssize_t show_actuator_init_delays(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d %d %d %d %d\n", sysfs_actuator.init_delays[0],
							sysfs_actuator.init_delays[1], sysfs_actuator.init_delays[2],
							sysfs_actuator.init_delays[3], sysfs_actuator.init_delays[4]);
}
#ifdef FIXED_SENSOR_DEBUG
static ssize_t show_fixed_sensor_val(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "fps(%d) ex(%d %d) a_gain(%d %d) d_gain(%d %d)\n",
			sysfs_sensor.frame_duration,
			sysfs_sensor.long_exposure_time,
			sysfs_sensor.short_exposure_time,
			sysfs_sensor.long_analog_gain,
			sysfs_sensor.short_analog_gain,
			sysfs_sensor.long_digital_gain,
			sysfs_sensor.short_digital_gain);
}

static ssize_t store_fixed_sensor_val(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret_count;
	int input_val[7];

	ret_count = sscanf(buf, "%d %d %d %d %d %d %d", &input_val[0], &input_val[1],
							&input_val[2], &input_val[3],
							&input_val[4], &input_val[5], &input_val[6]);
	if (ret_count != 7) {
		probe_err("%s: count should be 7 but %d \n", __func__, ret_count);
		return -EINVAL;
	}

	sysfs_sensor.frame_duration = input_val[0];
	sysfs_sensor.long_exposure_time = input_val[1];
	sysfs_sensor.short_exposure_time = input_val[2];
	sysfs_sensor.long_analog_gain = input_val[3];
	sysfs_sensor.short_analog_gain = input_val[4];
	sysfs_sensor.long_digital_gain = input_val[5];
	sysfs_sensor.short_digital_gain = input_val[6];

	return count;



}

static ssize_t show_en_fixed_sensor(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	if (sysfs_sensor.is_en)
		return snprintf(buf, PAGE_SIZE, "%s\n", "enabled");
	else
		return snprintf(buf, PAGE_SIZE, "%s\n", "disabled");
}

static ssize_t store_en_fixed_sensor(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	if (buf[0] == '1')
		sysfs_sensor.is_en = true;
	else
		sysfs_sensor.is_en = false;

	return count;
}
#endif
#endif

static DEVICE_ATTR(en_clk_gate, 0644, show_en_clk_gate, store_en_clk_gate);
static DEVICE_ATTR(clk_gate_mode, 0644, show_clk_gate_mode, store_clk_gate_mode);
static DEVICE_ATTR(en_dvfs, 0644, show_en_dvfs, store_en_dvfs);
static DEVICE_ATTR(hal_debug_mode, 0644, show_hal_debug_mode, store_hal_debug_mode);
static DEVICE_ATTR(hal_debug_delay, 0644, show_hal_debug_delay, store_hal_debug_delay);

#ifdef ENABLE_DBG_STATE
static DEVICE_ATTR(en_debug_state, 0644, show_debug_state, store_debug_state);
#endif

#ifndef ENABLE_IS_CORE
static DEVICE_ATTR(init_step, 0644, show_actuator_init_step, store_actuator_init_step);
static DEVICE_ATTR(init_positions, 0644, show_actuator_init_positions, store_actuator_init_positions);
static DEVICE_ATTR(init_delays, 0644, show_actuator_init_delays, store_actuator_init_delays);

#ifdef FIXED_SENSOR_DEBUG
static DEVICE_ATTR(fixed_sensor_val, 0644, show_fixed_sensor_val, store_fixed_sensor_val);
static DEVICE_ATTR(en_fixed_sensor, 0644, show_en_fixed_sensor, store_en_fixed_sensor);
#endif
#endif

static struct attribute *fimc_is_debug_entries[] = {
	&dev_attr_en_clk_gate.attr,
	&dev_attr_clk_gate_mode.attr,
	&dev_attr_en_dvfs.attr,
	&dev_attr_hal_debug_mode.attr,
	&dev_attr_hal_debug_delay.attr,
#ifdef ENABLE_DBG_STATE
	&dev_attr_en_debug_state.attr,
#endif
#ifndef ENABLE_IS_CORE
	&dev_attr_init_step.attr,
	&dev_attr_init_positions.attr,
	&dev_attr_init_delays.attr,
#ifdef FIXED_SENSOR_DEBUG
	&dev_attr_fixed_sensor_val.attr,
	&dev_attr_en_fixed_sensor.attr,
#endif
#endif
	NULL,
};
static struct attribute_group fimc_is_debug_attr_group = {
	.name	= "debug",
	.attrs	= fimc_is_debug_entries,
};

static int fimc_is_probe(struct platform_device *pdev)
{
	struct exynos_platform_fimc_is *pdata;
#if defined (ENABLE_IS_CORE) || defined (USE_MCUCTL)
	struct resource *mem_res;
	struct resource *regs_res;
#endif
	struct fimc_is_core *core;
	int ret = -ENODEV;
#ifndef ENABLE_IS_CORE
	int i;
#endif
	u32 stream;
#if defined(USE_I2C_LOCK)
	u32 channel;
#endif
	struct pinctrl_state *s;

	probe_info("%s:start(%ld, %ld)\n", __func__,
		sizeof(struct fimc_is_core), sizeof(struct fimc_is_video_ctx));

	core = kzalloc(sizeof(struct fimc_is_core), GFP_KERNEL);
	if (!core) {
		probe_err("core is NULL");
		return -ENOMEM;
	}

	fimc_is_dev = &pdev->dev;
	dev_set_drvdata(fimc_is_dev, core);

	pdata = dev_get_platdata(&pdev->dev);
	if (!pdata) {
#ifdef CONFIG_OF
		ret = fimc_is_parse_dt(pdev);
		if (ret) {
			err("fimc_is_parse_dt is fail(%d)", ret);
			return ret;
		}

		pdata = dev_get_platdata(&pdev->dev);
#else
		BUG();
#endif
	}

#ifdef USE_ION_ALLOC
	core->fimc_ion_client = exynos_ion_client_create("fimc-is");
#endif
	core->pdev = pdev;
	core->pdata = pdata;
	core->current_position = SENSOR_POSITION_REAR;
	device_init_wakeup(&pdev->dev, true);

	/* for mideaserver force down */
	atomic_set(&core->rsccount, 0);

#if defined (ENABLE_IS_CORE) || defined (USE_MCUCTL)
	mem_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem_res) {
		probe_err("Failed to get io memory region(%p)", mem_res);
		goto p_err1;
	}

	regs_res = request_mem_region(mem_res->start, resource_size(mem_res), pdev->name);
	if (!regs_res) {
		probe_err("Failed to request io memory region(%p)", regs_res);
		goto p_err1;
	}

	core->regs_res = regs_res;
	core->regs =  ioremap_nocache(mem_res->start, resource_size(mem_res));
	if (!core->regs) {
		probe_err("Failed to remap io region(%p)", core->regs);
		goto p_err2;
	}
#else
	core->regs_res = NULL;
	core->regs = NULL;
#endif

#ifdef ENABLE_IS_CORE
	core->irq = platform_get_irq(pdev, 0);
	if (core->irq < 0) {
		probe_err("Failed to get irq(%d)", core->irq);
		goto p_err3;
	}
#endif

	if (pdata) {
		ret = pdata->clk_get(&pdev->dev);
		if (ret) {
			probe_err("clk_get is fail(%d)", ret);
			goto p_err3;
		}
	}

	ret = fimc_is_lib_mem_map();
	if (ret) {
		probe_err("fimc_is_lib_mem_map is fail(%d)", ret);
		goto p_err3;
	}

	ret = fimc_is_mem_init(&core->resourcemgr.mem, core->pdev);
	if (ret) {
		probe_err("fimc_is_mem_probe is fail(%d)", ret);
		goto p_err3;
	}

	ret = fimc_is_resourcemgr_probe(&core->resourcemgr, core);
	if (ret) {
		probe_err("fimc_is_resourcemgr_probe is fail(%d)", ret);
		goto p_err3;
	}

	ret = fimc_is_interface_probe(&core->interface,
		&core->resourcemgr.minfo,
		(ulong)core->regs,
		core->irq,
		core);
	if (ret) {
		probe_err("fimc_is_interface_probe is fail(%d)", ret);
		goto p_err3;
	}

	ret = fimc_is_debug_probe();
	if (ret) {
		probe_err("fimc_is_deubg_probe is fail(%d)", ret);
		goto p_err3;
	}

	ret = fimc_is_vender_probe(&core->vender);
	if (ret) {
		probe_err("fimc_is_vender_probe is fail(%d)", ret);
		goto p_err3;
	}

	/* group initialization */
	ret = fimc_is_groupmgr_probe(&core->groupmgr);
	if (ret) {
		probe_err("fimc_is_groupmgr_probe is fail(%d)", ret);
		goto p_err3;
	}

	ret = fimc_is_devicemgr_probe(&core->devicemgr);
	if (ret) {
		probe_err("fimc_is_devicemgr_probe is fail(%d)", ret);
		goto p_err3;
	}

	for (stream = 0; stream < FIMC_IS_STREAM_COUNT; ++stream) {
		ret = fimc_is_ischain_probe(&core->ischain[stream],
			&core->interface,
			&core->resourcemgr,
			&core->groupmgr,
			&core->devicemgr,
			&core->resourcemgr.mem,
			core->pdev,
			stream);
		if (ret) {
			probe_err("fimc_is_ischain_probe(%d) is fail(%d)", stream, ret);
			goto p_err3;
		}

#ifndef ENABLE_IS_CORE
		core->ischain[stream].hardware = &core->hardware;
#endif
	}

	ret = v4l2_device_register(&pdev->dev, &core->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register fimc-is v4l2 device\n");
		goto p_err3;
	}

#ifdef SOC_30S
	/* video entity - 3a0 */
	fimc_is_30s_video_probe(core);
#endif

#ifdef SOC_30C
	/* video entity - 3a0 capture */
	fimc_is_30c_video_probe(core);
#endif

#ifdef SOC_30P
	/* video entity - 3a0 preview */
	fimc_is_30p_video_probe(core);
#endif

#ifdef SOC_31S
	/* video entity - 3a1 */
	fimc_is_31s_video_probe(core);
#endif

#ifdef SOC_31C
	/* video entity - 3a1 capture */
	fimc_is_31c_video_probe(core);
#endif

#ifdef SOC_31P
	/* video entity - 3a1 preview */
	fimc_is_31p_video_probe(core);
#endif

#ifdef SOC_I0S
	/* video entity - isp0 */
	fimc_is_i0s_video_probe(core);
#endif

#ifdef SOC_I0C
	/* video entity - isp0 capture */
	fimc_is_i0c_video_probe(core);
#endif

#ifdef SOC_I0P
	/* video entity - isp0 preview */
	fimc_is_i0p_video_probe(core);
#endif

#ifdef SOC_I1S
	/* video entity - isp1 */
	fimc_is_i1s_video_probe(core);
#endif

#ifdef SOC_I1C
	/* video entity - isp1 capture */
	fimc_is_i1c_video_probe(core);
#endif

#ifdef SOC_I1P
	/* video entity - isp1 preview */
	fimc_is_i1p_video_probe(core);
#endif

#if defined(SOC_DIS) || defined(SOC_D0S)
	/* video entity - tpu0 */
	fimc_is_d0s_video_probe(core);
#endif

#ifdef SOC_D0C
	/* video entity - tpu0 capture */
	fimc_is_d0c_video_probe(core);
#endif

#if defined(SOC_D1S)
	/* video entity - tpu1 */
	fimc_is_d1s_video_probe(core);
#endif

#ifdef SOC_D1C
	/* video entity - tpu1 capture */
	fimc_is_d1c_video_probe(core);
#endif

#ifdef SOC_SCC
	/* video entity - scc */
	fimc_is_scc_video_probe(core);
#endif

#ifdef SOC_SCP
	/* video entity - scp */
	fimc_is_scp_video_probe(core);
#endif

#ifdef SOC_MCS
	/* video entity - scp */
	fimc_is_m0s_video_probe(core);
	fimc_is_m1s_video_probe(core);
	fimc_is_m0p_video_probe(core);
	fimc_is_m1p_video_probe(core);
	fimc_is_m2p_video_probe(core);
	fimc_is_m3p_video_probe(core);
	fimc_is_m4p_video_probe(core);
#endif

#ifdef SOC_VRA
	/* video entity - vra */
	fimc_is_vra_video_probe(core);
#endif

/* TODO: video probe is needed for DCP */

	platform_set_drvdata(pdev, core);

#if defined(CONFIG_EXYNOS_DEVICE_MIPI_CSIS_VER3)
	/* CSIS common dma probe */
	ret = fimc_is_csi_dma_probe(&core->csi_dma, core->pdev);
	if (ret) {
		dev_err(&pdev->dev, "fimc_is_csi_dma_probe fail\n");
		goto p_err1;
	}
#endif

#ifndef ENABLE_IS_CORE
	ret = fimc_is_interface_ischain_probe(&core->interface_ischain,
		&core->hardware,
		&core->resourcemgr,
		core->pdev,
		(ulong)core->regs);
	if (ret) {
		dev_err(&pdev->dev, "interface_ischain_probe fail\n");
		goto p_err1;
	}

	ret = fimc_is_hardware_probe(&core->hardware, &core->interface, &core->interface_ischain);
	if (ret) {
		dev_err(&pdev->dev, "hardware_probe fail\n");
		goto p_err1;
	}

	/* set sysfs for set position to actuator */
	sysfs_actuator.init_step = 0;
	for (i = 0; i < INIT_MAX_SETTING; i++) {
		sysfs_actuator.init_positions[i] = -1;
		sysfs_actuator.init_delays[i] = -1;
	}
#ifdef FIXED_SENSOR_DEBUG
	sysfs_sensor.is_en = false;
	sysfs_sensor.frame_duration = FIXED_FPS_VALUE;
	sysfs_sensor.long_exposure_time = FIXED_EXPOSURE_VALUE;
	sysfs_sensor.short_exposure_time = FIXED_EXPOSURE_VALUE;
	sysfs_sensor.long_analog_gain = FIXED_AGAIN_VALUE;
	sysfs_sensor.short_analog_gain = FIXED_AGAIN_VALUE;
	sysfs_sensor.long_digital_gain = FIXED_DGAIN_VALUE;
	sysfs_sensor.short_digital_gain = FIXED_DGAIN_VALUE;
#endif
#endif

#if defined(CONFIG_SOC_EXYNOS5430) || defined(CONFIG_SOC_EXYNOS5433)
#if defined(CONFIG_VIDEOBUF2_ION)
	if (core->resourcemgr.mem.alloc_ctx)
		vb2_ion_attach_iommu(core->resourcemgr.mem.alloc_ctx);
#endif
#endif

	EXYNOS_MIF_ADD_NOTIFIER(&exynos_fimc_is_mif_throttling_nb);

#if defined(CONFIG_PM)
	pm_runtime_enable(&pdev->dev);
#endif

#ifdef ENABLE_FAULT_HANDLER
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0))
	exynos_sysmmu_set_fault_handler(fimc_is_dev, fimc_is_fault_handler);
#else
	iovmm_set_fault_handler(fimc_is_dev, fimc_is_fault_handler, NULL);
#endif
#endif

#if defined(USE_I2C_LOCK)
	for (channel = 0; channel < SENSOR_CONTROL_I2C_MAX; channel++) {
		mutex_init(&core->i2c_lock[channel]);
	}
#endif

	/* set sysfs for debuging */
	sysfs_debug.en_clk_gate = 0;
	sysfs_debug.en_dvfs = 1;
	sysfs_debug.hal_debug_mode = 0;
	sysfs_debug.hal_debug_delay = DBG_HAL_DEAD_PANIC_DELAY;
#ifdef ENABLE_CLOCK_GATE
	sysfs_debug.en_clk_gate = 1;
#ifdef HAS_FW_CLOCK_GATE
	sysfs_debug.clk_gate_mode = CLOCK_GATE_MODE_FW;
#else
	sysfs_debug.clk_gate_mode = CLOCK_GATE_MODE_HOST;
#endif
#endif

#ifdef ENABLE_DIRECT_CLOCK_GATE
	sysfs_debug.en_clk_gate = 1;
#endif
	ret = sysfs_create_group(&core->pdev->dev.kobj, &fimc_is_debug_attr_group);

	s = pinctrl_lookup_state(pdata->pinctrl, "release");

	if (pinctrl_select_state(pdata->pinctrl, s) < 0) {
		probe_err("pinctrl_select_state is fail\n");
		goto p_err3;
	}

	core->shutdown = false;
	core->reboot = false;

	probe_info("%s:end\n", __func__);
	return 0;

p_err3:
	iounmap(core->regs);
#if defined (ENABLE_IS_CORE) || defined (USE_MCUCTL)
p_err2:
	release_mem_region(regs_res->start, resource_size(regs_res));
#endif
p_err1:
	kfree(core);
	return ret;
}

static int fimc_is_remove(struct platform_device *pdev)
{
	return 0;
}

void fimc_is_cleanup(struct fimc_is_core *core)
{
	struct fimc_is_device_sensor *device;
	int ret = 0;
	u32 i;

	if (!core) {
		err("%s: core(NULL)", __func__);
		return;
	}

	core->reboot = true;
	info("%s++: shutdown(%d), reboot(%d)\n", __func__, core->shutdown, core->reboot);
	for (i = 0; i < FIMC_IS_SENSOR_COUNT; i++) {
		device = &core->sensor[i];
		if (!device) {
			err("%s: device(NULL)", __func__);
			continue;
		}

		if (test_bit(FIMC_IS_SENSOR_FRONT_START, &device->state)) {
			minfo("call sensor_front_stop()\n", device);

			ret = fimc_is_sensor_front_stop(device);
			if (ret)
				mwarn("fimc_is_sensor_front_stop() is fail(%d)", device, ret);
		}
	}
	info("%s:--\n", __func__);

	return;
}

static void fimc_is_shutdown(struct platform_device *pdev)
{
	struct fimc_is_core *core = (struct fimc_is_core *)platform_get_drvdata(pdev);
	struct fimc_is_device_sensor *device;
	struct v4l2_subdev *subdev;
	struct fimc_is_module_enum *module;
	struct fimc_is_device_sensor_peri *sensor_peri;
	u32 i;

	if (!core) {
		err("%s: core(NULL)", __func__);
		return;
	}

	core->shutdown = true;
	info("%s++: shutdown(%d), reboot(%d)\n", __func__, core->shutdown, core->reboot);
#if !defined(ENABLE_IS_CORE)
	fimc_is_cleanup(core);
#endif
	for (i = 0; i < FIMC_IS_SENSOR_COUNT; i++) {
		device = &core->sensor[i];
		if (!device) {
			warn("%s: device(NULL)", __func__);
			continue;
		}

		if (test_bit(FIMC_IS_SENSOR_OPEN, &device->state)) {
			subdev = device->subdev_module;
			if (!subdev) {
				warn("%s: subdev(NULL)", __func__);
				continue;
			}

			module = (struct fimc_is_module_enum *)v4l2_get_subdevdata(subdev);
			if (!module) {
				warn("%s: module(NULL)", __func__);
				continue;
			}

			sensor_peri = (struct fimc_is_device_sensor_peri *)module->private_data;
			if (!sensor_peri) {
				warn("%s: sensor_peri(NULL)", __func__);
				continue;
			}

			fimc_is_sensor_deinit_sensor_thread(sensor_peri);
			fimc_is_sensor_deinit_mode_change_thread(sensor_peri);
		}
	}
	info("%s:--\n", __func__);

	return;
}

static const struct dev_pm_ops fimc_is_pm_ops = {
	.suspend		= fimc_is_suspend,
	.resume			= fimc_is_resume,
	.runtime_suspend	= fimc_is_ischain_runtime_suspend,
	.runtime_resume		= fimc_is_ischain_runtime_resume,
};

#ifdef CONFIG_OF
static const struct of_device_id exynos_fimc_is_match[] = {
	{
		.compatible = "samsung,exynos5-fimc-is",
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_fimc_is_match);

static struct platform_driver fimc_is_driver = {
	.probe		= fimc_is_probe,
	.remove		= fimc_is_remove,
	.shutdown	= fimc_is_shutdown,
	.driver = {
		.name	= FIMC_IS_DRV_NAME,
		.owner	= THIS_MODULE,
		.pm	= &fimc_is_pm_ops,
		.of_match_table = exynos_fimc_is_match,
	}
};

#else
static struct platform_driver fimc_is_driver = {
	.probe		= fimc_is_probe,
	.remove 	= __devexit_p(fimc_is_remove),
	.driver = {
		.name	= FIMC_IS_DRV_NAME,
		.owner	= THIS_MODULE,
		.pm	= &fimc_is_pm_ops,
	}
};
#endif

static int __init fimc_is_init(void)
{
	int ret = platform_driver_register(&fimc_is_driver);
	if (ret)
		err("platform_driver_register failed: %d\n", ret);

	return ret;
}
device_initcall(fimc_is_init);

static void __exit fimc_is_exit(void)
{
	platform_driver_unregister(&fimc_is_driver);
}
module_exit(fimc_is_exit);

MODULE_AUTHOR("Gilyeon im<kilyeon.im@samsung.com>");
MODULE_DESCRIPTION("Exynos FIMC_IS2 driver");
MODULE_LICENSE("GPL");
