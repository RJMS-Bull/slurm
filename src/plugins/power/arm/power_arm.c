#if HAVE_CONFIG_H
#  include "config.h"
#endif

#define _GNU_SOURCE	/* For POLLRDHUP */
#include <ctype.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
//http_get function required libraries
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
//#include <stdlib.h>
#include <netdb.h>
#include <string.h>

#include "slurm/slurm.h"

#include "src/common/list.h"
#include "src/common/layouts_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/log.h"
#include "src/common/timers.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/common/hostlist.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/powercapping.h"
#include "src/plugins/power/common/power_common.h"

#define DEFAULT_BALANCE_INTERVAL  10
#define DEFAULT_CAP_WATTS         0
#define DEFAULT_DECREASE_RATE     40
#define DEFAULT_GET_TIMEOUT       5000
#define DEFAULT_INCREASE_RATE     20
#define DEFAULT_LOWER_THRESHOLD   90
#define DEFAULT_SET_TIMEOUT       30000
#define DEFAULT_UPPER_THRESHOLD   95
#define DEFAULT_RECENT_JOB        10  // 1*DEFAULT_BALANCE_INTERVAL
#define DEFAULT_RAPL_CONF_TIME    1800 // 1800 senconds

#define MILLI_WATT_TO_WATT	  0.001
#define USERAGENT		  "HTMLGET 1.0"
// Layouts(power/cpu_freq) keys
#define L_NAME          "power"
#define L_CLUSTER       "Cluster"
#define L_SUM_MAX       "MaxSumWatts"
#define L_SUM_IDLE      "IdleSumWatts"
#define L_SUM_CUR       "CurrentSumPower"
#define L_NODE_MAX      "MaxWatts"
#define L_NODE_IDLE     "IdleWatts"
#define L_NODE_DOWN     "DownWatts"
#define L_NODE_SAVE     "PowerSaveWatts"
#define L_NODE_CUR      "CurrentPower"
#define L_NUM_FREQ      "NumFreqChoices"
#define L_CUR_POWER     "CurrentCorePower"
#define L_SENSOR	"SensorId" // ARM - MB2 specific keys
#define L_PORT		"Port"
#define L_DNS		"DNS"

#if defined (__APPLE__)
struct node_record *node_record_table_ptr __attribute__((weak_import)) = NULL;
List job_list __attribute__((weak_import)) = NULL;
int node_record_count __attribute__((weak_import)) = 0;
bitstr_t *idle_node_bitmap __attribute__((weak_import));
bitstr_t *alloc_core_bitmap __attribute__((weak_import));
#else
struct node_record *node_record_table_ptr = NULL;
List job_list = NULL;
int node_record_count = 0;
bitstr_t *idle_node_bitmap;
bitstr_t *alloc_core_bitmap;
#endif

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "burst_buffer" for SLURM burst_buffer) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will only
 * load a burst_buffer plugin if the plugin_type string has a prefix of
 * "burst_buffer/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]        = "Power management plugin for ARM cluster";
const char plugin_type[]        = "power/arm";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

/******************* Local data within power/arm ***************/
typedef struct node_power {
	char *sensor_id;	// it is a pointer to layouts sensor id information
	uint32_t *cur_cpu_freq; // it is a pointer to layouts current cpufreq key
	uint32_t cur_power[3];	// It is current frequency's power consumption for 3 time 
	uint32_t current_power;	// It is current frequency's power consumption for 3 time 
	uint32_t *prev_cpu_freq;
	uint32_t prev_power[3];
} node_power_t;
node_power_t *np_details; // node power consumption details

/*********************** local variables *********************/
static int balance_interval = DEFAULT_BALANCE_INTERVAL;
static uint32_t cap_watts = DEFAULT_CAP_WATTS;
static uint32_t set_watts = 0;
static uint64_t debug_flag = 0;
static uint32_t decrease_rate = DEFAULT_DECREASE_RATE;
static uint32_t increase_rate = DEFAULT_INCREASE_RATE;
static uint32_t job_level = NO_VAL;
static time_t last_limits_read = 0;
static uint32_t lower_threshold = DEFAULT_LOWER_THRESHOLD;
static uint32_t recent_job = DEFAULT_RECENT_JOB;
static uint32_t upper_threshold = DEFAULT_UPPER_THRESHOLD;
static bool stop_power = false;  // used for stopping power_thread
static int get_timeout = DEFAULT_GET_TIMEOUT;
static int set_timeout = DEFAULT_SET_TIMEOUT;
static pthread_t power_thread = 0;
static pthread_mutex_t thread_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t term_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  term_cond = PTHREAD_COND_INITIALIZER;
static char *dns = NULL; // Default is localhost in MB2 cluster
static uint16_t *port = NULL;	// Default is 8080 in MB2 cluster

