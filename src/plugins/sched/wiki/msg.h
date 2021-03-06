/*****************************************************************************\
 *  msg.h - Message/communication manager for Wiki plugin
 *****************************************************************************
 *  Copyright (C) 2006 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

/*
 * Two modes of operation are currently supported for job prioritization:
 *
 * PRIO_HOLD: Wiki is a polling scheduler, so the initial priority is always
 * zero to keep SLURM from spontaneously starting the job.  The scheduler will
 * suggest which job's priority should be made non-zero and thus allowed to
 * proceed.
 *
 * PRIO_DECREMENT: Set the job priority to one less than the last job and let
 * Wiki change priorities of jobs as desired to re-order the queue
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "slurm/slurm_errno.h"

#include "src/common/bitstring.h"
#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/parse_config.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_interface.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"
#include "src/slurmctld/slurmctld.h"

/* Global configuration parameters */
#define E_HOST_SIZE  256
#define EXC_PART_CNT  32
#define HIDE_PART_CNT 32
#define KEY_SIZE      32
#define PRIO_HOLD      0
#define PRIO_DECREMENT 1
extern int	init_prio_mode;
extern char 	auth_key[KEY_SIZE];
extern char	e_host[E_HOST_SIZE];
extern char	e_host_bu[E_HOST_SIZE];
extern uint16_t	e_port;
extern struct	part_record *exclude_part_ptr[EXC_PART_CNT];
extern struct	part_record *hide_part_ptr[HIDE_PART_CNT];
extern struct	part_record *hide_part_nodes_ptr[HIDE_PART_CNT];
extern uint16_t	job_aggregation_time;
extern uint16_t kill_wait;
extern uint16_t use_host_exp;

extern char *   bitmap2wiki_node_name(bitstr_t *bitmap);
extern int	event_notify(char *msg);
extern int	spawn_msg_thread(void);
extern void	term_msg_thread(void);
extern char *   get_wiki_conf(void);

/*
 * Given a string, replace the first space found with '\0'
 */
extern void	null_term(char *str);

/* Functions called from within msg.c (rather than creating a bunch
 * more header files with one function definition each */
extern int	cancel_job(char *cmd_ptr, int *err_code, char **err_msg);
extern int	get_jobs(char *cmd_ptr, int *err_code, char **err_msg);
extern int 	get_nodes(char *cmd_ptr, int *err_code, char **err_msg);
extern int	initialize_wiki(char *cmd_ptr, int *err_code, char **err_msg);
extern int	job_add_task(char *cmd_ptr, int *err_code, char **err_msg);
extern int	job_modify_wiki(char *cmd_ptr, int *err_code, char **err_msg);
extern int	job_release_task(char *cmd_ptr, int *err_code, char **err_msg);
extern int	job_requeue_wiki(char *cmd_ptr, int *err_code, char **err_msg);
extern int	job_signal_wiki(char *cmd_ptr, int *err_code, char **err_msg);
extern int	job_will_run(char *cmd_ptr, int *err_code, char **err_msg);
extern char *	moab2slurm_task_list(char *moab_tasklist, int *task_cnt);
extern int	parse_wiki_config(void);
extern int	start_job(char *cmd_ptr, int *err_code, char **err_msg);
extern int	suspend_job(char *cmd_ptr, int *err_code, char **err_msg);
extern int	resume_job(char *cmd_ptr, int *err_code, char **err_msg);
