/*
 * Copyright (c) 2009, Code Aurora Forum. All rights reserved.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/kernel_stat.h>
#include <linux/workqueue.h>
#include <linux/slab.h>

#include "avs.h"

#define TEMPRS 16                /* total number of temperature regions */
#define GET_TEMPR() (avs_get_tscsr() >> 28) /* scale TSCSR[CTEMP] to regions */

struct mutex avs_lock;

static int debug=0;
module_param(debug, int, 00644);

static int enabled=0;

int avs_enabled(void) {
	return enabled;
}

static int vdd_max=VOLTAGE_MAX;
module_param(vdd_max, int, 00644);

static int vdd_min=VOLTAGE_MIN;
module_param(vdd_min, int, 00644);

static struct avs_state_s
{
	u32 freq_cnt;		/* Frequencies supported list */
	short *avs_v;		/* Dyanmically allocated storage for
				 * 2D table of voltages over temp &
				 * freq.  Used as a set of 1D tables.
				 * Each table is for a single temp.
				 * For usage see avs_get_voltage
				 */
	int (*set_vdd) (int);	/* Function Ptr for setting voltage */
	int changing;		/* Clock frequency is changing */
	u32 freq_idx;           /* Current frequency index */
	int vdd;                /* Current ACPU voltage */
	int current_tempr;	/* Current Temperature */
	short *default_vdd;	/* Default Voltages */
	int *freq;		/* Frequencies */
} avs_state;


static int vdd_index;
module_param(vdd_index,int,00644);

static int vdd_value;
static int set_vdd(const char *val, struct kernel_param *kp) {
	int hr,i;
	hr = param_set_int(val, kp);
	for(i=0;i<TEMPRS;i++) {
		avs_state.avs_v[i*avs_state.freq_cnt+vdd_index]=vdd_value;
	}
	return 0;
}

int get_vdd(char *buffer, struct kernel_param *kp) {
	int i;
	int c=0;

	for(i=0;i<TEMPRS;i++) {
		c+=sprintf(buffer+c,"%d ",avs_state.avs_v[i*avs_state.freq_cnt+vdd_index]);
	}
	return c;
}

module_param_call(vdd, set_vdd, get_vdd, &vdd_value, 00644);

int get_status(char *buffer, struct kernel_param *kp) {
        int i;
        int c=0;

	c+=sprintf(buffer+c,"Current TEMPR=%d\nCurrent Index=%d\nCurrent Vdd=%d\nVdd Table:\n",
		avs_state.current_tempr,avs_state.freq_idx,avs_state.vdd);

        for(i=0;i<avs_state.freq_cnt;i++) {
                c+=sprintf(buffer+c,"%2d:%7d %d\n",i,avs_state.freq[i],avs_state.avs_v[avs_state.current_tempr*avs_state.freq_cnt+i]);
        }
        return c;
}
module_param_call(status, NULL, get_status, NULL, 00644);
/*
 *  Update the AVS voltage vs frequency table, for current temperature
 *  Adjust based on the AVS delay circuit hardware status
 */

static int cpu_av=0;
static int l2_av=0;
static int vu_av=0;

