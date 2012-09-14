/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/


#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "glusterfs.h"
#include "logging.h"
#include "dict.h"
#include "xlator.h"
#include "list.h"
#include "compat.h"
#include "compat-errno.h"
#include "common-utils.h"
#include "call-stub.h"
#include "statedump.h"
#include "defaults.h"
#include "write-behind-mem-types.h"

#define MAX_VECTOR_COUNT          8
#define WB_AGGREGATE_SIZE         131072 /* 128 KB */
#define WB_WINDOW_SIZE            1048576 /* 1MB */

typedef struct list_head list_head_t;
struct wb_conf;
struct wb_inode;

typedef struct wb_inode {
        ssize_t      window_conf;
        ssize_t      window_current;
	ssize_t      transit; /* size of data stack_wound, and yet
				 to be fulfilled (wb_fulfill_cbk).
				 used for trickling_writes
			      */

        int32_t      op_ret;   /* Last found op_ret and op_errno
				  while completing a liability
				  operation. Will be picked by
				  the next arriving writev/flush/fsync
			       */
        int32_t      op_errno;

        list_head_t  all;    /* All requests, from enqueue() till destroy().
				Used only for resetting generation
				number when empty.
			     */
        list_head_t  todo;   /* Work to do (i.e, STACK_WIND to server).
				Once we STACK_WIND, the entry is taken
				off the list. If it is non-sync write,
				then we continue to track it via @liability
				or @temptation depending on the status
				of its writeback.
			     */
        list_head_t  liability;   /* Non-sync writes which are lied
				     (STACK_UNWIND'ed to caller) but ack
				     from server not yet complete. This
				     is the "liability" which we hold, and
				     must guarantee that dependent operations
				     which arrive later (which overlap, etc.)
				     are issued only after their dependencies
				     in this list are "fulfilled".

				     Server acks for entries in this list
				     shrinks the window.

				     The sum total of all req->write_size
				     of entries in this list must be kept less
				     than the permitted window size.
				  */
        list_head_t  temptation;  /* Operations for which we are tempted
				     to 'lie' (write-behind), but temporarily
				     holding off (because of insufficient
				     window capacity, etc.)

				     This is the list to look at to grow
				     the window (in __wb_pick_unwinds()).

				     Entries typically get chosen from
				     write-behind from this list, and therefore
				     get "upgraded" to the "liability" list.
			     */
	uint64_t     gen;    /* Liability generation number. Represents
				the current 'state' of liability. Every
				new addition to the liability list bumps
				the generation number.

				a newly arrived request is only required
				to perform causal checks against the entries
				in the liability list which were present
				at the time of its addition. the generation
				number at the time of its addition is stored
				in the request and used during checks.

				the liability list can grow while the request
				waits in the todo list waiting for its
				dependent operations to complete. however
				it is not of the request's concern to depend
				itself on those new entries which arrived
				after it arrived (i.e, those that have a
				liability generation higher than itself)
			     */
        gf_lock_t    lock;
        xlator_t    *this;
} wb_inode_t;


typedef struct wb_request {
        list_head_t           all;
        list_head_t           todo;
	list_head_t           lie;  /* either in @liability or @temptation */
        list_head_t           winds;
        list_head_t           unwinds;

        call_stub_t          *stub;

        size_t                write_size;  /* currently held size
					      (after collapsing) */
	size_t                orig_size;   /* size which arrived with the request.
					      This is the size by which we grow
					      the window when unwinding the frame.
					   */
        size_t                total_size;  /* valid only in @head in wb_fulfill().
					      This is the size with which we perform
					      STACK_WIND to server and therefore the
					      amount by which we shrink the window.
					   */

	int                   op_ret;
	int                   op_errno;

        int32_t               refcount;
        wb_inode_t           *wb_inode;
        glusterfs_fop_t       fop;
        gf_lkowner_t          lk_owner;
	struct iobref        *iobref;
	uint64_t              gen;  /* inode liability state at the time of
				       request arrival */

	fd_t                 *fd;
	struct {
		size_t        size;          /* 0 size == till infinity */
		off_t         off;
		int           append:1;      /* offset is invalid. only one
						outstanding append at a time */
		int           tempted:1;     /* true only for non-sync writes */
		int           lied:1;        /* sin committed */
		int           fulfilled:1;   /* got server acknowledgement */
		int           go:1;          /* enough aggregating, good to go */
	} ordering;
} wb_request_t;


typedef struct wb_conf {
        uint64_t         aggregate_size;
        uint64_t         window_size;
        gf_boolean_t     flush_behind;
        gf_boolean_t     trickling_writes;
	gf_boolean_t     strict_write_ordering;
	gf_boolean_t     strict_O_DIRECT;
} wb_conf_t;


void
wb_process_queue (wb_inode_t *wb_inode);


wb_inode_t *
__wb_inode_ctx_get (xlator_t *this, inode_t *inode)
{
        uint64_t    value    = 0;
        wb_inode_t *wb_inode = NULL;

        __inode_ctx_get (inode, this, &value);
        wb_inode = (wb_inode_t *)(unsigned long) value;

        return wb_inode;
}


wb_inode_t *
wb_inode_ctx_get (xlator_t *this, inode_t *inode)
{
        wb_inode_t *wb_inode = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);
        GF_VALIDATE_OR_GOTO (this->name, inode, out);

        LOCK (&inode->lock);
        {
                wb_inode = __wb_inode_ctx_get (this, inode);
        }
        UNLOCK (&inode->lock);
out:
        return wb_inode;
}


/*
  Below is a succinct explanation of the code deciding whether two regions
  overlap, from Pavan <tcp@gluster.com>.

  For any two ranges to be non-overlapping, either the end of the first
  range is lesser than the start of the second, or vice versa. Example -

  <--------->       <-------------->
  p         q       x              y

  ( q < x ) or (y < p) = > No overlap.

  To check for *overlap*, we can negate this (using de morgan's laws), and
  it becomes -

  (q >= x ) and (y >= p)

  Either that, or you write the negation using -

  if (! ((q < x) or (y < p)) ) {
  "Overlap"
  }
*/

