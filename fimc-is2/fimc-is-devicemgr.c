/*
 * Samsung Exynos SoC series FIMC-IS driver
 *
 * exynos5 fimc-is group manager functions
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd
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
#include <linux/v4l2-mediabus.h>
#include <linux/bug.h>

#include "fimc-is-hw.h"
#include "fimc-is-core.h"
#include "fimc-is-type.h"
#include "fimc-is-err.h"
#include "fimc-is-video.h"
#include "fimc-is-framemgr.h"
#include "fimc-is-groupmgr.h"
#include "fimc-is-devicemgr.h"
#include "fimc-is-device-ischain.h"
#include "fimc-is-hw-control.h"

#ifdef CONFIG_USE_SENSOR_GROUP
static void tasklet_sensor_tag(unsigned long data)
{
	int ret = 0;
	u32 stream;
	unsigned long flags;
	struct fimc_is_framemgr *framemgr;
	struct v4l2_subdev *subdev;
	struct camera2_node ldr_node = {0, };
	struct fimc_is_device_sensor *sensor;
	struct devicemgr_sensor_tag_data *tag_data;
	struct fimc_is_group *group;
	struct fimc_is_frame *frame;
	struct fimc_is_group_task *gtask;

	tag_data = (struct devicemgr_sensor_tag_data *)data;
	stream = tag_data->stream;
	sensor = tag_data->devicemgr->sensor[stream];
	group = tag_data->group;
	subdev = sensor->subdev_csi;
	gtask = &sensor->groupmgr->gtask[group->id];

#ifdef DBG_STREAMING
	mginfo("[F%d] DEVICE TASKLET start\n", group->device, group, tag_data->fcount);
#endif
	if (unlikely(test_bit(FIMC_IS_GROUP_FORCE_STOP, &group->state))) {
		mgwarn(" cancel by fstop", group, group);
		goto p_err;
	}

	if (unlikely(test_bit(FIMC_IS_GTASK_REQUEST_STOP, &gtask->state))) {
		mgerr(" cancel by gstop", group, group);
		goto p_err;
	}

	framemgr = GET_HEAD_GROUP_FRAMEMGR(group);
	if (!framemgr) {
		merr("framemgr is NULL", group);
		return;
	}

	framemgr_e_barrier(framemgr, 0);
	frame = find_frame(framemgr, FS_PROCESS, frame_fcount, (void *)(ulong)tag_data->fcount);
	framemgr_x_barrier(framemgr, 0);

	if (!frame) {
		frame_manager_print_queues(framemgr);
		merr("[F%d] There's no frame in processing." \
			"Can't sync sensor and ischain buffer anymore..",
			group, tag_data->fcount);
		return;
	}

	ldr_node = frame->shot_ext->node_group.leader;

	ret = fimc_is_sensor_group_tag(sensor, frame, &ldr_node);
	if (ret) {
		merr("fimc_is_sensor_group_tag is fail(%d)", group, ret);
		goto p_err;
	}

	if (sensor->fcount >= frame->fcount) {
		merr("late sensor tag. DMA will be canceled. (%d != %d)",
				group, sensor->fcount, frame->fcount);
		fimc_is_sensor_dma_cancel(sensor);

		mginfo("[F%d] Start CANCEL Other subdev frame\n", group->device, group, frame->fcount);
		flags = fimc_is_group_lock(group, group->device_type, false);
		fimc_is_group_subdev_cancel(group, frame, group->device_type, FS_PROCESS, true);
		fimc_is_group_unlock(group, flags, group->device_type, false);
		mginfo("[F%d] End CANCEL Other subdev frame\n", group->device, group, frame->fcount);
	}

#ifdef DBG_STREAMING
	mginfo("[F%d] DEVICE TASKLET end\n", group->device, group, tag_data->fcount);
#endif

p_err:
	return;
}

int fimc_is_devicemgr_probe(struct fimc_is_devicemgr *devicemgr)
{
	int ret = 0;

	return ret;
}

int fimc_is_devicemgr_open(struct fimc_is_devicemgr *devicemgr,
		void *device, enum fimc_is_device_type type)
{
	int ret = 0;
	u32 stream;
	u32 group_id;
	struct fimc_is_core *core;
	struct fimc_is_group *group;
	struct fimc_is_device_sensor *sensor;
	struct fimc_is_device_ischain *ischain;

	BUG_ON(!devicemgr);
	BUG_ON(!device);

	switch (type) {
	case FIMC_IS_DEVICE_SENSOR:
		sensor = (struct fimc_is_device_sensor *)device;
		group = &sensor->group_sensor;
		core = sensor->private_data;

		BUG_ON(!core);
		BUG_ON(!group);
		/* get the stream id */
		for (stream = 0; stream < FIMC_IS_STREAM_COUNT; ++stream) {
			ischain = &core->ischain[stream];
			if (!test_bit(FIMC_IS_ISCHAIN_OPEN, &ischain->state))
				break;
		}

		BUG_ON(stream >= FIMC_IS_STREAM_COUNT);

		/* init group's device information */
		devicemgr->sensor[stream] = sensor;
		group->instance = stream;
		group->device = ischain;
		group_id = GROUP_ID_SS0 + GET_SSX_ID(GET_VIDEO(sensor->vctx));

		ret = fimc_is_group_open(sensor->groupmgr,
				group,
				group_id,
				sensor->vctx);
		if (ret) {
			merr("fimc_is_group_open is fail(%d)", ischain, ret);
			ret = -EINVAL;
			goto p_err;
		}

		sensor->vctx->next_device = ischain;
		break;
	case FIMC_IS_DEVICE_ISCHAIN:
		ischain = (struct fimc_is_device_ischain *)device;

		devicemgr->ischain[stream] = ischain;
		break;
	default:
		mgerr("device type(%d) is invalid", group, group, group->device_type);
		BUG();
		break;
	}

