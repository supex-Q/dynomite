#include "dyn_core.h"
#include "dyn_server.h"
#include "dyn_dnode_peer.h"

void
init_response_mgr(struct response_mgr *rspmgr, struct msg *req, bool is_read,
                  uint8_t max_responses, struct conn *conn)
{
    memset(rspmgr, 0, sizeof(struct response_mgr));
    rspmgr->is_read = is_read;
    rspmgr->max_responses = max_responses;
    rspmgr->quorum_responses = max_responses/2 + 1;
    rspmgr->conn = conn;
    rspmgr->msg = req;
    req->awaiting_rsps = max_responses;
}

static bool
rspmgr_is_quorum_achieved(struct response_mgr *rspmgr)
{
    if (rspmgr->quorum_responses == 1 &&
            rspmgr->good_responses == rspmgr->quorum_responses)
        return true;
    if (rspmgr->good_responses < rspmgr->quorum_responses)
        return false;

    uint32_t chk0, chk1, chk2;
    chk0 = rspmgr->checksums[0];
    chk1 = rspmgr->checksums[1];
    if (chk0 == chk1)
        return true;
    if (rspmgr->good_responses < 3)
        return false;
    chk2 = rspmgr->checksums[2];
    if ((chk1 == chk2) || (chk0 == chk2))
        return true;
    return false;
}

// Wait for all responses before responding
/*bool
rspmgr_check_is_done(struct response_mgr *rspmgr)
{
    uint8_t pending_responses = rspmgr->max_responses -
                                rspmgr->good_responses -
                                rspmgr->error_responses;
    if (pending_responses) {
        rspmgr->done = false;
    } else
        rspmgr->done = true;
    return rspmgr->done;
}*/

// Wait for only quorum number of responses before responding
bool
rspmgr_check_is_done(struct response_mgr *rspmgr)
{
    uint8_t pending_responses = rspmgr->max_responses -
                                rspmgr->good_responses -
                                rspmgr->error_responses;
    // do the required calculation and tell if we are done here
    if (rspmgr->good_responses >= rspmgr->quorum_responses) {
        // We received enough good responses but do their checksum match?
        if (rspmgr_is_quorum_achieved(rspmgr)) {
            log_info("req %lu quorum achieved", rspmgr->msg->id);
            rspmgr->done = true;
        } else if (pending_responses) {
            // Theres a mismatch in checksum. Wait for any pending responses
            rspmgr->done = false;
        } else {
            // no pending responses, and the checksum do not match.
            rspmgr->done = true;
        }
    } else if ((pending_responses + rspmgr->good_responses) <
                                                       rspmgr->quorum_responses) {
        // Even if we receive all the pending responses, still we do not form
        // a quorum, So decision is done, no quorum possible
        rspmgr->done = true;
    }
    return rspmgr->done;
}

static void
rspmgr_incr_non_quorum_responses_stats(struct response_mgr *rspmgr)
{
    if (rspmgr->is_read)
        stats_pool_incr(conn_to_ctx(rspmgr->conn),
                        client_non_quorum_r_responses);
    else
        stats_pool_incr(conn_to_ctx(rspmgr->conn),
                        client_non_quorum_w_responses);

}

struct msg*
rspmgr_get_response(struct response_mgr *rspmgr)
{
    // no quorum possible
    if (rspmgr->good_responses < rspmgr->quorum_responses) {
        ASSERT(rspmgr->err_rsp);
        rspmgr_incr_non_quorum_responses_stats(rspmgr);
        log_error("req: %lu return non quorum error rsp %p good rsp:%u quorum: %u",
                  rspmgr->msg->id, rspmgr->err_rsp, rspmgr->good_responses,
                  rspmgr->quorum_responses);
        if (log_loggable(LOG_INFO))
            msg_dump(LOG_INFO, rspmgr->err_rsp);
        return rspmgr->err_rsp;
    }

    uint32_t chk0, chk1, chk2;
    chk0 = rspmgr->checksums[0];
    chk1 = rspmgr->checksums[1];
    if (chk0 == chk1) {
        return rspmgr->responses[0];
    } else if (rspmgr->good_responses == 3) {
        chk2 = rspmgr->checksums[2];
        if (chk1 == chk2)
            return rspmgr->responses[1];
        else if (chk0 == chk2)
            return rspmgr->responses[0];
    }
    rspmgr_incr_non_quorum_responses_stats(rspmgr);
    if (log_loggable(LOG_DEBUG)) {
        log_error("Request: ");
        msg_dump(LOG_DEBUG, rspmgr->msg);
    }
    if (log_loggable(LOG_VVERB)) {
        log_error("Respone 0: ");
        msg_dump(LOG_VVERB, rspmgr->responses[0]);
        log_error("Respone 1: ");
        msg_dump(LOG_VVERB, rspmgr->responses[1]);
        if (rspmgr->good_responses == 3) {
            log_error("Respone 2: ");
            msg_dump(LOG_VVERB, rspmgr->responses[2]);
        }
    }
    return g_reconcile_responses(rspmgr);
}

void
rspmgr_free_other_responses(struct response_mgr *rspmgr, struct msg *dont_free)
{
    int i;
    for (i = 0; i < rspmgr->good_responses; i++) {
        if (dont_free && (rspmgr->responses[i] == dont_free))
            continue;
        rsp_put(rspmgr->responses[i]);
    }
    if (rspmgr->err_rsp) {
        if (dont_free && (dont_free == rspmgr->err_rsp))
            return;
        rsp_put(rspmgr->err_rsp);
    }
}

rstatus_t
rspmgr_submit_response(struct response_mgr *rspmgr, struct msg*rsp)
{
    log_info("req %d submitting response %d awaiting_rsps %d",
              rspmgr->msg->id, rsp->id, rspmgr->msg->awaiting_rsps);
    if (rsp->is_error) {
        log_debug(LOG_VERB, "Received error response %d:%d for req %d:%d",
                  rsp->id, rsp->parent_id, rspmgr->msg->id, rspmgr->msg->parent_id);
        rspmgr->error_responses++;
        if (rspmgr->err_rsp == NULL)
            rspmgr->err_rsp = rsp;
        else
            rsp_put(rsp);
    } else {
        rspmgr->checksums[rspmgr->good_responses] = msg_payload_crc32(rsp);
        log_debug(LOG_VERB, "Good response %d:%d checksum %u", rsp->id,
                  rsp->parent_id, rspmgr->checksums[rspmgr->good_responses]);
        rspmgr->responses[rspmgr->good_responses++] =  rsp;
    }
    msg_decr_awaiting_rsps(rspmgr->msg);
    return DN_OK;
}

rstatus_t
rspmgr_clone_responses(struct response_mgr *rspmgr, struct array *responses)
{
    uint8_t iter = 0;
    struct msg *dst = NULL;
    rstatus_t s = DN_OK;
    for(iter = 0; iter < rspmgr->good_responses; iter++)
    {
        struct msg *src = rspmgr->responses[iter];
        dst = rsp_get(rspmgr->conn);
        if (!dst) {
            s = DN_ENOMEM;
            goto error;
        }

        s = msg_clone(src, STAILQ_FIRST(&src->mhdr), dst);
        if (s != DN_OK)
            goto error;
        struct msg **pdst = (struct msg **)array_push(responses);
        *pdst = dst;
    }
    return DN_OK;
error:
    rsp_put(dst);
    return ;
}