/*********************** local functios declarations *********************/
int _get_power(); //get power using http request
static void _load_config(void);
extern void *_power_agent(void *args);
static void _stop_power_agent(void);
static void _my_sleep(int add_secs);
static void _update_power_in_layout(int layout_type);
int _create_tcp_socket();
char *_dns_to_ip(char *host);
char *_build_http_query(char *host, char *sensor_id, int interval);

/*********************** local functions definition*********************/
/* Parse PowerParameters configuration */
static void _load_config(void)
{
        char *end_ptr = NULL, *sched_params, *tmp_ptr;

        debug_flag = slurm_get_debug_flags();
        sched_params = slurm_get_power_parameters();
        if (!sched_params)
                sched_params = xmalloc(1);      /* Set defaults below */

        if ((tmp_ptr = strstr(sched_params, "balance_interval="))) {
                balance_interval = atoi(tmp_ptr + 17);
                if (balance_interval < 1) {
                        error("PowerParameters: balance_interval=%d invalid",
                              balance_interval);
                        balance_interval = DEFAULT_BALANCE_INTERVAL;
                }
        } else {
                balance_interval = DEFAULT_BALANCE_INTERVAL;
        }

        if ((tmp_ptr = strstr(sched_params, "capmc_path="))) {
		fatal("power/arm plugin does not support capmc_path "
			"in PowerParameters[slurm.conf]");
        } 

        if ((tmp_ptr = strstr(sched_params, "cap_watts="))) {
                cap_watts = strtol(tmp_ptr + 10, &end_ptr, 10);
                if ((end_ptr[0] == 'k') || (end_ptr[0] == 'K')) {
                        cap_watts *= 1000;
                } else if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
                        cap_watts *= 1000000;
                }
        } else {
                cap_watts = DEFAULT_CAP_WATTS;
        }


        if ((tmp_ptr = strstr(sched_params, "decrease_rate="))) {
                decrease_rate = atoi(tmp_ptr + 14);
                if (decrease_rate < 1) {
                        error("PowerParameters: decrease_rate=%u invalid",
                              balance_interval);
                        lower_threshold = DEFAULT_DECREASE_RATE;
                }
        } else {
                decrease_rate = DEFAULT_DECREASE_RATE;
        }

        if ((tmp_ptr = strstr(sched_params, "increase_rate="))) {
                increase_rate = atoi(tmp_ptr + 14);
                if (increase_rate < 1) {
                        error("PowerParameters: increase_rate=%u invalid",
                              balance_interval);
                        lower_threshold = DEFAULT_INCREASE_RATE;
                }
        } else {
                increase_rate = DEFAULT_INCREASE_RATE;
        }

        if (strstr(sched_params, "job_level"))
                job_level = 1;
        else if (strstr(sched_params, "job_no_level"))
                job_level = 0;
        else
                job_level = NO_VAL;

        if ((tmp_ptr = strstr(sched_params, "get_timeout="))) {
                get_timeout = atoi(tmp_ptr + 12);
                if (get_timeout < 1) {
                        error("PowerParameters: get_timeout=%u invalid",
                              get_timeout);
                        get_timeout = DEFAULT_GET_TIMEOUT;
                }
        } else {
                get_timeout = DEFAULT_GET_TIMEOUT;
        }

        if ((tmp_ptr = strstr(sched_params, "lower_threshold="))) {
                lower_threshold = atoi(tmp_ptr + 16);
                if (lower_threshold < 1) {
                        error("PowerParameters: lower_threshold=%u invalid",
                              lower_threshold);
                        lower_threshold = DEFAULT_LOWER_THRESHOLD;
                }
        } else {
                lower_threshold = DEFAULT_LOWER_THRESHOLD;
        }

        if ((tmp_ptr = strstr(sched_params, "recent_job="))) {
                recent_job = atoi(tmp_ptr + 11);
                if (recent_job < 1) {
                        error("PowerParameters: recent_job=%u invalid",
                              recent_job);
                        recent_job = DEFAULT_RECENT_JOB;
                }
        } else {
                recent_job = DEFAULT_RECENT_JOB;
        }

        if ((tmp_ptr = strstr(sched_params, "set_timeout="))) {
                set_timeout = atoi(tmp_ptr + 12);
                if (set_timeout < 1) {
                        error("PowerParameters: set_timeout=%u invalid",
                              set_timeout);
                        set_timeout = DEFAULT_SET_TIMEOUT;
                }
        } else {
                set_timeout = DEFAULT_SET_TIMEOUT;
        }

        if ((tmp_ptr = strstr(sched_params, "set_watts="))) {
                set_watts = strtol(tmp_ptr + 10, &end_ptr, 10);
                if ((end_ptr[0] == 'k') || (end_ptr[0] == 'K')) {
                        set_watts *= 1000;
                } else if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
                        set_watts *= 1000000;
                }
        } else {
                set_watts = 0;
        }

        if ((tmp_ptr = strstr(sched_params, "upper_threshold="))) {
                upper_threshold = atoi(tmp_ptr + 16);
                if (upper_threshold < 1) {
                        error("PowerParameters: upper_threshold=%u invalid",
                              upper_threshold);
                        upper_threshold = DEFAULT_UPPER_THRESHOLD;
                }
        } else {
                upper_threshold = DEFAULT_UPPER_THRESHOLD;
        }

        xfree(sched_params);
        if (debug_flag & DEBUG_FLAG_POWER) {
                char *level_str = "";
                if (job_level == 0)
                        level_str = "job_no_level,";
                else if (job_level == 1)
                        level_str = "job_level,";
                info("PowerParameters=balance_interval=%d,"
                     "cap_watts=%u,decrease_rate=%u,get_timeout=%d,"
                     "increase_rate=%u,%slower_threshold=%u,recent_job=%u,"
                     "set_timeout=%d,set_watts=%u,upper_threshold=%u",
                     balance_interval, cap_watts, decrease_rate,
                     get_timeout, increase_rate, level_str, lower_threshold,
                     recent_job, set_timeout, set_watts, upper_threshold);
        }
	if (set_timeout > 0) {
		error("set_timeout is not relevant to power/arm plugin");
	}
        last_limits_read = 0;   /* Read node power limits again */
}