p_err:
	return ret;
}

int fimc_is_devicemgr_binding(struct fimc_is_devicemgr *devicemgr,
		struct fimc_is_device_sensor *sensor,
		struct fimc_is_device_ischain *ischain,
		enum fimc_is_device_type type)
{
	int ret = 0;
	struct fimc_is_group *group;
	struct fimc_is_group *child_group;

	switch (type) {
	case FIMC_IS_DEVICE_SENSOR:
		if (test_bit(FIMC_IS_SENSOR_DRIVING, &sensor->state)) {
			/* in case of sensor driving */
			ischain->sensor = sensor;
			sensor->ischain = ischain;
			/*
			 * Forcely set the ischain's state to "Opened"
			 * Because if it's sensor driving mode, ischain was not opened from HAL.
			 */
			set_bit(FIMC_IS_ISCHAIN_OPEN, &ischain->state);

			ret = fimc_is_groupmgr_init(sensor->groupmgr, ischain);
			if (ret) {
				merr("fimc_is_groupmgr_init is fail(%d)", ischain, ret);
				ret = -EINVAL;
				goto p_err;
			}
		}
		break;
	case FIMC_IS_DEVICE_ISCHAIN:
		if (sensor &&
			!test_bit(FIMC_IS_ISCHAIN_REPROCESSING, &ischain->state)) {
			group = &sensor->group_sensor;
			BUG_ON(group->instance != ischain->instance);

			child_group = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN, group);
			if (child_group) {
				info("[%d/%d] sensor otf output set",
					sensor->instance, ischain->instance);
				set_bit(FIMC_IS_SENSOR_OTF_OUTPUT, &sensor->state);
			}
		}
		break;
	default:
		mgerr("device type(%d) is invalid", group, group, group->device_type);
		BUG();
		break;
	}

p_err:
	return ret;
}

