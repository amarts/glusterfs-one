/*
 * Please do not edit this file.
 * It was generated using rpcgen.
 */

#ifndef _GLUSTERD1_H_RPCGEN
#define _GLUSTERD1_H_RPCGEN

#include <rpc/rpc.h>


#ifdef __cplusplus
extern "C" {
#endif


enum glusterd_volume_status {
	GLUSTERD_STATUS_NONE = 0,
	GLUSTERD_STATUS_STARTED = 0 + 1,
	GLUSTERD_STATUS_STOPPED = 0 + 2,
};
typedef enum glusterd_volume_status glusterd_volume_status;

struct gd1_mgmt_probe_req {
	u_char uuid[16];
	char *hostname;
};
typedef struct gd1_mgmt_probe_req gd1_mgmt_probe_req;

struct gd1_mgmt_probe_rsp {
	u_char uuid[16];
	char *hostname;
};
typedef struct gd1_mgmt_probe_rsp gd1_mgmt_probe_rsp;

struct gd1_mgmt_friend_req {
	u_char uuid[16];
	char *hostname;
};
typedef struct gd1_mgmt_friend_req gd1_mgmt_friend_req;

struct gd1_mgmt_friend_rsp {
	u_char uuid[16];
	char *hostname;
	int op_ret;
	int op_errno;
};
typedef struct gd1_mgmt_friend_rsp gd1_mgmt_friend_rsp;

struct gd1_mgmt_unfriend_req {
	u_char uuid[16];
	char *hostname;
};
typedef struct gd1_mgmt_unfriend_req gd1_mgmt_unfriend_req;

struct gd1_mgmt_unfriend_rsp {
	u_char uuid[16];
	char *hostname;
	int op_ret;
	int op_errno;
};
typedef struct gd1_mgmt_unfriend_rsp gd1_mgmt_unfriend_rsp;

struct gd1_mgmt_cluster_lock_req {
	u_char uuid[16];
};
typedef struct gd1_mgmt_cluster_lock_req gd1_mgmt_cluster_lock_req;

struct gd1_mgmt_cluster_lock_rsp {
	u_char uuid[16];
	int op_ret;
	int op_errno;
};
typedef struct gd1_mgmt_cluster_lock_rsp gd1_mgmt_cluster_lock_rsp;

struct gd1_mgmt_cluster_unlock_req {
	u_char uuid[16];
};
typedef struct gd1_mgmt_cluster_unlock_req gd1_mgmt_cluster_unlock_req;

struct gd1_mgmt_cluster_unlock_rsp {
	u_char uuid[16];
	int op_ret;
	int op_errno;
};
typedef struct gd1_mgmt_cluster_unlock_rsp gd1_mgmt_cluster_unlock_rsp;

struct gd1_mgmt_stage_op_req {
	u_char uuid[16];
	int op;
	struct {
		u_int buf_len;
		char *buf_val;
	} buf;
};
typedef struct gd1_mgmt_stage_op_req gd1_mgmt_stage_op_req;

struct gd1_mgmt_stage_op_rsp {
	u_char uuid[16];
	int op;
	int op_ret;
	int op_errno;
};
typedef struct gd1_mgmt_stage_op_rsp gd1_mgmt_stage_op_rsp;

struct gd1_mgmt_commit_op_req {
	u_char uuid[16];
	int op;
	struct {
		u_int buf_len;
		char *buf_val;
	} buf;
};
typedef struct gd1_mgmt_commit_op_req gd1_mgmt_commit_op_req;

struct gd1_mgmt_commit_op_rsp {
	u_char uuid[16];
	int op;
	int op_ret;
	int op_errno;
};
typedef struct gd1_mgmt_commit_op_rsp gd1_mgmt_commit_op_rsp;

struct gd1_mgmt_friend_update {
	u_char uuid[16];
	u_char friend_uuid[16];
	char *hostname;
};
typedef struct gd1_mgmt_friend_update gd1_mgmt_friend_update;

/* the xdr functions */

#if defined(__STDC__) || defined(__cplusplus)
extern  bool_t xdr_glusterd_volume_status (XDR *, glusterd_volume_status*);
extern  bool_t xdr_gd1_mgmt_probe_req (XDR *, gd1_mgmt_probe_req*);
extern  bool_t xdr_gd1_mgmt_probe_rsp (XDR *, gd1_mgmt_probe_rsp*);
extern  bool_t xdr_gd1_mgmt_friend_req (XDR *, gd1_mgmt_friend_req*);
extern  bool_t xdr_gd1_mgmt_friend_rsp (XDR *, gd1_mgmt_friend_rsp*);
extern  bool_t xdr_gd1_mgmt_unfriend_req (XDR *, gd1_mgmt_unfriend_req*);
extern  bool_t xdr_gd1_mgmt_unfriend_rsp (XDR *, gd1_mgmt_unfriend_rsp*);
extern  bool_t xdr_gd1_mgmt_cluster_lock_req (XDR *, gd1_mgmt_cluster_lock_req*);
extern  bool_t xdr_gd1_mgmt_cluster_lock_rsp (XDR *, gd1_mgmt_cluster_lock_rsp*);
extern  bool_t xdr_gd1_mgmt_cluster_unlock_req (XDR *, gd1_mgmt_cluster_unlock_req*);
extern  bool_t xdr_gd1_mgmt_cluster_unlock_rsp (XDR *, gd1_mgmt_cluster_unlock_rsp*);
extern  bool_t xdr_gd1_mgmt_stage_op_req (XDR *, gd1_mgmt_stage_op_req*);
extern  bool_t xdr_gd1_mgmt_stage_op_rsp (XDR *, gd1_mgmt_stage_op_rsp*);
extern  bool_t xdr_gd1_mgmt_commit_op_req (XDR *, gd1_mgmt_commit_op_req*);
extern  bool_t xdr_gd1_mgmt_commit_op_rsp (XDR *, gd1_mgmt_commit_op_rsp*);
extern  bool_t xdr_gd1_mgmt_friend_update (XDR *, gd1_mgmt_friend_update*);

#else /* K&R C */
extern bool_t xdr_glusterd_volume_status ();
extern bool_t xdr_gd1_mgmt_probe_req ();
extern bool_t xdr_gd1_mgmt_probe_rsp ();
extern bool_t xdr_gd1_mgmt_friend_req ();
extern bool_t xdr_gd1_mgmt_friend_rsp ();
extern bool_t xdr_gd1_mgmt_unfriend_req ();
extern bool_t xdr_gd1_mgmt_unfriend_rsp ();
extern bool_t xdr_gd1_mgmt_cluster_lock_req ();
extern bool_t xdr_gd1_mgmt_cluster_lock_rsp ();
extern bool_t xdr_gd1_mgmt_cluster_unlock_req ();
extern bool_t xdr_gd1_mgmt_cluster_unlock_rsp ();
extern bool_t xdr_gd1_mgmt_stage_op_req ();
extern bool_t xdr_gd1_mgmt_stage_op_rsp ();
extern bool_t xdr_gd1_mgmt_commit_op_req ();
extern bool_t xdr_gd1_mgmt_commit_op_rsp ();
extern bool_t xdr_gd1_mgmt_friend_update ();

#endif /* K&R C */

#ifdef __cplusplus
}
#endif

#endif /* !_GLUSTERD1_H_RPCGEN */