gf_boolean_t
wb_requests_overlap (wb_request_t *req1, wb_request_t *req2)
{
        off_t            r1_start = 0;
	off_t            r1_end = 0;
	off_t            r2_start = 0;
	off_t            r2_end = 0;
        enum _gf_boolean do_overlap = 0;

        r1_start = req1->ordering.off;
	if (req1->ordering.size)
		r1_end = r1_start + req1->ordering.size - 1;
	else
		r1_end = ULLONG_MAX;

        r2_start = req2->ordering.off;
	if (req2->ordering.size)
		r2_end = r2_start + req2->ordering.size - 1;
	else
		r2_end = ULLONG_MAX;

        do_overlap = ((r1_end >= r2_start) && (r2_end >= r1_start));

        return do_overlap;
}


gf_boolean_t
wb_requests_conflict (wb_request_t *lie, wb_request_t *req)
{
	wb_conf_t  *conf = NULL;

	conf = req->wb_inode->this->private;

	if (lie == req)
		/* request cannot conflict with itself */
		return _gf_false;

	if (lie->gen >= req->gen)
		/* this liability entry was behind
		   us in the todo list */
		return _gf_false;

	if (lie->ordering.append)
		/* all modifications wait for the completion
		   of outstanding append */
		return _gf_true;

	if (conf->strict_write_ordering)
		/* We are sure (lie->gen < req->gen) by now. So
		   skip overlap check if strict write ordering is
		   requested and always return "conflict" against a
		   lower generation lie. */
		return _gf_true;

	return wb_requests_overlap (lie, req);
}


gf_boolean_t
wb_liability_has_conflict (wb_inode_t *wb_inode, wb_request_t *req)
{
        wb_request_t *each     = NULL;

        list_for_each_entry (each, &wb_inode->liability, lie) {
		if (wb_requests_conflict (each, req))
			return _gf_true;
        }

        return _gf_false;
}


static int
__wb_request_unref (wb_request_t *req)
{
        int         ret = -1;
	wb_inode_t *wb_inode = NULL;

	wb_inode = req->wb_inode;

        if (req->refcount <= 0) {
                gf_log ("wb-request", GF_LOG_WARNING,
                        "refcount(%d) is <= 0", req->refcount);
                goto out;
        }

        ret = --req->refcount;
        if (req->refcount == 0) {
                list_del_init (&req->todo);
                list_del_init (&req->lie);

		list_del_init (&req->all);
		if (list_empty (&wb_inode->all)) {
			wb_inode->gen = 0;
			/* in case of accounting errors? */
			wb_inode->window_current = 0;
		}

		list_del_init (&req->winds);
		list_del_init (&req->unwinds);

                if (req->stub && req->ordering.tempted) {
                        call_stub_destroy (req->stub);
			req->stub = NULL;
                } /* else we would have call_resume()'ed */

		if (req->iobref)
			iobref_unref (req->iobref);

		if (req->fd)
			fd_unref (req->fd);

                GF_FREE (req);
        }
out:
        return ret;
}


static int
wb_request_unref (wb_request_t *req)
{
        wb_inode_t *wb_inode = NULL;
        int         ret      = -1;

        GF_VALIDATE_OR_GOTO ("write-behind", req, out);

        wb_inode = req->wb_inode;

        LOCK (&wb_inode->lock);
        {
                ret = __wb_request_unref (req);
        }
        UNLOCK (&wb_inode->lock);

out:
        return ret;
}


static wb_request_t *
__wb_request_ref (wb_request_t *req)
{
        GF_VALIDATE_OR_GOTO ("write-behind", req, out);

        if (req->refcount < 0) {
                gf_log ("wb-request", GF_LOG_WARNING,
                        "refcount(%d) is < 0", req->refcount);
                req = NULL;
                goto out;
        }

        req->refcount++;

out:
        return req;
}


wb_request_t *
wb_request_ref (wb_request_t *req)
{
        wb_inode_t *wb_inode = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", req, out);

        wb_inode = req->wb_inode;
        LOCK (&wb_inode->lock);
        {
                req = __wb_request_ref (req);
        }
        UNLOCK (&wb_inode->lock);

out:
        return req;
}


gf_boolean_t
wb_enqueue_common (wb_inode_t *wb_inode, call_stub_t *stub, int tempted)
{
        wb_request_t *req     = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", wb_inode, out);
        GF_VALIDATE_OR_GOTO (wb_inode->this->name, stub, out);

        req = GF_CALLOC (1, sizeof (*req), gf_wb_mt_wb_request_t);
        if (!req)
                goto out;

        INIT_LIST_HEAD (&req->all);
        INIT_LIST_HEAD (&req->todo);
        INIT_LIST_HEAD (&req->lie);
        INIT_LIST_HEAD (&req->winds);
        INIT_LIST_HEAD (&req->unwinds);

        req->stub = stub;
        req->wb_inode = wb_inode;
        req->fop  = stub->fop;
	req->ordering.tempted = tempted;

        if (stub->fop == GF_FOP_WRITE) {
                req->write_size = iov_length (stub->args.writev.vector,
					      stub->args.writev.count);

		/* req->write_size can change as we collapse
		   small writes. But the window needs to grow
		   only by how much we acknowledge the app. so
		   copy the original size in orig_size for the
		   purpose of accounting.
		*/
		req->orig_size = req->write_size;

		/* Let's be optimistic that we can
		   lie about it
		*/
		req->op_ret = req->write_size;
		req->op_errno = 0;

		if (stub->args.writev.fd->flags & O_APPEND)
			req->ordering.append = 1;
        }

        req->lk_owner = stub->frame->root->lk_owner;

	switch (stub->fop) {
	case GF_FOP_WRITE:
		req->ordering.off = stub->args.writev.off;
		req->ordering.size = req->write_size;

		req->fd = fd_ref (stub->args.writev.fd);

		break;
	case GF_FOP_READ:
		req->ordering.off = stub->args.readv.off;
		req->ordering.size = stub->args.readv.size;

		req->fd = fd_ref (stub->args.readv.fd);

		break;
	case GF_FOP_TRUNCATE:
		req->ordering.off = stub->args.truncate.off;
		req->ordering.size = 0; /* till infinity */
		break;
	case GF_FOP_FTRUNCATE:
		req->ordering.off = stub->args.ftruncate.off;
		req->ordering.size = 0; /* till infinity */

		req->fd = fd_ref (stub->args.ftruncate.fd);

		break;
	default:
		break;
	}

        LOCK (&wb_inode->lock);
        {
                list_add_tail (&req->all, &wb_inode->all);

		req->gen = wb_inode->gen;

                list_add_tail (&req->todo, &wb_inode->todo);
		__wb_request_ref (req); /* for wind */

		if (req->ordering.tempted) {
			list_add_tail (&req->lie, &wb_inode->temptation);
			__wb_request_ref (req); /* for unwind */
		}
        }
        UNLOCK (&wb_inode->lock);

out:
	if (!req)
		return _gf_false;

	return _gf_true;
}


