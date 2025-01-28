/*
 * Copyright (c) 2024 ttwards <12411711@mail.sustech.edu.cn>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "motor_dji.h"
#include "zephyr/device.h"
#include "zephyr/devicetree.h"
#include "zephyr/spinlock.h"
#include "zephyr/toolchain.h"
#include <string.h>
#include <math.h>
#include <soc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/_types.h>
#include <sys/types.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/time_units.h>

#define DT_DRV_COMPAT dji_motor

LOG_MODULE_REGISTER(motor_dji, CONFIG_MOTOR_LOG_LEVEL);

const struct device *motor_devices[] = {DT_INST_FOREACH_STATUS_OKAY(DJI_DEVICE_POINTER)};

const struct device *can_devices[] = {
	DT_FOREACH_CHILD_STATUS_OKAY_SEP(CAN_BUS_PATH, CAN_DEVICE_POINTER, (, ))};

#define CTRL_STRUCT_DATA(node_id)                                                                  \
	{                                                                                          \
		.can_dev = DT_GET_CANPHY_BY_BUS(node_id), .flags = 0, .full = {{false}},           \
		.mask = 0,                                                                         \
	}

static int frames_id(int tx_id)
{
	if (tx_id == 0x200) {
		return 0;
	} else if (tx_id == 0x1FF) {
		return 1;
	} else if (tx_id == 0x1FE) {
		return 2;
	} else if (tx_id == 0x2FE) {
		return 3;
	} else if (tx_id == 0x2FF) {
		return 4;
	}
	return -1; // Return a default value if no match is found
}

int get_can_id(const struct device *dev)
{
	const struct dji_motor_config *cfg = dev->config;
	for (int i = 0; i < CAN_COUNT; i++) {
		if (can_devices[i] == cfg->common.phy) {
			return i;
		}
	}
	return -1;
}

static int txframe_id(int frames_id)
{
	if (frames_id == 0) {
		return 0x200;
	} else if (frames_id == 1) {
		return 0x1FF;
	} else if (frames_id == 2) {
		return 0x1FE;
	} else if (frames_id == 3) {
		return 0x2FE;
	} else if (frames_id == 4) {
		return 0x2FF;
	}
	return -1;
}

/**
 * @brief 将 float 类型转换为 int16_t，包含溢出处理和四舍五入。
 *
 * @param value 要转换的浮点数。
 * @return 转换后的 int16_t 值。
 */
static int16_t to16t(float value)
{
	// 溢出处理
	if (value > INT16_MAX) {
		return INT16_MAX;
	} else if (value < INT16_MIN) {
		return INT16_MIN;
	} else {
		return (int16_t)value;
	}
}

struct motor_controller ctrl_structs[CAN_COUNT] = {
	DT_FOREACH_CHILD_STATUS_OKAY_SEP(CAN_BUS_PATH, CTRL_STRUCT_DATA, (, ))};

static inline motor_id_t canbus_id(const struct device *dev)
{
	for (int i = 0; i < MOTOR_COUNT; i++) {
		if (motor_devices[i] == dev) {
			return i;
		}
	}
	return 0;
}

static inline motor_id_t motor_id(const struct device *dev)
{
	const struct dji_motor_config *cfg = dev->config;
	return cfg->common.id - 1;
}

void dji_speed_limit(const struct device *dev, float max_speed, float min_speed)
{
	struct dji_motor_data *data = dev->data;
	data->common.speed_limit[0] = min_speed;
	data->common.speed_limit[1] = max_speed;
}

void dji_torque_limit(const struct device *dev, float max_torque, float min_torque)
{
	struct dji_motor_data *data = dev->data;
	data->common.torque_limit[0] = min_torque;
	data->common.torque_limit[1] = max_torque;
}

int dji_set_speed(const struct device *dev, float speed_rpm)
{
	struct dji_motor_data *data = dev->data;
	const struct dji_motor_config *cfg = dev->config;

	if (speed_rpm > data->common.speed_limit[1]) {
		speed_rpm = data->common.speed_limit[1];
	} else if (speed_rpm < data->common.speed_limit[0]) {
		speed_rpm = data->common.speed_limit[0];
	}

	data->target_rpm = speed_rpm;
	for (int i = 0; i < sizeof(cfg->common.pid_datas) / sizeof(cfg->common.pid_datas[0]); i++) {
		if (cfg->common.pid_datas[i]->pid_dev == NULL) {
			break;
		}
		if (strcmp(cfg->common.capabilities[i], "speed") == 0) {
			pid_calc(cfg->common.pid_datas[i]);
			data->current_mode_index = i;
		}
	}
	// ctrl_struct->current[id] = pid_calc(dev);
	return 0;
}

