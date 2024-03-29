/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <mach/cpufreq.h>

#define MSM_THERMAL_WORK_TIMEOUT 500

unsigned int temp_threshold = 70;
module_param(temp_threshold, int, 0644);

struct thermal_info {
	uint32_t max_freq;
	uint32_t time_left;
	bool throttling;
	bool pending_change;
};

struct thermal_level {
	uint32_t diff;
	uint32_t freq;
	uint32_t time;
};

static struct thermal_info info = {
	.max_freq = UINT_MAX,
	.time_left = 0,
	.throttling = false,
	.pending_change = false
};

static struct thermal_level levels[] = {
	{ .diff = 15, .freq = 729600,  .time = 4000 },
	{ .diff = 12, .freq = 1190400, .time = 3000 },
	{ .diff = 9,  .freq = 1497600, .time = 3000 },
	{ .diff = 5,  .freq = 1728000, .time = 2000 },
	{ .diff = 0,  .freq = 1958400, .time = 2000 }
};

static struct msm_thermal_data msm_thermal_info;
static struct delayed_work check_temp_work;
static struct workqueue_struct *thermal_wq;

static int msm_thermal_cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_ADJUST && !info.pending_change)
		return NOTIFY_OK;

	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
		info.max_freq);

	return NOTIFY_OK;
}

static struct notifier_block msm_thermal_cpufreq_notifier = {
	.notifier_call = msm_thermal_cpufreq_callback,
};

static bool limit_cpu_freq(uint32_t freq, uint32_t time, uint32_t time_passed)
{
	unsigned int cpu;

	if (info.time_left < time_passed) info.time_left = 0;
	else info.time_left = info.time_left - time_passed;

	if (info.max_freq < freq && info.time_left > 0) return false;
	else if (info.max_freq == freq) return false;

	info.max_freq = freq;
	info.time_left = time;
	info.pending_change = true;

	get_online_cpus();
	for_each_online_cpu(cpu) {
		cpufreq_update_policy(cpu);
	}
	put_online_cpus();

	info.pending_change = false;
	return true;
}

static void check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	long temp = 0;
	int i = 0;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	tsens_get_temp(&tsens_dev, &temp);

	if (info.throttling) {
		if (temp < temp_threshold) {
			info.throttling = !limit_cpu_freq(UINT_MAX, 0, MSM_THERMAL_WORK_TIMEOUT);
			goto reschedule;
		}
	}

	for (i = 0; i < sizeof(levels)/sizeof(struct thermal_level); i++) {
		if (temp > temp_threshold + levels[i].diff) {
			limit_cpu_freq(levels[i].freq, levels[i].time, MSM_THERMAL_WORK_TIMEOUT);
			info.throttling = true;
			goto reschedule;
		}
	}

reschedule:
	queue_delayed_work(thermal_wq, &check_temp_work, msecs_to_jiffies(MSM_THERMAL_WORK_TIMEOUT));
}

static int __devinit msm_thermal_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *node = pdev->dev.of_node;
	struct msm_thermal_data data;

	memset(&data, 0, sizeof(struct msm_thermal_data));

	ret = of_property_read_u32(node, "qcom,sensor-id", &data.sensor_id);
	if (ret) {
		pr_err("thermals: if this fails here, we're in troble\n");
		goto err;
	}

	WARN_ON(data.sensor_id >= TSENS_MAX_SENSORS);

	memcpy(&msm_thermal_info, &data, sizeof(struct msm_thermal_data));

	ret = cpufreq_register_notifier(&msm_thermal_cpufreq_notifier,
		CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("thermals: well, if this fails here, we're fucked\n");
		goto err;
	}

	thermal_wq = alloc_workqueue("thermal_wq", WQ_HIGHPRI, 0);
	if (!thermal_wq) {
		pr_err("thermals: don't worry, if this fails we're also bananas\n");
		goto err;
	}

	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	queue_delayed_work(thermal_wq, &check_temp_work, 5);

err:
	return ret;
}

static int msm_thermal_dev_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&check_temp_work);
	destroy_workqueue(thermal_wq);
	cpufreq_unregister_notifier(&msm_thermal_cpufreq_notifier,
                        CPUFREQ_POLICY_NOTIFIER);
	return 0;
}

static struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal"},
	{},
};

static struct platform_driver msm_thermal_device_driver = {
	.probe = msm_thermal_dev_probe,
	.remove = msm_thermal_dev_remove,
	.driver = {
		.name = "msm-thermal",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
};

static int __init msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}

static void __exit msm_thermal_device_exit(void)
{
	platform_driver_unregister(&msm_thermal_device_driver);
}

late_initcall(msm_thermal_device_init);
module_exit(msm_thermal_device_exit);