static void avs_update_voltage_table(short *vdd_table)
{
	u32 avscsr;
	int cpu;
	int vu;
	int l2;
	int i;
	u32 cur_freq_idx;
	short cur_voltage;

	cur_freq_idx = avs_state.freq_idx;
	cur_voltage = avs_state.vdd;

	// don't update the MPLL based entry
	if(avs_state.freq[avs_state.freq_idx]==245760)
		return;

	avscsr = avs_test_delays();
	AVSDEBUG("avscsr=%x, avsdscr=%x\n", avscsr, avs_get_avsdscr());

	/*
	 * Read the results for the various unit's AVS delay circuits
	 * 2=> up, 1=>down, 0=>no-change
	 */
	cpu = ((avscsr >> 23) & 2) + ((avscsr >> 16) & 1);
	vu  = ((avscsr >> 28) & 2) + ((avscsr >> 21) & 1);
	l2  = ((avscsr >> 29) & 2) + ((avscsr >> 22) & 1);

	if ((cpu == 3) || (vu == 3) || (l2 == 3)) {
		printk(KERN_ERR "AVS: Dly Synth O/P error\n");
	} else if ((cpu == 2) || (l2 == 2) || (vu == 2)) {
		/*
		 * even if one oscillator asks for up, increase the voltage,
		 * as its an indication we are running outside the
		 * critical acceptable range of v-f combination.
		 */
		AVSDEBUG("cpu=%d l2=%d vu=%d\n", cpu, l2, vu);
		AVSDEBUG("Voltage up at %d\n", cur_freq_idx);

		if (cur_voltage >= vdd_max)
			printk(KERN_ERR
				"AVS: Voltage can not get high enough!\n");

		/* Raise the voltage for all frequencies */
		for (i = 0; i < avs_state.freq_cnt; i++) {
			vdd_table[i] = cur_voltage + VOLTAGE_STEP;
			if (vdd_table[i] > vdd_max)
				vdd_table[i] = vdd_max;
		}
	} else {
		// exponential averaging is used to decide if we need to lower 
		// the voltage.
		cpu_av=cpu_av/2;
		l2_av=l2_av/2;
		vu_av=vu_av/2;
		if(cpu==1) cpu_av+=50;
		if(l2==1) l2_av+=50;
		if(vu==1) vu_av+=50;
		AVSDEBUG("CPU=%d, L2=%d, VU=%d\n",cpu_av,l2_av,vu_av);

		if ((cpu_av > 90) && (l2_av > 90) && (vu_av >90)) {
		cpu_av=l2_av=vu_av=0;
		if ((cur_voltage - VOLTAGE_STEP >= vdd_min) &&
		    (cur_voltage <= vdd_table[cur_freq_idx])) {
			vdd_table[cur_freq_idx] = cur_voltage - VOLTAGE_STEP;
			AVSDEBUG("Voltage down for %d and lower levels\n",
				cur_freq_idx);

			/* clamp to this voltage for all lower levels */
			for (i = 0; i < cur_freq_idx; i++) {
				if (vdd_table[i] > vdd_table[cur_freq_idx])
					vdd_table[i] = vdd_table[cur_freq_idx];
			}
		}
		}
	}
}

/*
 * Return the voltage for the target performance freq_idx and optionally
 * use AVS hardware to check the present voltage freq_idx
 */
static short avs_get_target_voltage(int freq_idx, bool update_table)
{
	unsigned cur_tempr = GET_TEMPR();
	unsigned temp_index = cur_tempr*avs_state.freq_cnt;

	/* Table of voltages vs frequencies for this temp */
	short *vdd_table = avs_state.avs_v + temp_index;

	if(avs_state.current_tempr!=cur_tempr) { // only print tempr if it changes.
		avs_state.current_tempr=cur_tempr;
		AVSDEBUG("TEMPR=%d\n",cur_tempr);
	}

	if (update_table)
		avs_update_voltage_table(vdd_table);

	return vdd_table[freq_idx];
}


/*
 * Set the voltage for the freq_idx and optionally
 * use AVS hardware to update the voltage
 */
static int avs_set_target_voltage(int freq_idx, bool update_table)
{
	int rc = 0;
	int new_voltage = avs_get_target_voltage(freq_idx, update_table);
	if (avs_state.vdd != new_voltage) {
		AVSDEBUG("AVS setting V to %d mV @%d\n",
			new_voltage, freq_idx);
		rc = avs_state.set_vdd(new_voltage);
		if (rc)
			return rc;
		avs_state.vdd = new_voltage;
		cpu_av=l2_av=vu_av=0;
	}
	return rc;
}

/*
 * Notify avs of clk frquency transition begin & end
 */
int avs_adjust_freq(u32 freq_idx, int begin)
{
	int rc = 0;

	if (!avs_state.set_vdd) {
		/* AVS not initialized */
		return 0;
	}

	if (freq_idx >= avs_state.freq_cnt) {
 		AVSDEBUG("Out of range :%d\n", freq_idx);
		return -EINVAL;
	}

	mutex_lock(&avs_lock);
	if ((begin && (freq_idx > avs_state.freq_idx)) ||
	    (!begin && (freq_idx < avs_state.freq_idx))) {
		/* Update voltage before increasing frequency &
		 * after decreasing frequency
		 */
		rc = avs_set_target_voltage(freq_idx, 0);
		if (rc)
			goto aaf_out;

		avs_state.freq_idx = freq_idx;
	}
	avs_state.changing = begin;
aaf_out:
	mutex_unlock(&avs_lock);

	return rc;
}