gf_boolean_t
wb_enqueue (wb_inode_t *wb_inode, call_stub_t *stub)
{
	return wb_enqueue_common (wb_inode, stub, 0);
}


gf_boolean_t
wb_enqueue_tempted (wb_inode_t *wb_inode, call_stub_t *stub)
{
	return wb_enqueue_common (wb_inode, stub, 1);
}


wb_inode_t *
__wb_inode_create (xlator_t *this, inode_t *inode)
{
        wb_inode_t *wb_inode = NULL;
        wb_conf_t  *conf     = NULL;

        GF_VALIDATE_OR_GOTO (this->name, inode, out);

        conf = this->private;

        wb_inode = GF_CALLOC (1, sizeof (*wb_inode), gf_wb_mt_wb_inode_t);
        if (!wb_inode)
                goto out;

        INIT_LIST_HEAD (&wb_inode->all);
        INIT_LIST_HEAD (&wb_inode->todo);
        INIT_LIST_HEAD (&wb_inode->liability);
        INIT_LIST_HEAD (&wb_inode->temptation);

        wb_inode->this = this;

        wb_inode->window_conf = conf->window_size;

        LOCK_INIT (&wb_inode->lock);

        __inode_ctx_put (inode, this, (uint64_t)(unsigned long)wb_inode);

out:
        return wb_inode;
}


wb_inode_t *
wb_inode_create (xlator_t *this, inode_t *inode)
{
        wb_inode_t *wb_inode = NULL;

        GF_VALIDATE_OR_GOTO (this->name, inode, out);

        LOCK (&inode->lock);
        {
                wb_inode = __wb_inode_ctx_get (this, inode);
                if (!wb_inode)
                        wb_inode = __wb_inode_create (this, inode);
        }
        UNLOCK (&inode->lock);

out:
        return wb_inode;
}


void
wb_inode_destroy (wb_inode_t *wb_inode)
{
        GF_VALIDATE_OR_GOTO ("write-behind", wb_inode, out);

        LOCK_DESTROY (&wb_inode->lock);
        GF_FREE (wb_inode);
out:
        return;
}


void
__wb_fulfill_request (wb_request_t *req)
{
	wb_inode_t *wb_inode = NULL;

	wb_inode = req->wb_inode;

	req->ordering.fulfilled = 1;
	wb_inode->window_current -= req->total_size;
	wb_inode->transit -= req->total_size;

	if (!req->ordering.lied) {
		/* TODO: fail the req->frame with error if
		   necessary
		*/
	}

	__wb_request_unref (req);
}


void
wb_head_done (wb_request_t *head)
{
	wb_request_t *req = NULL;
	wb_request_t *tmp     = NULL;
	wb_inode_t   *wb_inode = NULL;

	wb_inode = head->wb_inode;

	LOCK (&wb_inode->lock);
	{
		list_for_each_entry_safe (req, tmp, &head->winds, winds) {
			__wb_fulfill_request (req);
		}
		__wb_fulfill_request (head);
	}
	UNLOCK (&wb_inode->lock);
}


void
wb_inode_err (wb_inode_t *wb_inode, int op_errno)
{
	LOCK (&wb_inode->lock);
	{
		wb_inode->op_ret = -1;
		wb_inode->op_errno = op_errno;
	}
	UNLOCK (&wb_inode->lock);
}


int
wb_fulfill_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
		int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
		struct iatt *postbuf, dict_t *xdata)
{
        wb_inode_t   *wb_inode   = NULL;
        wb_request_t *head       = NULL;

	head = frame->local;
	frame->local = NULL;

        wb_inode = head->wb_inode;

	if (op_ret == -1) {
		wb_inode_err (wb_inode, op_errno);
	} else if (op_ret < head->total_size) {
		/*
		 * We've encountered a short write, for whatever reason.
		 * Set an EIO error for the next fop. This should be
		 * valid for writev or flush (close).
		 *
		 * TODO: Retry the write so we can potentially capture
		 * a real error condition (i.e., ENOSPC).
		 */
		wb_inode_err (wb_inode, EIO);
	}

	wb_head_done (head);

        wb_process_queue (wb_inode);

        STACK_DESTROY (frame->root);

        return 0;
}


#define WB_IOV_LOAD(vec, cnt, req, head) do {				\
		memcpy (&vec[cnt], req->stub->args.writev.vector,	\
			(req->stub->args.writev.count * sizeof(vec[0]))); \
		cnt += req->stub->args.writev.count;			\
		head->total_size += req->write_size;			\
	} while (0)


void
wb_fulfill_head (wb_inode_t *wb_inode, wb_request_t *head)
{
	struct iovec   vector[MAX_VECTOR_COUNT];
	int            count = 0;
	wb_request_t  *req = NULL;
	call_frame_t  *frame = NULL;

	frame = create_frame (wb_inode->this, wb_inode->this->ctx->pool);
	if (!frame)
		goto enomem;

	WB_IOV_LOAD (vector, count, head, head);

	list_for_each_entry (req, &head->winds, winds) {
		WB_IOV_LOAD (vector, count, req, head);

		iobref_merge (head->stub->args.writev.iobref,
			      req->stub->args.writev.iobref);
	}

	frame->root->lk_owner = head->lk_owner;
	frame->local = head;

	LOCK (&wb_inode->lock);
	{
		wb_inode->transit += head->total_size;
	}
	UNLOCK (&wb_inode->lock);

	STACK_WIND (frame, wb_fulfill_cbk, FIRST_CHILD (frame->this),
		    FIRST_CHILD (frame->this)->fops->writev,
		    head->fd, vector, count,
		    head->stub->args.writev.off,
		    head->stub->args.writev.flags,
		    head->stub->args.writev.iobref, NULL);

	return;
enomem:
	wb_inode_err (wb_inode, ENOMEM);

	wb_head_done (head);

	return;
}