int dji_set_angle(const struct device *dev, float angle)
{
	struct dji_motor_data *data = dev->data;
	const struct dji_motor_config *cfg = dev->config;

	data->target_angle = angle;

	for (int i = 0; i < SIZE_OF_ARRAY(cfg->common.pid_datas); i++) {
		if (cfg->common.pid_datas[i]->pid_dev == NULL) {
			break;
		}
		if (strcmp(cfg->common.capabilities[i], "angle") == 0) {
			pid_calc(cfg->common.pid_datas[i]);
			data->current_mode_index = i;
		}
	}
	// ctrl_struct->current[id] = pid_calc(dev);
	return 0;
}

int dji_set_torque(const struct device *dev, float torque)
{
	struct dji_motor_data *data = dev->data;
	const struct dji_motor_config *cfg = dev->config;

	if (torque > data->common.torque_limit[1]) {
		torque = data->common.torque_limit[1];
	} else if (torque < data->common.torque_limit[0]) {
		torque = data->common.torque_limit[0];
	}

	data->target_torque = torque;
	for (int i = 0; i < SIZE_OF_ARRAY(cfg->common.pid_datas); i++) {
		if (cfg->common.pid_datas[i]->pid_dev == NULL) {
			data->current_mode_index = i + 1;
			break;
		}
		if (strcmp(cfg->common.capabilities[i], "torque") == 0) {
			pid_calc(cfg->common.pid_datas[i]);
			data->current_mode_index = i;
		}
	}
	// ctrl_struct->current[id] = pid_calc(dev);
	return 0;
}

void dji_control(const struct device *dev, enum motor_cmd cmd)
{
	struct dji_motor_data *data = dev->data;
	const struct dji_motor_config *cfg = dev->config;

	struct can_frame frame;
	frame.id = cfg->common.tx_id;

	switch (cmd) {
	case ENABLE_MOTOR:
		data->online = true;
		break;
	case DISABLE_MOTOR:
		data->online = false;
		break;
	case SET_ZERO_OFFSET:
		data->angle_add = 0;
		data->common.angle = 0;
		break;
	case CLEAR_PID:
		break;
	case CLEAR_ERROR:
		data->missed_times = 0;
		data->online = true;
		break;
	}
}

float dji_get_angle(const struct device *dev)
{
	struct dji_motor_data *data = dev->data;
	return fmodf(data->common.angle, 360.0f);
}

float dji_get_speed(const struct device *dev)
{
	struct dji_motor_data *data = dev->data;
	return data->common.rpm;
}

float dji_get_torque(const struct device *dev)
{
	struct dji_motor_data *data = dev->data;
	return data->common.torque;
}

int dji_init(const struct device *dev)
{
	/*
	    Each motor find their own controller and write their own data,
	    which cannot be done at the first init because the other motors have not
	    been instantiated.
	*/
	if (dev) {
		const struct dji_motor_config *cfg = dev->config;
		struct dji_motor_data *data = dev->data;
		uint8_t frame_id = frames_id(cfg->common.tx_id);
		uint8_t id = motor_id(dev);

		data->ctrl_struct->mask[frame_id] |= 1 << id;
		data->ctrl_struct->rx_ids[id] = cfg->common.rx_id;

		data->ctrl_struct->full_handle.handler = dji_tx_handler;

		data->online = true;
		for (int i = 0;
		     i < sizeof(cfg->common.pid_datas) / sizeof(cfg->common.pid_datas[0]); i++) {
			if (cfg->common.pid_datas[i]->pid_dev == NULL) {
				if (i > 0) {
					pid_reg_output(cfg->common.pid_datas[i - 1],
						       &data->target_torque);
				}
				break;
			}
			if (strcmp(cfg->common.capabilities[i], "speed") == 0) {
				pid_reg_input(cfg->common.pid_datas[i], &data->common.rpm,
					      &data->target_rpm);
				if (i > 0) {
					pid_reg_output(cfg->common.pid_datas[i - 1],
						       &data->target_rpm);
				}
			} else if (strcmp(cfg->common.capabilities[i], "angle") == 0) {
				pid_reg_input(cfg->common.pid_datas[i], &data->pid_angle_input,
					      &data->pid_ref_input);
			} else if (strcmp(cfg->common.capabilities[i], "torque") == 0) {
				pid_reg_input(cfg->common.pid_datas[i], &data->common.torque,
					      &data->target_torque);
				if (i > 0) {
					pid_reg_output(cfg->common.pid_datas[i - 1],
						       &data->target_torque);
				}
			} else {
				LOG_ERR("Unsupported motor mode");
				return -1;
			}
			pid_reg_time(cfg->common.pid_datas[i], &(data->curr_time),
				     &(data->prev_time));
		}
		data->current_mode_index = 0;
		data->ctrl_struct->motor_devs[id] = (struct device *)dev;
		data->prev_time = 0;
		data->ctrl_struct->flags = 0;
		data->ctrl_struct->mapping[frame_id][id % 4] = id;
		if (cfg->is_gm6020) {
			data->convert_num = GM6020_CONVERT_NUM;
		} else if (cfg->is_m3508) {
			data->convert_num = M3508_CONVERT_NUM;
		} else if (cfg->is_m2006) {
			data->convert_num = M2006_CONVERT_NUM;
		} else {
			LOG_ERR("Unsupported motor type");
		}

		if (!device_is_ready(cfg->common.phy)) {
			return -1;
		}
		if (dji_miss_handle_timer.expiry_fn == NULL) {
			k_timer_init(&dji_miss_handle_timer, dji_miss_isr_handler, NULL);
			k_timer_start(&dji_miss_handle_timer, K_NO_WAIT, K_MSEC(2));
		}
	}
	return 0;
}

