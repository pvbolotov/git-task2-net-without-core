/*
 * Input layer to RF Kill interface connector
 *
 * Copyright (c) 2007 Dmitry Torokhov
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/rfkill.h>

#include "rfkill-input.h"

MODULE_AUTHOR("Dmitry Torokhov <dtor@mail.ru>");
MODULE_DESCRIPTION("Input layer to RF switch connector");
MODULE_LICENSE("GPL");

struct rfkill_task {
	struct work_struct work;
	enum rfkill_type type;
	struct mutex mutex; /* ensures that task is serialized */
	spinlock_t lock; /* for accessing last and desired state */
	unsigned long last; /* last schedule */
	enum rfkill_state desired_state; /* on/off */
};

static void rfkill_task_handler(struct work_struct *work)
{
	struct rfkill_task *task = container_of(work, struct rfkill_task, work);

	mutex_lock(&task->mutex);

	rfkill_switch_all(task->type, task->desired_state);

	mutex_unlock(&task->mutex);
}

static void rfkill_task_epo_handler(struct work_struct *work)
{
	rfkill_epo();
}

static DECLARE_WORK(epo_work, rfkill_task_epo_handler);

static void rfkill_schedule_epo(void)
{
	schedule_work(&epo_work);
}

static void rfkill_schedule_set(struct rfkill_task *task,
				enum rfkill_state desired_state)
{
	unsigned long flags;

	if (unlikely(work_pending(&epo_work)))
		return;

	spin_lock_irqsave(&task->lock, flags);

	if (time_after(jiffies, task->last + msecs_to_jiffies(200))) {
		task->desired_state = desired_state;
		task->last = jiffies;
		schedule_work(&task->work);
	}

	spin_unlock_irqrestore(&task->lock, flags);
}

static void rfkill_schedule_toggle(struct rfkill_task *task)
{
	unsigned long flags;

	if (unlikely(work_pending(&epo_work)))
		return;

	spin_lock_irqsave(&task->lock, flags);

	if (time_after(jiffies, task->last + msecs_to_jiffies(200))) {
		task->desired_state = !task->desired_state;
		task->last = jiffies;
		schedule_work(&task->work);
	}

	spin_unlock_irqrestore(&task->lock, flags);
}

#define DEFINE_RFKILL_TASK(n, t)			\
	struct rfkill_task n = {			\
		.work = __WORK_INITIALIZER(n.work,	\
				rfkill_task_handler),	\
		.type = t,				\
		.mutex = __MUTEX_INITIALIZER(n.mutex),	\
		.lock = __SPIN_LOCK_UNLOCKED(n.lock),	\
		.desired_state = RFKILL_STATE_ON,	\
	}

static DEFINE_RFKILL_TASK(rfkill_wlan, RFKILL_TYPE_WLAN);
static DEFINE_RFKILL_TASK(rfkill_bt, RFKILL_TYPE_BLUETOOTH);
static DEFINE_RFKILL_TASK(rfkill_uwb, RFKILL_TYPE_UWB);
static DEFINE_RFKILL_TASK(rfkill_wimax, RFKILL_TYPE_WIMAX);
static DEFINE_RFKILL_TASK(rfkill_wwan, RFKILL_TYPE_WWAN);

static void rfkill_event(struct input_handle *handle, unsigned int type,
			unsigned int code, int data)
{
	if (type == EV_KEY && data == 1) {
		switch (code) {
		case KEY_WLAN:
			rfkill_schedule_toggle(&rfkill_wlan);
			break;
		case KEY_BLUETOOTH:
			rfkill_schedule_toggle(&rfkill_bt);
			break;
		case KEY_UWB:
			rfkill_schedule_toggle(&rfkill_uwb);
			break;
		case KEY_WIMAX:
			rfkill_schedule_toggle(&rfkill_wimax);
			break;
		default:
			break;
		}
	} else if (type == EV_SW) {
		switch (code) {
		case SW_RFKILL_ALL:
			/* EVERY radio type. data != 0 means radios ON */
			/* handle EPO (emergency power off) through shortcut */
			if (data) {
				rfkill_schedule_set(&rfkill_wwan,
						    RFKILL_STATE_ON);
				rfkill_schedule_set(&rfkill_wimax,
						    RFKILL_STATE_ON);
				rfkill_schedule_set(&rfkill_uwb,
						    RFKILL_STATE_ON);
				rfkill_schedule_set(&rfkill_bt,
						    RFKILL_STATE_ON);
				rfkill_schedule_set(&rfkill_wlan,
						    RFKILL_STATE_ON);
			} else
				rfkill_schedule_epo();
			break;
		default:
			break;
		}
	}
}

static int rfkill_connect(struct input_handler *handler, struct input_dev *dev,
			  const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "rfkill";

	error = input_register_handle(handle);
	if (error)
		goto err_free_handle;

	error = input_open_device(handle);
	if (error)
		goto err_unregister_handle;

	return 0;

 err_unregister_handle:
	input_unregister_handle(handle);
 err_free_handle:
	kfree(handle);
	return error;
}

static void rfkill_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id rfkill_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_WLAN)] = BIT_MASK(KEY_WLAN) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_BLUETOOTH)] = BIT_MASK(KEY_BLUETOOTH) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_UWB)] = BIT_MASK(KEY_UWB) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_WIMAX)] = BIT_MASK(KEY_WIMAX) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_SWBIT,
		.evbit = { BIT(EV_SW) },
		.swbit = { [BIT_WORD(SW_RFKILL_ALL)] = BIT_MASK(SW_RFKILL_ALL) },
	},
	{ }
};

static struct input_handler rfkill_handler = {
	.event =	rfkill_event,
	.connect =	rfkill_connect,
	.disconnect =	rfkill_disconnect,
	.name =		"rfkill",
	.id_table =	rfkill_ids,
};

static int __init rfkill_handler_init(void)
{
	return input_register_handler(&rfkill_handler);
}

static void __exit rfkill_handler_exit(void)
{
	input_unregister_handler(&rfkill_handler);
	flush_scheduled_work();
}

module_init(rfkill_handler_init);
module_exit(rfkill_handler_exit);