#define NEXT_HEAD(head, req) do {					\
		if (head)						\
			wb_fulfill_head (wb_inode, head);		\
		head = req;						\
		expected_offset = req->stub->args.writev.off +		\
			req->write_size;				\
		curr_aggregate = 0;					\
		vector_count = 0;					\
	} while (0)


void
wb_fulfill (wb_inode_t *wb_inode, list_head_t *liabilities)
{
	wb_request_t  *req     = NULL;
	wb_request_t  *head    = NULL;
	wb_request_t  *tmp     = NULL;
	wb_conf_t     *conf    = NULL;
	off_t          expected_offset = 0;
	size_t         curr_aggregate = 0;
	size_t         vector_count = 0;

	conf = wb_inode->this->private;

	list_for_each_entry_safe (req, tmp, liabilities, winds) {
		list_del_init (&req->winds);

		if (!head) {
			NEXT_HEAD (head, req);
			continue;
		}

		if (req->fd != head->fd) {
			NEXT_HEAD (head, req);
			continue;
		}

		if (!is_same_lkowner (&req->lk_owner, &head->lk_owner)) {
			NEXT_HEAD (head, req);
			continue;
		}

		if (expected_offset != req->stub->args.writev.off) {
			NEXT_HEAD (head, req);
			continue;
		}

		if ((curr_aggregate + req->write_size) > conf->aggregate_size) {
			NEXT_HEAD (head, req);
			continue;
		}

		if (vector_count + req->stub->args.writev.count >
		    MAX_VECTOR_COUNT) {
			NEXT_HEAD (head, req);
			continue;
		}

		list_add_tail (&req->winds, &head->winds);
		curr_aggregate += req->write_size;
		vector_count += req->stub->args.writev.count;
	}

	if (head)
		wb_fulfill_head (wb_inode, head);
	return;
}


void
wb_do_unwinds (wb_inode_t *wb_inode, list_head_t *lies)
{
	wb_request_t *req = NULL;
	wb_request_t *tmp = NULL;
	call_frame_t *frame = NULL;
	struct iatt   buf = {0, };

        list_for_each_entry_safe (req, tmp, lies, unwinds) {
                frame = req->stub->frame;

                STACK_UNWIND_STRICT (writev, frame, req->op_ret, req->op_errno,
				     &buf, &buf, NULL); /* :O */
		req->stub->frame = NULL;

		list_del_init (&req->unwinds);
                wb_request_unref (req);
        }

        return;
}


void
__wb_pick_unwinds (wb_inode_t *wb_inode, list_head_t *lies)
{
        wb_request_t *req = NULL;
        wb_request_t *tmp = NULL;

	list_for_each_entry_safe (req, tmp, &wb_inode->temptation, lie) {
		if (!req->ordering.fulfilled &&
		    wb_inode->window_current > wb_inode->window_conf)
			continue;

		list_del_init (&req->lie);
		list_move_tail (&req->unwinds, lies);

		wb_inode->window_current += req->orig_size;

		if (!req->ordering.fulfilled) {
			/* burden increased */
			list_add_tail (&req->lie, &wb_inode->liability);

			req->ordering.lied = 1;

			wb_inode->gen++;
		}
	}

        return;
}


int
__wb_collapse_small_writes (wb_request_t *holder, wb_request_t *req)
{
        char          *ptr    = NULL;
        struct iobuf  *iobuf  = NULL;
        struct iobref *iobref = NULL;
        int            ret    = -1;

        if (!holder->iobref) {
                /* TODO: check the required size */
                iobuf = iobuf_get (req->wb_inode->this->ctx->iobuf_pool);
                if (iobuf == NULL) {
                        goto out;
                }

                iobref = iobref_new ();
                if (iobref == NULL) {
                        iobuf_unref (iobuf);
                        goto out;
                }

                ret = iobref_add (iobref, iobuf);
                if (ret != 0) {
                        iobuf_unref (iobuf);
                        iobref_unref (iobref);
                        gf_log (req->wb_inode->this->name, GF_LOG_WARNING,
                                "cannot add iobuf (%p) into iobref (%p)",
                                iobuf, iobref);
                        goto out;
                }

                iov_unload (iobuf->ptr, holder->stub->args.writev.vector,
                            holder->stub->args.writev.count);
                holder->stub->args.writev.vector[0].iov_base = iobuf->ptr;
		holder->stub->args.writev.count = 1;

                iobref_unref (holder->stub->args.writev.iobref);
                holder->stub->args.writev.iobref = iobref;

                iobuf_unref (iobuf);

                holder->iobref = iobref_ref (iobref);
        }

        ptr = holder->stub->args.writev.vector[0].iov_base + holder->write_size;

        iov_unload (ptr, req->stub->args.writev.vector,
                    req->stub->args.writev.count);

        holder->stub->args.writev.vector[0].iov_len += req->write_size;
        holder->write_size += req->write_size;
        holder->ordering.size += req->write_size;

        ret = 0;
out:
        return ret;
}