void can_rx_callback(const struct device *can_dev, struct can_frame *frame, void *user_data)
{
	uint32_t curr_time = k_cycle_get_32();
	struct motor_controller *ctrl_struct = (struct motor_controller *)user_data;
	struct can_frame rx_frame = *frame;
	// Suppose it is 3508/2006
	uint8_t id = (rx_frame.id & 0xF) - 1;
	// If RX_ID does not match, it should be GM6020
	if (ctrl_struct->rx_ids[id] != rx_frame.id && id >= 4) {
		id -= 4;
	}
	// It should match, but in case we check again
	if (ctrl_struct->rx_ids[id] != rx_frame.id) {
		LOG_ERR("Unknown motor ID: %d, database: %d, received: %d", id,
			ctrl_struct->rx_ids[id], rx_frame.id);
		return;
	}
	int8_t bus_id = canbus_id(can_dev);
	if (!ctrl_struct) {
		return;
	}
	struct dji_motor_data *motor_data = ctrl_struct->motor_devs[id]->data;
	uint32_t prev_time = motor_data->curr_time;

	k_spinlock_key_t key;
	if (k_spin_trylock(&motor_data->data_input_lock, &key) != 0) {
		return;
	}

	if (!motor_data) {
		return;
	}
	if (motor_data->missed_times > 3) {
		LOG_ERR("Motor %d is responding again, resuming...", id);
		motor_data->missed_times = 0;
		motor_data->online = true;
		const struct dji_motor_config *motor_cfg =
			(const struct dji_motor_config *)ctrl_struct->motor_devs[id]->config;
		int8_t frame_id = frames_id(motor_cfg->common.tx_id);
		ctrl_struct[bus_id].mask[frame_id] ^= 1 << id;
	} else if (motor_data->missed_times > 0) {
		motor_data->missed_times--;
	}
	// Store in RAW data. Process when API is called.
	// Using FPU in ISR is not recommended, since it requires actions on registers
	motor_data->RAWprev_angle = motor_data->RAWangle;
	motor_data->RAWangle = COMBINE_HL8(rx_frame.data[0], rx_frame.data[1]);
	motor_data->RAWrpm = COMBINE_HL8(rx_frame.data[2], rx_frame.data[3]);
	motor_data->RAWcurrent = COMBINE_HL8(rx_frame.data[4], rx_frame.data[5]);
	motor_data->RAWtemp = rx_frame.data[6];
	ctrl_struct[bus_id].flags |= 1 << id;
	motor_data->curr_time = curr_time;
	motor_data->prev_time = prev_time;
	bool full = false;
	for (int i = 0; i < 5; i++) {
		uint8_t combined = ctrl_struct[bus_id].mask[i] & ctrl_struct[bus_id].flags;
		if (combined == ctrl_struct[bus_id].mask[i]) {
			//   ctrl_struct->flags ^= 0xF;
			ctrl_struct[bus_id].full[i] = true;
			full = true;
		}
	}

	if (full) {
		k_work_submit_to_queue(&dji_work_queue, &ctrl_struct[bus_id].full_handle);
	}
	// k_thread_resume(dji_motor_ctrl_thread);
	k_spin_unlock(&motor_data->data_input_lock, key);
	return;
}

static const struct can_filter filter20x = {.id = 0x200, .mask = 0x3F0, .flags = 0};