static struct delayed_work avs_work;
static struct workqueue_struct  *kavs_wq;
#define AVS_DELAY msecs_to_jiffies(50)

static void do_avs_timer(struct work_struct *work)
{
	int cur_freq_idx;

	mutex_lock(&avs_lock);
	if (!avs_state.changing) {
		/* Only adjust the voltage if clk is stable */
		cur_freq_idx = avs_state.freq_idx;
		avs_set_target_voltage(cur_freq_idx, 1);
	}
	mutex_unlock(&avs_lock);
	queue_delayed_work_on(0, kavs_wq, &avs_work, AVS_DELAY);
}


static void  avs_timer_init(void)
{
	INIT_DELAYED_WORK_DEFERRABLE(&avs_work, do_avs_timer);
	queue_delayed_work_on(0, kavs_wq, &avs_work, AVS_DELAY);
}

static void  avs_timer_exit(void)
{
	cancel_delayed_work(&avs_work);
}

static int  avs_work_init(void)
{
	kavs_wq = create_workqueue("avs");
	if (!kavs_wq) {
		printk(KERN_ERR "AVS initialization failed\n");
		return -EFAULT;
	}
	avs_timer_init();

	return 1;
}

static void  avs_work_exit(void)
{
	avs_timer_exit();
	destroy_workqueue(kavs_wq);
	kavs_wq=0;
}

extern int acpuclk_get_index(void);

void avs_enable(int i) {
	if(i) {
		avs_reset_delays(AVSDSCR_INPUT);
        	avs_set_tscsr(TSCSR_INPUT);
		if(!kavs_wq) {
			avs_adjust_freq(acpuclk_get_index(), 0);
			avs_work_init();
		}
	} else {
		avs_disable();
		avs_work_exit();
	}
}

void avs_set_default_vdds(void) {
	int i,j;
	for (i = 0; i < TEMPRS; i++)
		for(j=0;j<avs_state.freq_cnt; j++)
			avs_state.avs_v[i*avs_state.freq_cnt+j] = avs_state.default_vdd[j];
}

static int set_avs(const char *val, struct kernel_param *kp)
{
	int hr;
	hr = param_set_int(val, kp);
	printk("AVS Enable(%d)\n",enabled);
	avs_enable(enabled);
	return 0;
}

module_param_call(enabled, set_avs, param_get_int, &enabled, 00644);

int avs_init(int (*set_vdd)(int), u32 freq_cnt, u32 freq_idx, short *vdd_table, int *freq_table)
{
	int j;

	mutex_init(&avs_lock);

	if (freq_cnt == 0)
		return -EINVAL;

	avs_state.freq_cnt = freq_cnt;

	if (freq_idx >= avs_state.freq_cnt)
		return -EINVAL;

	avs_state.avs_v = kmalloc(TEMPRS * avs_state.freq_cnt *
		sizeof(avs_state.avs_v[0]), GFP_KERNEL);

	avs_state.default_vdd =  kmalloc(avs_state.freq_cnt * sizeof(avs_state.default_vdd[0]), GFP_KERNEL);
	avs_state.freq =  kmalloc(avs_state.freq_cnt * sizeof(avs_state.freq[0]), GFP_KERNEL);

	for(j=0;j<avs_state.freq_cnt; j++) {
		avs_state.default_vdd[j]=vdd_table[j];
		avs_state.freq[j]=freq_table[j];
	}

	if (avs_state.avs_v == 0)
		return -ENOMEM;

	avs_set_default_vdds();

	avs_state.set_vdd = set_vdd;
	avs_state.changing = 0;
	avs_state.freq_idx = -1;
	avs_state.vdd = -1;
	avs_state.current_tempr = 0;

	if(enabled)
		avs_enable(enabled);

	return 0;
}

void  avs_exit(void)
{
	avs_work_exit();

	kfree(avs_state.avs_v);
}