void
__wb_preprocess_winds (wb_inode_t *wb_inode)
{
        off_t         offset_expected = 0;
        size_t        space_left      = 0;
	wb_request_t *req             = NULL;
	wb_request_t *tmp             = NULL;
	wb_request_t *holder          = NULL;
	wb_conf_t    *conf            = NULL;
        int           ret             = 0;
	size_t        page_size       = 0;

	/* With asynchronous IO from a VM guest (as a file), there
	   can be two sequential writes happening in two regions
	   of the file. But individual (broken down) IO requests
	   can arrive interleaved.

	   TODO: cycle for each such sequence sifting
	   through the interleaved ops
	*/

	page_size = wb_inode->this->ctx->page_size;
	conf = wb_inode->this->private;

        list_for_each_entry_safe (req, tmp, &wb_inode->todo, todo) {
		if (!req->ordering.tempted) {
			if (holder) {
				if (wb_requests_conflict (holder, req))
					/* do not hold on write if a
					   dependent write is in queue */
					holder->ordering.go = 1;
			}
			/* collapse only non-sync writes */
			continue;
		} else if (!holder) {
			/* holder is always a non-sync write */
			holder = req;
			continue;
		}

		offset_expected = holder->stub->args.writev.off
			+ holder->write_size;

		if (req->stub->args.writev.off != offset_expected) {
			holder->ordering.go = 1;
			holder = req;
			continue;
		}

		if (!is_same_lkowner (&req->lk_owner, &holder->lk_owner)) {
			holder->ordering.go = 1;
			holder = req;
			continue;
		}

		space_left = page_size - holder->write_size;

		if (space_left < req->write_size) {
			holder->ordering.go = 1;
			holder = req;
			continue;
		}

		ret = __wb_collapse_small_writes (holder, req);
		if (ret)
			continue;

		/* collapsed request is as good as wound
		   (from its p.o.v)
		*/
		list_del_init (&req->todo);
		__wb_fulfill_request (req);

               /* Only the last @holder in queue which

                  - does not have any non-buffered-writes following it
                  - has not yet filled its capacity

                  does not get its 'go' set, in anticipation of the arrival
                  of consecutive smaller writes.
               */
        }

	/* but if trickling writes are enabled, then do not hold back
	   writes if there are no outstanding requests
	*/

	if (conf->trickling_writes && !wb_inode->transit && holder)
		holder->ordering.go = 1;

        return;
}


void
__wb_pick_winds (wb_inode_t *wb_inode, list_head_t *tasks,
		 list_head_t *liabilities)
{
	wb_request_t *req = NULL;
	wb_request_t *tmp = NULL;

	list_for_each_entry_safe (req, tmp, &wb_inode->todo, todo) {
		if (wb_liability_has_conflict (wb_inode, req))
			continue;

		if (req->ordering.tempted && !req->ordering.go)
			/* wait some more */
			continue;

		list_del_init (&req->todo);

		if (req->ordering.tempted)
			list_add_tail (&req->winds, liabilities);
		else
			list_add_tail (&req->winds, tasks);
	}
}


void
wb_do_winds (wb_inode_t *wb_inode, list_head_t *tasks)
{
	wb_request_t *req = NULL;
	wb_request_t *tmp = NULL;

	list_for_each_entry_safe (req, tmp, tasks, winds) {
		list_del_init (&req->winds);

		call_resume (req->stub);

		wb_request_unref (req);
	}
}


void
wb_process_queue (wb_inode_t *wb_inode)
{
        list_head_t tasks  = {0, };
	list_head_t lies = {0, };
	list_head_t liabilities = {0, };

        INIT_LIST_HEAD (&tasks);
        INIT_LIST_HEAD (&lies);
        INIT_LIST_HEAD (&liabilities);

        LOCK (&wb_inode->lock);
        {
		__wb_preprocess_winds (wb_inode);

		__wb_pick_winds (wb_inode, &tasks, &liabilities);

                __wb_pick_unwinds (wb_inode, &lies);

        }
        UNLOCK (&wb_inode->lock);

	wb_do_unwinds (wb_inode, &lies);

	wb_do_winds (wb_inode, &tasks);

	wb_fulfill (wb_inode, &liabilities);

        return;
}


int
wb_writev_helper (call_frame_t *frame, xlator_t *this, fd_t *fd,
		  struct iovec *vector, int32_t count, off_t offset,
		  uint32_t flags, struct iobref *iobref, dict_t *xdata)
{
	STACK_WIND (frame, default_writev_cbk,
		    FIRST_CHILD (this), FIRST_CHILD (this)->fops->writev,
		    fd, vector, count, offset, flags, iobref, xdata);
	return 0;
}


int
wb_writev (call_frame_t *frame, xlator_t *this, fd_t *fd, struct iovec *vector,
           int32_t count, off_t offset, uint32_t flags, struct iobref *iobref,
           dict_t *xdata)
{
        wb_inode_t   *wb_inode      = NULL;
	wb_conf_t    *conf          = NULL;
        gf_boolean_t  wb_disabled   = 0;
        call_stub_t  *stub          = NULL;
        int           ret           = -1;
	int           op_errno      = EINVAL;
	int           o_direct      = O_DIRECT;

	conf = this->private;
        wb_inode = wb_inode_create (this, fd->inode);
	if (!wb_inode) {
		op_errno = ENOMEM;
		goto unwind;
	}

	if (!conf->strict_O_DIRECT)
		o_direct = 0;

	if (fd->flags & (O_SYNC|O_DSYNC|o_direct))
		wb_disabled = 1;

	if (flags & (O_SYNC|O_DSYNC|O_DIRECT))
		/* O_DIRECT flag in params of writev must _always_ be honored */
		wb_disabled = 1;

	op_errno = 0;
	LOCK (&wb_inode->lock);
	{
		/* pick up a previous error in fulfillment */
		if (wb_inode->op_ret < 0)
			op_errno = wb_inode->op_errno;

		wb_inode->op_ret = 0;
	}
	UNLOCK (&wb_inode->lock);

        if (op_errno)
                goto unwind;

	if (wb_disabled)
		stub = fop_writev_stub (frame, wb_writev_helper, fd, vector,
					count, offset, flags, iobref, xdata);
	else
		stub = fop_writev_stub (frame, NULL, fd, vector, count, offset,
					flags, iobref, xdata);
        if (!stub) {
                op_errno = ENOMEM;
                goto unwind;
        }

	if (wb_disabled)
		ret = wb_enqueue (wb_inode, stub);
	else
		ret = wb_enqueue_tempted (wb_inode, stub);

	if (!ret) {
		op_errno = ENOMEM;
		goto unwind;
	}

        wb_process_queue (wb_inode);

        return 0;

unwind:
        STACK_UNWIND_STRICT (writev, frame, -1, op_errno, NULL, NULL, NULL);

        if (stub)
                call_stub_destroy (stub);

        return 0;
}