static void _my_sleep(int add_secs)
{
        struct timespec ts = {0, 0};
        struct timeval  tv = {0, 0};

        if (gettimeofday(&tv, NULL)) {          /* Some error */
                sleep(1);
                return;
        }

        ts.tv_sec  = tv.tv_sec + add_secs;
        ts.tv_nsec = tv.tv_usec * 1000;
        slurm_mutex_lock(&term_lock);
        if (!stop_power)
                pthread_cond_timedwait(&term_cond, &term_lock, &ts);
        slurm_mutex_unlock(&term_lock);
}

void _dump_power_details() 
{
	int i = 0, node_cont = 0;
	float cluster_power = 0.0f;
	struct node_record *node_ptr = NULL;
	node_ptr = node_record_table_ptr;
	power_mgmt_data_t *powr = NULL;
	for (i = 0; i < node_record_count; i++, node_ptr++) {
		if (!node_ptr || !node_ptr->power || !node_ptr->power->state)
			continue;

		powr = node_ptr->power;
		cluster_power += powr->current_watts;
		node_cont++;
		debug("=== Node:%s Detials from power/arm =>", node_ptr->name);
		debug("\tCurrent:%d W Status:%d", powr->current_watts,
			powr->state);
		debug("=====================================================>");
	}
	debug("%s cluster_power = %f cluster_cap = %u nb_nodes = %u",__func__,
			cluster_power, cap_watts, node_cont);
}

/* Periodically attempt to re-balance power caps across sockets, based on
 * power consumption feedback. This makes powercap intelligent and sensible */
extern void *_power_agent(void *args)
{
	time_t now;
	double wait_time;
	static time_t last_balance_time = 0;
	static int layout_type = 0;
	last_balance_time = time(NULL);

	info("started power/arm agent");
	while (!stop_power) {
		_my_sleep(1);
		if (stop_power)
			break;

		now = time(NULL);
		wait_time = difftime(now, last_balance_time);
		if (wait_time < balance_interval) 
			continue;

		_get_power(); 
		/*
 		if (set_watts)
			error("set watts option is not supported in "
				"power/arm-%s-%d",__func__,__LINE__);
		else if (cap_watts == 0)
			error("Disable powercap option is not supported in "
				"power/arm-%s-%d",__func__,__LINE__);
		else
			_rebalance_dvfs(); // rebalance DVFS value to meet powercap
		_set_dvfs();	// reset DVFS value to meet powercap
		*/
		layout_type =  which_power_layout();
		if (layout_type > 0) {
			_update_power_in_layout(layout_type);
		}
		_dump_power_details();
		last_balance_time = time(NULL);
	}
	return NULL;
}