int fimc_is_devicemgr_start(struct fimc_is_devicemgr *devicemgr,
		void *device, enum fimc_is_device_type type)
{
	int ret = 0;
	struct fimc_is_group *group;
	struct fimc_is_group *child_group;
	struct fimc_is_device_sensor *sensor;
	struct devicemgr_sensor_tag_data *tag_data;
	u32 stream;
	int i;

	switch (type) {
	case FIMC_IS_DEVICE_SENSOR:
		sensor = (struct fimc_is_device_sensor *)device;
		group = &sensor->group_sensor;
		child_group = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN, group);
		stream = group->instance;

		if (!test_bit(FIMC_IS_SENSOR_DRIVING, &sensor->state) && sensor->ischain) {
			ret = fimc_is_ischain_start_wrap(sensor->ischain, group);
			if (ret) {
				merr("fimc_is_ischain_start_wrap is fail(%d)", sensor->ischain, ret);
				ret = -EINVAL;
				goto p_err;
			}
		} else {
			ret = fimc_is_groupmgr_start(sensor->groupmgr, group->device);
			if (ret) {
				merr("fimc_is_groupmgr_start is fail(%d)", group->device, ret);
				ret = -EINVAL;
				goto p_err;
			}
		}

		/* Only in case of OTF case, used tasklet. */
		if (sensor->ischain && child_group) {
			for (i = 0; i < TAG_DATA_MAX; i++) {
				tag_data = &devicemgr->sensor_tag_data[stream][i];
				tasklet_init(&devicemgr->tasklet_sensor_tag[stream][i], tasklet_sensor_tag, (unsigned long)tag_data);
			}
		}
		break;
	case FIMC_IS_DEVICE_ISCHAIN:
		break;
	default:
		mgerr("device type(%d) is invalid", group, group, group->device_type);
		BUG();
		break;
	}

p_err:
	return ret;
}

int fimc_is_devicemgr_stop(struct fimc_is_devicemgr *devicemgr,
		void *device, enum fimc_is_device_type type)
{
	int ret = 0;
	struct fimc_is_group *group;
	struct fimc_is_device_sensor *sensor;

	switch (type) {
	case FIMC_IS_DEVICE_SENSOR:
		sensor = (struct fimc_is_device_sensor *)device;
		group = &sensor->group_sensor;

		if (!test_bit(FIMC_IS_SENSOR_DRIVING, &sensor->state) && sensor->ischain) {
			ret = fimc_is_ischain_stop_wrap(sensor->ischain, group);
			if (ret) {
				merr("fimc_is_ischain_stop_wrap is fail(%d)", sensor->ischain, ret);
				ret = -EINVAL;
				goto p_err;
			}
		} else {
			ret = fimc_is_groupmgr_stop(sensor->groupmgr, group->device);
			if (ret) {
				merr("fimc_is_groupmgr_stop is fail(%d)", group->device, ret);
				ret = -EINVAL;
				goto p_err;
			}
		}
		break;
	case FIMC_IS_DEVICE_ISCHAIN:
		break;
	default:
		mgerr("device type(%d) is invalid", group, group, group->device_type);
		BUG();
		break;
	}

p_err:
	return ret;
}

int fimc_is_devicemgr_close(struct fimc_is_devicemgr *devicemgr,
		void *device, enum fimc_is_device_type type)
{
	int ret = 0;
	struct fimc_is_group *group;
	struct fimc_is_device_sensor *sensor;

	switch (type) {
	case FIMC_IS_DEVICE_SENSOR:
		sensor = (struct fimc_is_device_sensor *)device;

		BUG_ON(!sensor);

		/*
		 * Forcely set the ischain's state to "Not Opened"
		 * Because if it's sensor driving mode, ischain was not opened from HAL.
		 */
		if (test_bit(FIMC_IS_SENSOR_DRIVING, &sensor->state) && sensor->ischain)
			clear_bit(FIMC_IS_ISCHAIN_OPEN, &sensor->ischain->state);
		break;
	case FIMC_IS_DEVICE_ISCHAIN:
		break;
	default:
		mgerr("device type(%d) is invalid", group, group, group->device_type);
		BUG();
		break;
	}

	return ret;
}

int fimc_is_devicemgr_shot_callback(struct fimc_is_group *group,
		struct fimc_is_frame *frame,
		u32 fcount,
		enum fimc_is_device_type type)
{
	int ret = 0;
	struct fimc_is_group *child_group;
	struct camera2_node ldr_node = {0, };
	struct fimc_is_devicemgr *devicemgr;
	struct devicemgr_sensor_tag_data *tag_data;
	u32 stream;
	u32 index;