int
wb_readv_helper (call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
                 off_t offset, uint32_t flags, dict_t *xdata)
{
        STACK_WIND (frame, default_readv_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->readv, fd, size, offset, flags,
                    xdata);
        return 0;
}


int
wb_readv (call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
          off_t offset, uint32_t flags, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        call_stub_t  *stub         = NULL;

        wb_inode = wb_inode_ctx_get (this, fd->inode);
	if (!wb_inode)
		goto noqueue;

	stub = fop_readv_stub (frame, wb_readv_helper, fd, size,
			       offset, flags, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;

unwind:
        STACK_UNWIND_STRICT (readv, frame, -1, ENOMEM, NULL, 0, NULL, NULL,
                             NULL);
        return 0;

noqueue:
        STACK_WIND (frame, default_readv_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->readv, fd, size, offset, flags,
                    xdata);
        return 0;
}


int
wb_flush_bg_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, dict_t *xdata)
{
        STACK_DESTROY (frame->root);
        return 0;
}


int
wb_flush_helper (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        wb_conf_t    *conf        = NULL;
        wb_inode_t   *wb_inode    = NULL;
        call_frame_t *bg_frame    = NULL;
	int           op_errno    = 0;
	int           op_ret      = 0;

        conf = this->private;

	wb_inode = wb_inode_ctx_get (this, fd->inode);
	if (!wb_inode) {
		op_ret = -1;
		op_errno = EINVAL;
		goto unwind;
	}

        LOCK (&wb_inode->lock);
        {
		if (wb_inode->op_ret < 0) {
			op_ret = -1;
			op_errno = wb_inode->op_errno;
		}

                wb_inode->op_ret = 0;
        }
        UNLOCK (&wb_inode->lock);

	if (op_errno)
		goto unwind;

        if (conf->flush_behind)
		goto flushbehind;

	STACK_WIND (frame, default_flush_cbk, FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->flush, fd, xdata);
	return 0;

flushbehind:
	bg_frame = copy_frame (frame);
	if (!bg_frame) {
		op_ret = -1;
		op_errno = ENOMEM;
		goto unwind;
	}

	STACK_WIND (bg_frame, wb_flush_bg_cbk, FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->flush, fd, xdata);
	/* fall through */
unwind:
        STACK_UNWIND_STRICT (flush, frame, op_ret, op_errno, NULL);

        return 0;
}


int
wb_flush (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        call_stub_t  *stub         = NULL;

        wb_inode = wb_inode_ctx_get (this, fd->inode);
	if (!wb_inode)
		goto noqueue;

	stub = fop_flush_stub (frame, wb_flush_helper, fd, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;

unwind:
        STACK_UNWIND_STRICT (flush, frame, -1, ENOMEM, NULL);

        return 0;

noqueue:
        STACK_WIND (frame, default_flush_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->flush, fd, xdata);
        return 0;
}



int
wb_fsync_helper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                 int32_t datasync, dict_t *xdata)
{
        STACK_WIND (frame, default_fsync_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fsync, fd, datasync, xdata);
        return 0;
}