/* Terminate power thread */
static void _stop_power_agent(void)
{
        slurm_mutex_lock(&term_lock);
        stop_power = true;
        pthread_cond_signal(&term_cond);
        slurm_mutex_unlock(&term_lock);
}

void _load_layouts()
{
	// get clusters DNS and Port and nodes' sensor id details
	int i;
	static uint16_t default_port = 8080;
	struct node_record *node_ptr = node_record_table_ptr;
	node_power_t *np = np_details;
	layouts_entity_get_kv_ref(L_NAME, L_CLUSTER, L_DNS, 
			(void **)&dns, L_T_STRING);
	layouts_entity_get_kv_ref(L_NAME, L_CLUSTER, L_PORT, 
			(void **)&port, L_T_UINT16);
	if (!dns) {
		info("%s %d dns is not perfect value",
			__func__, __LINE__);
		dns = "localhost";
	}
	if (!port) {
		info("%s %d port is not perfect value",
			__func__, __LINE__);
		port = &default_port;
	}
	info("%s %d cluster_ip: %s cluster_port: %d", __func__, __LINE__,
		dns, *port);
	for (i = 0; i < node_record_count && np && node_ptr;
			i++, node_ptr++, np++) {
		if (!np || !node_ptr)
			continue;
		layouts_entity_get_kv_ref(L_NAME, node_ptr->name, L_SENSOR,
				(void **)(&np->sensor_id), L_T_STRING);
		info("%s %d node:%s sensor:%s", __func__, __LINE__,
			node_ptr->name, np->sensor_id);
	}	
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	pthread_attr_t attr;
	int i;
	struct node_record *node_ptr = node_record_table_ptr;
	if (!run_in_daemon("slurmctld"))
		return SLURM_SUCCESS;

	np_details = (node_power_t *)xmalloc(node_record_count * sizeof(node_power_t));
	/* Write nodes */
	slurmctld_lock_t node_lock = { NO_LOCK, NO_LOCK, WRITE_LOCK, NO_LOCK };
	lock_slurmctld (node_lock);
	for (i = 0; i < node_record_count && node_ptr; i++, node_ptr++) {
		if (node_record_table_ptr[i].power)
			continue;

		node_ptr->power = xmalloc(sizeof(power_mgmt_data_t));
	}
	unlock_slurmctld(node_lock);
	slurm_mutex_lock(&thread_flag_mutex);
	if (power_thread) {
		debug2("Power thread already running, not starting another");
		slurm_mutex_unlock(&thread_flag_mutex);
		return SLURM_ERROR;
	}
	_load_config();
	_load_layouts(); // load layout sensor_id info to the local np_details structure
	slurm_attr_init(&attr);
	/* Since we do a join on thread later, don't make it detached */
	if (pthread_create(&power_thread, &attr, _power_agent, NULL))
		error("Unable to start power thread: %m");
	slurm_attr_destroy(&attr);
	slurm_mutex_unlock(&thread_flag_mutex);
	debug("%s: %s", plugin_name, __func__);
	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is unloaded. Free all memory.
 */
extern void fini(void)
{
	/* free arm details from nodes */
	int i;
	slurmctld_lock_t node_lock = { NO_LOCK, NO_LOCK, WRITE_LOCK, NO_LOCK };
	slurm_mutex_lock(&thread_flag_mutex);
	if (power_thread) {
		_stop_power_agent();
		pthread_join(power_thread, NULL);
		power_thread = 0;
	}
	slurm_mutex_unlock(&thread_flag_mutex);
	lock_slurmctld (node_lock);
	for (i = 0; i < node_record_count; i++) {
		if (!(node_record_table_ptr + i) || 
			!(node_record_table_ptr[i].power)) 
			continue;

		xfree(node_record_table_ptr[i].power);
	}
	unlock_slurmctld(node_lock);
	if (np_details)
		xfree(np_details);
	debug("%s: %s", plugin_name, __func__);
	return;
}

/* Read the configuration file */
extern void power_p_reconfig(void)
{
	slurm_mutex_lock(&thread_flag_mutex);
	_load_config();
	slurm_mutex_unlock(&thread_flag_mutex);
	debug("%s: %s reconfiguration in powercap", plugin_name, __func__);
	return;
}

/* Note that a suspended job has been resumed */
extern void power_p_job_resume(struct job_record *job_ptr)
{
        set_node_new_job(job_ptr, node_record_table_ptr);
}


/* Note that a job has been allocated resources and is ready to start */
extern void power_p_job_start(struct job_record *job_ptr)
{
        set_node_new_job(job_ptr, node_record_table_ptr);
}

int _get_node_power(struct node_record *node_ptr, int n)
{
	struct sockaddr_in *remote;
	int sock, tmpres;
	char *ip, *get, *sensor; // sennsor is the URL for getting power value
	char buf[BUFSIZ + 1];

	sensor = np_details[n].sensor_id;
	sock = _create_tcp_socket();
	ip = _dns_to_ip(dns);
	debug2("%s-%d IP is %s\n", __func__, __LINE__, ip);
	remote = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in *));
	remote->sin_family = AF_INET;
	tmpres = inet_pton(AF_INET, ip, (void *)(&(remote->sin_addr.s_addr)));
	if (tmpres < 0) {
		error("%s-%d Can't set remote->sin_addr.s_addr", __func__, __LINE__);
		exit(1);
	} else if (tmpres == 0) {
		error("%s-%d %s is not a valid IP address\n", __func__, __LINE__, ip);
		exit(1);
	}
	remote->sin_port = htons(*port);
	if (connect(sock, (struct sockaddr *)remote, sizeof(struct sockaddr)) < 0) {
		error("%s-%d Could not connect", __func__, __LINE__);
		exit(1);
	}
	get = _build_http_query(dns, sensor, balance_interval); // build http get querry
	debug3("Query is:\n\t<<START>>\n\t%s\n\t<<END>>\n", get);
	int sent = 0;
	while (sent < strlen(get)) {
		tmpres = send(sock, get + sent, strlen(get) - sent, 0);
		if (tmpres == -1) {
			error("%s-%d Can't send query", __func__, __LINE__);
			exit(1);
		}
		sent += tmpres;
	}
	memset(buf, 0, sizeof(buf));
	int htmlstart = 0;
	char *htmlcontent;
	while ((tmpres = recv(sock, buf, BUFSIZ, 0)) > 0) {
		if (htmlstart == 0) {
			htmlcontent = strstr(buf, "\r\n\r\n");
			if (htmlcontent != NULL) {
				htmlstart = 1;
				htmlcontent += 4;
			}
		} else {
			htmlcontent = buf;
		}
		if (htmlstart) {
			info("%s %d node:%s power:%s MW", __func__, __LINE__,
					node_ptr->name, htmlcontent);
			node_ptr->power->current_watts = atoi(htmlcontent);
			if (node_ptr->power->current_watts > 0) {
				node_ptr->power->current_watts = (uint32_t)ceil(
					node_ptr->power->current_watts * MILLI_WATT_TO_WATT);
				node_ptr->power->state = 1;
			} else {
				node_ptr->power->current_watts = 0;
				node_ptr->power->state = 0;
			}
			node_ptr->power->cap_watts = node_ptr->power->current_watts;
		}
		memset(buf, 0, tmpres);
	}
	if(tmpres < 0) {
		error("%s-%d Error receiving power data from http req", __func__, __LINE__);
		node_ptr->power->current_watts = 0;
		node_ptr->power->state = 0;
	}
	free(get);
	free(remote);
	free(ip);
	close(sock);
	return SLURM_SUCCESS;
}

