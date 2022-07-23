// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2016 Intel Corporation
 * Copyright (c) 2020-2022 Dorian Stoll
 *
 * Linux driver for Intel Precise Touch & Stylus
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/mei_cl_bus.h>
#include <linux/moduleparam.h>
#include <linux/types.h>

#include "cmd.h"
#include "context.h"
#include "control.h"
#include "hid.h"
#include "protocol.h"
#include "resources.h"

static int ipts_receiver_handle_get_device_info(struct ipts_context *ipts,
						struct ipts_response *rsp)
{
	memcpy(&ipts->device_info, rsp->payload, sizeof(ipts->device_info));

	return ipts_cmd_set_mode(ipts, ipts->mode);
}

static int ipts_receiver_handle_set_mode(struct ipts_context *ipts)
{
	int ret;

	// Allocate buffers ...
	ret = ipts_resources_alloc(ipts);
	if (ret) {
		dev_err(ipts->dev, "Failed to allocate resources\n");
		return ret;
	}

	// ... and send them to the hardware.
	return ipts_cmd_set_mem_window(ipts);
}

static int ipts_receiver_handle_set_mem_window(struct ipts_context *ipts)
{
	// Update host status
	ipts->status = IPTS_HOST_STATUS_STARTED;

	// Initialize HID device
	ipts_hid_init(ipts);

	// Notify wait queue
	complete_all(&ipts->on_device_ready);

	// Host and Hardware are now ready to receive data
	return ipts_cmd_ready_for_data(ipts);
}

static int ipts_receiver_handle_ready_for_data(struct ipts_context *ipts)
{
	u32 *doorbell = (u32 *)ipts->doorbell.address;

	if (ipts->mode != IPTS_MODE_SINGLETOUCH)
		return 0;

	// Trigger a doorbell update
	*doorbell = *doorbell + 1;
	return 0;
}

static int ipts_receiver_handle_feedback(struct ipts_context *ipts,
					 struct ipts_response *rsp)
{
	// In singletouch mode, the READY_FOR_DATA command
	// needs to be resent after every feedback command.
	if (ipts->mode == IPTS_MODE_SINGLETOUCH)
		return ipts_cmd_ready_for_data(ipts);

	return 0;
}

static int ipts_receiver_handle_reset(struct ipts_context *ipts)
{
	// Update host status (this disables receiving messages from MEI)
	ipts->status = IPTS_HOST_STATUS_STOPPED;

	// If the host is restarting, don't clear
	// resources and restart immideately.
	if (ipts->restart) {
		msleep(1000);
		return ipts_control_start(ipts);
	}

	ipts_resources_free(ipts);
	ipts_hid_free(ipts);

	return 0;
}

static bool ipts_receiver_handle_error(struct ipts_context *ipts,
				       struct ipts_response *rsp)
{
	bool error;

	switch (rsp->status) {
	case IPTS_STATUS_SUCCESS:
	case IPTS_STATUS_COMPAT_CHECK_FAIL:
		error = false;
		break;
	case IPTS_STATUS_INVALID_PARAMS:
		error = rsp->code != IPTS_RSP_FEEDBACK;
		break;
	case IPTS_STATUS_SENSOR_DISABLED:
	case IPTS_STATUS_SENSOR_EXPECTED_RESET:
		error = ipts->status != IPTS_HOST_STATUS_STOPPING;
		break;
	default:
		error = true;
		break;
	}

	if (!error)
		return false;

	dev_err(ipts->dev, "Command 0x%08x failed: %d\n", rsp->code,
		rsp->status);

	return true;
}

static void ipts_receiver_handle_response(struct ipts_context *ipts,
					  struct ipts_response *rsp)
{
	int ret;

	// If the sensor was reset, initiate a restart
	if (rsp->status == IPTS_STATUS_SENSOR_UNEXPECTED_RESET) {
		dev_info(ipts->dev, "Sensor was reset\n");

		if (ipts_control_restart(ipts))
			dev_err(ipts->dev, "Failed to restart IPTS\n");

		return;
	}

	if (ipts_receiver_handle_error(ipts, rsp))
		return;

	switch (rsp->code) {
	case IPTS_RSP_GET_DEVICE_INFO:
		ret = ipts_receiver_handle_get_device_info(ipts, rsp);
		break;
	case IPTS_RSP_SET_MODE:
		ret = ipts_receiver_handle_set_mode(ipts);
		break;
	case IPTS_RSP_SET_MEM_WINDOW:
		ret = ipts_receiver_handle_set_mem_window(ipts);
		break;
	case IPTS_RSP_READY_FOR_DATA:
		ret = ipts_receiver_handle_ready_for_data(ipts);
		break;
	case IPTS_RSP_FEEDBACK:
		ret = ipts_receiver_handle_feedback(ipts, rsp);
		break;
	case IPTS_RSP_RESET_SENSOR:
		ret = ipts_receiver_handle_reset(ipts);
		break;
	default:
		ret = 0;
		break;
	}

	if (!ret)
		return;

	dev_err(ipts->dev, "Error while handling response 0x%08x: %d\n",
		rsp->code, ret);

	if (ipts_control_stop(ipts))
		dev_err(ipts->dev, "Failed to stop IPTS\n");
}

void ipts_receiver_callback(struct mei_cl_device *cldev)
{
	int ret;
	struct ipts_response rsp;
	struct ipts_context *ipts;

	ipts = mei_cldev_get_drvdata(cldev);

	// Ignore incoming messages if the host is stopped
	if (ipts->status == IPTS_HOST_STATUS_STOPPED)
		return;

	ret = mei_cldev_recv(cldev, (u8 *)&rsp, sizeof(rsp));
	if (ret <= 0) {
		dev_err(ipts->dev, "Error while reading response: %d\n", ret);
		return;
	}

	ipts_receiver_handle_response(ipts, &rsp);
}
