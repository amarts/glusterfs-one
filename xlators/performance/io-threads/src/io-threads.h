/*
   Copyright (c) 2006-2009 Z RESEARCH, Inc. <http://www.zresearch.com>
   This file is part of GlusterFS.

   GlusterFS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3 of the License,
   or (at your option) any later version.

   GlusterFS is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.
*/

#ifndef __IOT_H
#define __IOT_H

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif


#include "compat-errno.h"
#include "glusterfs.h"
#include "logging.h"
#include "dict.h"
#include "xlator.h"
#include "common-utils.h"
#include "list.h"

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

struct iot_conf;
struct iot_worker;
struct iot_request;

struct iot_request {
  struct list_head list;        /* Attaches this request to the list of
                                   requests.
                                   */
  call_stub_t *stub;
};

struct iot_worker {
  struct list_head rqlist;      /* List of requests assigned to me. */
  struct iot_conf *conf;
  int64_t q,dq;
  pthread_cond_t dq_cond;
  pthread_mutex_t qlock;
  int32_t queue_size;
  pthread_t thread;
};

struct iot_conf {
  int32_t thread_count;
  struct iot_worker ** workers;
};

typedef struct iot_conf iot_conf_t;
typedef struct iot_worker iot_worker_t;
typedef struct iot_request iot_request_t;

#endif /* __IOT_H */