static void can_tx_callback(const struct device *can_dev, int error, void *user_data)
{
	struct k_sem *queue_sem = user_data;
	k_sem_give(queue_sem);
}

static void proceed_delta_degree(const struct device *dev)
{
	struct dji_motor_data *data_temp = dev->data;
	const struct dji_motor_config *config_temp = dev->config;
	int delta = data_temp->RAWangle - data_temp->RAWprev_angle;
	if (data_temp->RAWangle < 2048 && data_temp->RAWprev_angle > 6144) {
		delta += 8192;
		data_temp->common.round_cnt++;
		if (data_temp->target_angle < 0 || data_temp->target_angle > 360) {
			data_temp->target_angle -= 360;
		}
	} else if (data_temp->RAWangle > 6144 && data_temp->RAWprev_angle < 2048) {
		delta -= 8192;
		data_temp->common.round_cnt--;
		if (data_temp->target_angle < 0 || data_temp->target_angle > 360) {
			data_temp->target_angle += 360;
		}
	}

	if (fabsf(config_temp->gear_ratio - 1) > 0.001f) {
		// I dont know why RAW_angle for M3508 is 4 times of angle
		data_temp->angle_add +=
			(float)(delta)*convert[data_temp->convert_num][ANGLE2DEGREE] /
			(config_temp->gear_ratio);

		data_temp->common.angle = fmodf(data_temp->angle_add, 360.0f);
		while (data_temp->common.angle < 0) {
			data_temp->common.angle += 360.0f;
		}
	} else {
		data_temp->common.angle = (float)(data_temp->RAWangle) *
					  convert[data_temp->convert_num][ANGLE2DEGREE];
	}

	float delta_angle = data_temp->common.angle - data_temp->target_angle;

	if (delta_angle > 0) {
		if (delta_angle < 180) {
			data_temp->pid_angle_input = delta_angle;
		} else {
			data_temp->pid_angle_input = delta_angle - 360.0f;
		}
	} else if (delta_angle < 0) {
		if (delta_angle > -180) {
			data_temp->pid_angle_input = delta_angle;
		} else {
			data_temp->pid_angle_input = delta_angle + 360.0f;
		}
	}
}

static void can_pack_add(uint8_t *data, struct device *motor_dev, uint8_t num)
{
	struct dji_motor_data *data_temp = motor_dev->data;

	int16_t value = to16t(data_temp->target_current);

	data[num * 2] = HIGH_BYTE(value);
	data[num * 2 + 1] = LOW_BYTE(value);
}

static void dji_timeout_handle(const struct device *dev, uint32_t curr_time,
			       struct motor_controller *ctrl_struct)
{
	struct dji_motor_data *motor_data = (struct dji_motor_data *)dev->data;
	const struct dji_motor_config *motor_cfg = (const struct dji_motor_config *)dev->config;

	if (motor_data->online == false) {
		return;
	}
	uint32_t prev_time = motor_data->curr_time;
	if (k_cyc_to_us_near32(curr_time - prev_time) > 2000) {
		motor_data->missed_times++;
		if (motor_data->missed_times > 3) {
			LOG_ERR("Motor %d is not responding", motor_cfg->common.id);
			ctrl_struct->mask[frames_id(motor_cfg->common.tx_id)] ^=
				1 << (motor_cfg->common.id - 1);
			motor_data->online = false;
		}
	}
}

