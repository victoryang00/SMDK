/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2015-2020 Intel Corporation. All rights reserved. */
#ifndef __NDCTL_ACTION_H__
#define __NDCTL_ACTION_H__
enum device_action {
	ACTION_ENABLE,
	ACTION_DISABLE,
	ACTION_CREATE,
	ACTION_DESTROY,
	ACTION_CHECK,
	ACTION_WAIT,
	ACTION_START,
	ACTION_CLEAR,
	ACTION_ACTIVATE,
	ACTION_READ_INFOBLOCK,
	ACTION_WRITE_INFOBLOCK,
};
#endif /* __NDCTL_ACTION_H__ */