int
wb_fsync (call_frame_t *frame, xlator_t *this, fd_t *fd, int32_t datasync,
          dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        call_stub_t  *stub         = NULL;

        wb_inode = wb_inode_ctx_get (this, fd->inode);
	if (!wb_inode)
		goto noqueue;

	stub = fop_fsync_stub (frame, wb_fsync_helper, fd, datasync, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;

unwind:
        STACK_UNWIND_STRICT (fsync, frame, -1, ENOMEM, NULL, NULL, NULL);

        return 0;

noqueue:
        STACK_WIND (frame, default_fsync_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fsync, fd, datasync, xdata);
        return 0;
}


int
wb_stat_helper (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
        STACK_WIND (frame, default_stat_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->stat, loc, xdata);
        return 0;
}


int
wb_stat (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        call_stub_t  *stub         = NULL;


	wb_inode = wb_inode_ctx_get (this, loc->inode);
        if (!wb_inode)
		goto noqueue;

	stub = fop_stat_stub (frame, wb_stat_helper, loc, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;

unwind:
        STACK_UNWIND_STRICT (stat, frame, -1, ENOMEM, NULL, NULL);

        if (stub)
                call_stub_destroy (stub);
        return 0;

noqueue:
        STACK_WIND (frame, default_stat_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->stat, loc, xdata);
	return 0;
}


int
wb_fstat_helper (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        STACK_WIND (frame, default_fstat_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fstat, fd, xdata);
        return 0;
}


int
wb_fstat (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
	call_stub_t  *stub         = NULL;


        wb_inode = wb_inode_ctx_get (this, fd->inode);
	if (!wb_inode)
		goto noqueue;

	stub = fop_fstat_stub (frame, wb_fstat_helper, fd, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;

unwind:
        STACK_UNWIND_STRICT (fstat, frame, -1, ENOMEM, NULL, NULL);

        if (stub)
                call_stub_destroy (stub);
        return 0;

noqueue:
        STACK_WIND (frame, default_fstat_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fstat, fd, xdata);
        return 0;
}


int
wb_truncate_helper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                    off_t offset, dict_t *xdata)
{
        STACK_WIND (frame, default_truncate_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->truncate, loc, offset, xdata);
        return 0;
}


int
wb_truncate (call_frame_t *frame, xlator_t *this, loc_t *loc, off_t offset,
             dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
	call_stub_t  *stub         = NULL;

	wb_inode = wb_inode_create (this, loc->inode);
	if (!wb_inode)
		goto unwind;

	stub = fop_truncate_stub (frame, wb_truncate_helper, loc,
				  offset, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;

unwind:
        STACK_UNWIND_STRICT (truncate, frame, -1, ENOMEM, NULL, NULL, NULL);

        if (stub)
                call_stub_destroy (stub);

        return 0;
}


int
wb_ftruncate_helper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                     off_t offset, dict_t *xdata)
{
        STACK_WIND (frame, default_ftruncate_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->ftruncate, fd, offset, xdata);
        return 0;
}


int
wb_ftruncate (call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
              dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        call_stub_t  *stub         = NULL;

        wb_inode = wb_inode_create (this, fd->inode);
	if (!wb_inode)
		goto unwind;

	stub = fop_ftruncate_stub (frame, wb_ftruncate_helper, fd,
				   offset, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;

unwind:
        STACK_UNWIND_STRICT (ftruncate, frame, -1, ENOMEM, NULL, NULL, NULL);

        if (stub)
                call_stub_destroy (stub);
        return 0;
}


int
wb_setattr_helper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                   struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
        STACK_WIND (frame, default_setattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setattr, loc, stbuf, valid, xdata);
        return 0;
}


int
wb_setattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
            struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        call_stub_t  *stub         = NULL;

	wb_inode = wb_inode_ctx_get (this, loc->inode);
	if (!wb_inode)
		goto noqueue;

	stub = fop_setattr_stub (frame, wb_setattr_helper, loc, stbuf,
                                         valid, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;
unwind:
        STACK_UNWIND_STRICT (setattr, frame, -1, ENOMEM, NULL, NULL, NULL);

        if (stub)
                call_stub_destroy (stub);
	return 0;

noqueue:
        STACK_WIND (frame, default_setattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setattr, loc, stbuf, valid, xdata);
        return 0;
}


int
wb_fsetattr_helper (call_frame_t *frame, xlator_t *this, fd_t *fd,
		    struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
        STACK_WIND (frame, default_fsetattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fsetattr, fd, stbuf, valid, xdata);
        return 0;
}


int
wb_fsetattr (call_frame_t *frame, xlator_t *this, fd_t *fd,
	     struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        call_stub_t  *stub         = NULL;

	wb_inode = wb_inode_ctx_get (this, fd->inode);
	if (!wb_inode)
		goto noqueue;

	stub = fop_fsetattr_stub (frame, wb_fsetattr_helper, fd, stbuf,
				  valid, xdata);
	if (!stub)
		goto unwind;

	if (!wb_enqueue (wb_inode, stub))
		goto unwind;

	wb_process_queue (wb_inode);

        return 0;
unwind:
        STACK_UNWIND_STRICT (fsetattr, frame, -1, ENOMEM, NULL, NULL, NULL);

        if (stub)
                call_stub_destroy (stub);
	return 0;

noqueue:
        STACK_WIND (frame, default_fsetattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fsetattr, fd, stbuf, valid, xdata);
        return 0;
}


int
wb_forget (xlator_t *this, inode_t *inode)
{
        uint64_t    tmp      = 0;
        wb_inode_t *wb_inode = NULL;

        inode_ctx_del (inode, this, &tmp);

        wb_inode = (wb_inode_t *)(long)tmp;

	if (!wb_inode)
		return 0;

	LOCK (&wb_inode->lock);
	{
		GF_ASSERT (list_empty (&wb_inode->todo));
		GF_ASSERT (list_empty (&wb_inode->liability));
		GF_ASSERT (list_empty (&wb_inode->temptation));
	}
	UNLOCK (&wb_inode->lock);

        return 0;
}


int
wb_priv_dump (xlator_t *this)
{
        wb_conf_t      *conf                            = NULL;
        char            key_prefix[GF_DUMP_MAX_BUF_LEN] = {0, };
        int             ret                             = -1;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);

        conf = this->private;
        GF_VALIDATE_OR_GOTO (this->name, conf, out);

        gf_proc_dump_build_key (key_prefix, "xlator.performance.write-behind",
                                "priv");

        gf_proc_dump_add_section (key_prefix);

        gf_proc_dump_write ("aggregate_size", "%d", conf->aggregate_size);
        gf_proc_dump_write ("window_size", "%d", conf->window_size);
        gf_proc_dump_write ("flush_behind", "%d", conf->flush_behind);
        gf_proc_dump_write ("trickling_writes", "%d", conf->trickling_writes);

        ret = 0;
out:
        return ret;
}


void
__wb_dump_requests (struct list_head *head, char *prefix)
{
        char          key[GF_DUMP_MAX_BUF_LEN]        = {0, };
        char          key_prefix[GF_DUMP_MAX_BUF_LEN] = {0, }, flag = 0;
        wb_request_t *req                             = NULL;

        list_for_each_entry (req, head, all) {
                gf_proc_dump_build_key (key_prefix, key,
                                        (char *)gf_fop_list[req->fop]);

                gf_proc_dump_add_section(key_prefix);

                gf_proc_dump_write ("request-ptr", "%p", req);

                gf_proc_dump_write ("refcount", "%d", req->refcount);

		if (list_empty (&req->todo))
			gf_proc_dump_write ("wound", "yes");
		else
			gf_proc_dump_write ("wound", "no");

                if (req->fop == GF_FOP_WRITE) {
                        gf_proc_dump_write ("size", "%"GF_PRI_SIZET,
                                            req->write_size);

                        gf_proc_dump_write ("offset", "%"PRId64,
                                            req->stub->args.writev.off);

                        flag = req->ordering.lied;
                        gf_proc_dump_write ("lied", "%d", flag);

                        flag = req->ordering.append;
                        gf_proc_dump_write ("append", "%d", flag);

                        flag = req->ordering.fulfilled;
                        gf_proc_dump_write ("fulfilled", "%d", flag);

                        flag = req->ordering.go;
                        gf_proc_dump_write ("go", "%d", flag);
                }
        }
}


int
wb_inode_dump (xlator_t *this, inode_t *inode)
{
        wb_inode_t *wb_inode                       = NULL;
        int32_t     ret                            = -1;
        char       *path                           = NULL;
        char       key_prefix[GF_DUMP_MAX_BUF_LEN] = {0, };

        if ((inode == NULL) || (this == NULL)) {
                ret = 0;
                goto out;
        }

        wb_inode = wb_inode_ctx_get (this, inode);
        if (wb_inode == NULL) {
                ret = 0;
                goto out;
        }

        gf_proc_dump_build_key (key_prefix, "xlator.performance.write-behind",
                                "wb_inode");

        gf_proc_dump_add_section (key_prefix);

        __inode_path (inode, NULL, &path);
        if (path != NULL) {
                gf_proc_dump_write ("path", "%s", path);
                GF_FREE (path);
        }

        gf_proc_dump_write ("inode", "%p", inode);

        gf_proc_dump_write ("window_conf", "%"GF_PRI_SIZET,
                            wb_inode->window_conf);

        gf_proc_dump_write ("window_current", "%"GF_PRI_SIZET,
                            wb_inode->window_current);

        gf_proc_dump_write ("op_ret", "%d", wb_inode->op_ret);

        gf_proc_dump_write ("op_errno", "%d", wb_inode->op_errno);

        LOCK (&wb_inode->lock);
        {
                if (!list_empty (&wb_inode->all)) {
                        __wb_dump_requests (&wb_inode->all, key_prefix);
                }
        }
        UNLOCK (&wb_inode->lock);

        ret = 0;
out:
        return ret;
}


int
mem_acct_init (xlator_t *this)
{
        int     ret = -1;

        if (!this) {
                goto out;
        }

        ret = xlator_mem_acct_init (this, gf_wb_mt_end + 1);

        if (ret != 0) {
                gf_log (this->name, GF_LOG_ERROR, "Memory accounting init"
                        "failed");
        }

out:
        return ret;
}


int
reconfigure (xlator_t *this, dict_t *options)
{
        wb_conf_t *conf = NULL;
        int        ret  = -1;

        conf = this->private;

        GF_OPTION_RECONF ("cache-size", conf->window_size, options, size, out);

        GF_OPTION_RECONF ("flush-behind", conf->flush_behind, options, bool,
                          out);

        GF_OPTION_RECONF ("trickling-writes", conf->trickling_writes, options,
			  bool, out);

        GF_OPTION_RECONF ("strict-O_DIRECT", conf->strict_O_DIRECT, options,
			  bool, out);

        GF_OPTION_RECONF ("strict-write-ordering", conf->strict_write_ordering,
			  options, bool, out);
        ret = 0;
out:
        return ret;
}


int32_t
init (xlator_t *this)
{
        wb_conf_t *conf    = NULL;
        int32_t    ret     = -1;

        if ((this->children == NULL)
            || this->children->next) {
                gf_log (this->name, GF_LOG_ERROR,
                        "FATAL: write-behind (%s) not configured with exactly "
                        "one child", this->name);
                goto out;
        }

        if (this->parents == NULL) {
                gf_log (this->name, GF_LOG_WARNING,
                        "dangling volume. check volfilex");
        }

        conf = GF_CALLOC (1, sizeof (*conf), gf_wb_mt_wb_conf_t);
        if (conf == NULL) {
                goto out;
        }

        /* configure 'options aggregate-size <size>' */
        conf->aggregate_size = WB_AGGREGATE_SIZE;

        /* configure 'option window-size <size>' */
        GF_OPTION_INIT ("cache-size", conf->window_size, size, out);

        if (!conf->window_size && conf->aggregate_size) {
                gf_log (this->name, GF_LOG_WARNING,
                        "setting window-size to be equal to "
                        "aggregate-size(%"PRIu64")",
                        conf->aggregate_size);
                conf->window_size = conf->aggregate_size;
        }

        if (conf->window_size < conf->aggregate_size) {
                gf_log (this->name, GF_LOG_ERROR,
                        "aggregate-size(%"PRIu64") cannot be more than "
                        "window-size(%"PRIu64")", conf->aggregate_size,
                        conf->window_size);
                goto out;
        }

        /* configure 'option flush-behind <on/off>' */
        GF_OPTION_INIT ("flush-behind", conf->flush_behind, bool, out);

        GF_OPTION_INIT ("trickling-writes", conf->trickling_writes, bool, out);

        GF_OPTION_INIT ("strict-O_DIRECT", conf->strict_O_DIRECT, bool, out);

        GF_OPTION_INIT ("strict-write-ordering", conf->strict_write_ordering,
			bool, out);

        this->private = conf;
        ret = 0;

out:
        if (ret) {
                GF_FREE (conf);
        }
        return ret;
}


void
fini (xlator_t *this)
{
        wb_conf_t *conf = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);

        conf = this->private;
        if (!conf) {
                goto out;
        }

        this->private = NULL;
        GF_FREE (conf);

out:
        return;
}