int _get_power()
{
	struct node_record *node_ptr = node_record_table_ptr;
	int i;
	for (i = 0; i < node_record_count && node_ptr; i++, node_ptr++) {
		if (!node_ptr || IS_NODE_UNKNOWN(node_ptr) ||
			IS_NODE_DOWN(node_ptr) || IS_NODE_ERROR(node_ptr) ||
			IS_NODE_FUTURE(node_ptr) || !node_ptr->power)
			continue;
		if (!np_details[i].sensor_id) {
			info("node: %s does not have sensor id info", node_ptr->name);
			continue;
		}

		_get_node_power(node_ptr, i);
	}
	return SLURM_SUCCESS;
}

void _update_power_in_layout_core(struct node_record *node_ptr, int node_i)
{
	int i, j = 0, core_begin = 0, core_end = 0;
	uint32_t idle_cores_power = 0, nb_idle_core = 0;
	uint32_t cores_in_node = (node_ptr->sockets * node_ptr->cores);
	uint32_t core_power = 0, core_power_precision = 0, node_power = 0;
	char ename[125];

	node_power = node_ptr->power->current_watts;
	core_begin = cr_get_coremap_offset(node_i);
	core_end = cr_get_coremap_offset(node_i + 1);
	for (i = core_begin; i < core_end; i++) {
		if (bit_test(alloc_core_bitmap, i))
			continue;

		sprintf(ename, "virtualcore%u", i);
		layouts_entity_get_kv(L_NAME,
		    ename, "IdleCoreWatts", &core_power, L_T_UINT32);
		idle_cores_power += core_power;
		nb_idle_core++;
	}
	node_power -= idle_cores_power;
	cores_in_node -= nb_idle_core;
	if (node_power < 0)
		return;
	core_power = (uint32_t)(node_power / cores_in_node);
	core_power_precision = (node_power -
				(core_power * cores_in_node));
	core_power_precision += core_power; // node_power will be ceil() perfectly
	for (i = core_begin; i < core_end; i++) {
		if (!bit_test(alloc_core_bitmap, i))
			continue;

		sprintf(ename, "virtualcore%u", i);
		if (j == 0) {
			layouts_entity_set_kv("power",
		    ename, "CurrentCorePower", &core_power_precision, L_T_UINT32);
			j = 1;
		} else {
			layouts_entity_set_kv("power",
		    ename, "CurrentCorePower", &core_power, L_T_UINT32);
		}
	}
}