	switch (type) {
	case FIMC_IS_DEVICE_SENSOR:
		child_group = GET_HEAD_GROUP_IN_DEVICE(FIMC_IS_DEVICE_ISCHAIN, group);

		PROGRAM_COUNT(9);

#ifdef DBG_STREAMING
		mgrinfo(" DEVICE SHOT CALLBACK(%d) %s\n", group->device,
			       group, frame, frame->index, child_group ? "OTF" : "M2M");
#endif

		/* OTF */
		if (child_group) {
			child_group->shot_callback(child_group->device, frame);
		/* M2M */
		} else {
#ifdef DBG_STREAMING
			mginfo("[F%d] DEVICE TASKLET M2M\n", group->device, group, frame->fcount);
#endif
			ret = fimc_is_sensor_group_tag(group->device->sensor, frame, &ldr_node);
			if (ret) {
				merr("fimc_is_sensor_group_tag is fail(%d)", group, ret);
				ret = -EINVAL;
				goto p_err;
			}
		}

		PROGRAM_COUNT(10);

		break;
	case FIMC_IS_DEVICE_ISCHAIN:
		devicemgr = group->device->devicemgr;
		index = fcount % TAG_DATA_MAX;
		stream = group->instance;

		tag_data = &devicemgr->sensor_tag_data[stream][index];
		tag_data->fcount = fcount;
		tag_data->devicemgr = devicemgr;
		tag_data->group = &devicemgr->sensor[stream]->group_sensor;
		tag_data->stream = stream;

		/* OTF */
		if (frame->type == SHOT_TYPE_EXTERNAL &&
			group->head->device_type == FIMC_IS_DEVICE_SENSOR) {
#ifdef DBG_STREAMING
			mginfo("[F%d] DEVICE TASKLET schedule\n", group->device, group, fcount);
#endif
			tasklet_schedule(&devicemgr->tasklet_sensor_tag[stream][index]);
		}
		break;
	default:
		mgerr("device type(%d) is invalid", group, group, group->device_type);
		BUG();
		break;
	}

p_err:
	return ret;
}

int fimc_is_devicemgr_shot_done(struct fimc_is_group *group,
		struct fimc_is_frame *ldr_frame,
		u32 status)
{
	int ret = 0;
	unsigned long flags;

	/* skip in case of the sensor -> 3AA M2M case */
	if (group->device_type == FIMC_IS_DEVICE_ISCHAIN)
		return ret;

	/* if error happened, cancel the sensor's subdev frames */
	if (status) {
		mginfo("[F%d] Start CANCEL Other subdev frame\n", group->device, group, ldr_frame->fcount);
		flags = fimc_is_group_lock(group, group->device_type, false);
		fimc_is_group_subdev_cancel(group, ldr_frame, group->device_type, FS_REQUEST, false);
		fimc_is_group_unlock(group, flags, group->device_type, false);
		mginfo("[F%d] End CANCEL Other subdev frame\n", group->device, group, ldr_frame->fcount);
	}

	return ret;
}
#else
int fimc_is_devicemgr_probe(struct fimc_is_devicemgr *devicemgr)
{
	return 0;
}

int fimc_is_devicemgr_open(struct fimc_is_devicemgr *devicemgr,
		void *device, enum fimc_is_device_type type)
{
	return 0;
}

int fimc_is_devicemgr_binding(struct fimc_is_devicemgr *devicemgr,
		struct fimc_is_device_sensor *sensor,
		struct fimc_is_device_ischain *ischain,
		enum fimc_is_device_type type)
{
	return 0;
}

int fimc_is_devicemgr_start(struct fimc_is_devicemgr *devicemgr,
		void *device, enum fimc_is_device_type type)
{
	return 0;
}

int fimc_is_devicemgr_stop(struct fimc_is_devicemgr *devicemgr,
		void *device, enum fimc_is_device_type type)
{
	return 0;
}

int fimc_is_devicemgr_close(struct fimc_is_devicemgr *devicemgr,
		void *device, enum fimc_is_device_type type)
{
	return 0;
}

int fimc_is_devicemgr_shot_callback(struct fimc_is_group *group,
		struct fimc_is_frame *frame,
		u32 fcount,
		enum fimc_is_device_type type)
{
	return 0;
}

int fimc_is_devicemgr_shot_done(struct fimc_is_group *group,
		struct fimc_is_frame *ldr_frame,
		u32 status)
{
	return 0;
}
#endif