struct xlator_fops fops = {
        .writev      = wb_writev,
        .readv       = wb_readv,
        .flush       = wb_flush,
        .fsync       = wb_fsync,
        .stat        = wb_stat,
        .fstat       = wb_fstat,
        .truncate    = wb_truncate,
        .ftruncate   = wb_ftruncate,
        .setattr     = wb_setattr,
        .fsetattr    = wb_fsetattr,
};


struct xlator_cbks cbks = {
        .forget   = wb_forget,
};


struct xlator_dumpops dumpops = {
        .priv      =  wb_priv_dump,
        .inodectx  =  wb_inode_dump,
};


struct volume_options options[] = {
        { .key  = {"flush-behind"},
          .type = GF_OPTION_TYPE_BOOL,
          .default_value = "on",
          .description = "If this option is set ON, instructs write-behind "
                          "translator to perform flush in background, by "
                          "returning success (or any errors, if any of "
                          "previous  writes were failed) to application even "
                          "before flush FOP is sent to backend filesystem. "
        },
        { .key  = {"cache-size", "window-size"},
          .type = GF_OPTION_TYPE_SIZET,
          .min  = 512 * GF_UNIT_KB,
          .max  = 1 * GF_UNIT_GB,
          .default_value = "1MB",
          .description = "Size of the write-behind buffer for a single file "
                         "(inode)."
        },
        { .key = {"trickling-writes"},
          .type = GF_OPTION_TYPE_BOOL,
          .default_value = "on",
        },
        { .key = {"strict-O_DIRECT"},
          .type = GF_OPTION_TYPE_BOOL,
          .default_value = "off",
        },
        { .key = {"strict-write-ordering"},
          .type = GF_OPTION_TYPE_BOOL,
          .default_value = "off",
	  .description = "Do not let later writes overtake earlier writes even "
	                  "if they do not overlap",
        },
        { .key = {NULL} },
};