void _update_power_in_layout_node(node_ptr)
{
	error("Not supported in Prototype");
}
/* This is the wrapper function to update powercap values in layouts 
 * (power/cpufreq or power/default) to synchronize power allocation   
 */
static void  _update_power_in_layout(int layout_type)
{
	struct node_record *node_ptr = node_record_table_ptr;
	int i;

	for (i = 0; i < node_record_count; i++, node_ptr++) {
		if (!node_ptr || IS_NODE_UNKNOWN(node_ptr) ||
			IS_NODE_DOWN(node_ptr) || IS_NODE_IDLE(node_ptr) ||
			IS_NODE_ERROR(node_ptr) || IS_NODE_FUTURE(node_ptr) ||
			!node_ptr->power || !node_ptr->power->state)
			continue;

		if (layout_type == 2) {
			_update_power_in_layout_core(node_ptr, i);
		} else if (layout_type == 1) {
			_update_power_in_layout_node(node_ptr);
		} else {
			error("%s %d Not possible to reach here",
					__func__, __LINE__);
			return;
		}
	}
	layouts_entity_pull_kv("power", "Cluster", "CurrentSumPower");
}

int _create_tcp_socket()
{
	int sock;
	if((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		perror("Can't create TCP socket");
		exit(1);
	}
	return sock;
}


char *_dns_to_ip(char *host)
{
	struct hostent *hent;
	int iplen = 15; //XXX.XXX.XXX.XXX
	char *ip = (char *)malloc(iplen + 1);
	memset(ip, 0, (iplen + 1));
	if ((hent = gethostbyname(host)) == NULL) {
		error("Can't get IP from cluster DNS");
		exit(1);
	}
	if (inet_ntop(AF_INET, (void *)hent->h_addr_list[0], ip, iplen)
		== NULL) {
		perror("Can't resolve host");
		exit(1);
	}
	return ip;
}

/* It is a http get querry building function.
 */
char *_build_http_query(char *host, char *sensor_id, int interval)
{
	char *query;
	char *getpage = sensor_id;
	char *tpl = "GET /%s?avg=%d HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\n\r\n";
	if (getpage[0] == '/') {
		getpage = getpage + 1;
		error("Removing leading \"/\" in sensor_id, converting %s to %s\n",
				sensor_id, getpage);
	}
	// -5 is to consider the %s %s %s in tpl and the ending \0
	query = (char *)malloc(strlen(host) + strlen(getpage) +
				strlen(USERAGENT) + strlen(tpl) - 5);
	sprintf(query, tpl, getpage, interval, host, USERAGENT);
	return query;
}