static void motor_calc(const struct device *dev)
{
	const struct device *dev_temp = dev;
	struct dji_motor_data *data_temp = dev_temp->data;
	k_spinlock_key_t key;
	if (k_spin_trylock(&data_temp->data_input_lock, &key) != 0) {
		return;
	}
	const struct dji_motor_config *config_temp = dev_temp->config;
	// Proceed the RAW data
	// Add up to avoid circular overflow
	proceed_delta_degree(dev_temp);

	data_temp->common.rpm = data_temp->RAWrpm * convert[data_temp->convert_num][SPEED2RPM] /
				config_temp->gear_ratio;
	data_temp->common.torque = data_temp->RAWcurrent *
				   convert[data_temp->convert_num][CURRENT2TORQUE] *
				   config_temp->gear_ratio;

	// If current_mode_index is -1, it means the motor is in torque control.
	// From the current mode index to the end of the controller array
	bool torque_proceeded = false;
	for (int i = data_temp->current_mode_index;
	     i < SIZE_OF_ARRAY(config_temp->common.capabilities); i++) {
		if (config_temp->common.pid_datas[i]->pid_dev == NULL) {
			if (torque_proceeded) {
				break;
			}
			if (data_temp->target_torque > data_temp->common.torque_limit[1]) {
				data_temp->target_torque = data_temp->common.torque_limit[1];
			} else if (data_temp->target_torque < data_temp->common.torque_limit[0]) {
				data_temp->target_torque = data_temp->common.torque_limit[0];
			}
			data_temp->target_current = data_temp->target_torque /
						    config_temp->gear_ratio *
						    convert[data_temp->convert_num][TORQUE2CURRENT];
			break;
		}

		pid_calc(config_temp->common.pid_datas[i]);

		if (strcmp(config_temp->common.capabilities[i], "angle") == 0) {
			if (data_temp->target_rpm > data_temp->common.speed_limit[1]) {
				data_temp->target_rpm = data_temp->common.speed_limit[1];
			} else if (data_temp->target_rpm < data_temp->common.speed_limit[0]) {
				data_temp->target_rpm = data_temp->common.speed_limit[0];
			}
		}

		if (strcmp(config_temp->common.capabilities[i], "torque") == 0) {
			torque_proceeded = true;
		} else if (strcmp(config_temp->common.capabilities[i], "mit") == 0) {
			break;
		}
	}
	k_spin_unlock(&data_temp->data_input_lock, key);
}

struct can_frame txframe;

static struct k_sem dji_thread_sem;

void dji_miss_isr_handler(struct k_timer *dummy)
{
	ARG_UNUSED(dummy);
	k_work_submit_to_queue(&dji_work_queue, &dji_miss_handle);
}

void dji_miss_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int curr_time = k_cycle_get_32();
	for (int i = 0; i < CAN_COUNT; i++) {
		for (int j = 0; j < 8; j++) {
			if (ctrl_structs[i].motor_devs[j]) {
				dji_timeout_handle(ctrl_structs[i].motor_devs[j], curr_time,
						   &ctrl_structs[i]);
			}
		}
	}
}

void dji_init_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	struct device *can_dev = NULL;
	for (int i = 0; i < CAN_COUNT; i++) {
		k_sem_init(&ctrl_structs[i].tx_queue_sem, 3, 3); // 初始化信号量

		can_dev = (struct device *)ctrl_structs[i].can_dev;
		can_start(can_dev);

		int err = can_add_rx_filter(can_dev, can_rx_callback, &ctrl_structs[i], &filter20x);

		if (err < 0) {
			LOG_ERR("Error adding CAN filter (err %d)", err);
		}
		// If you recieved an error here, remember that 2# CAN of STM32 is in slave
		// mode and does not have an independent filter. If that is the issue, you
		// can ignore that.
	}
	dji_miss_handle_timer.expiry_fn = dji_miss_isr_handler;
}

void dji_tx_handler(struct k_work *work)
{
	struct motor_controller *ctrl_struct =
		CONTAINER_OF(work, struct motor_controller, full_handle);

	for (int i = 0; i < 5; i++) { // For each frame
		if (ctrl_struct->full[i]) {
			uint8_t id_temp = ctrl_struct->mapping[0][0];
			uint8_t data[8] = {0};
			bool packed = false;
			for (int j = 0; j < 4; j++) {
				id_temp = ctrl_struct->mapping[i][j];
				if (id_temp < 0) {
					continue;
				}
				const struct device *dev = ctrl_struct->motor_devs[id_temp];
				struct dji_motor_data *data_temp = dev->data;
				// Check if recieved just now.
				if (id_temp < 8 &&
				    ((ctrl_struct->flags & (1 << id_temp)) || !data_temp->online)) {
					motor_calc(ctrl_struct->motor_devs[id_temp]);
					can_pack_add(data, ctrl_struct->motor_devs[id_temp], j);
					ctrl_struct->flags ^= 1 << id_temp;
					packed = true;
				}
			}
			if (packed) {
				txframe.id = txframe_id(i);
				txframe.dlc = 8;
				txframe.flags = 0;
				memcpy(txframe.data, data, sizeof(data));
				const struct device *can_dev = ctrl_struct->can_dev;
				struct k_sem tx_queue_sem = ctrl_struct->tx_queue_sem;
				int err = k_sem_take(&tx_queue_sem, K_NO_WAIT);
				if (err == 0) {
					err = can_send(can_dev, &txframe, K_NO_WAIT,
						       can_tx_callback, &tx_queue_sem);
				}
				if (err != 0 && err != -EAGAIN && err != -EBUSY) {
					LOG_ERR("Error sending CAN frame (err %d)", err);
				}
			}
		}
	}
}

DT_INST_FOREACH_STATUS_OKAY(DMOTOR_INST)