/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rdkafka_int.h"
#include "rdkafka_admin.h"
#include "rdkafka_request.h"
#include "rdkafka_aux.h"

#include <stdarg.h>



/** @brief Descriptive strings for rko_u.admin_request.state */
static const char *rd_kafka_admin_state_desc[] = {
        "initializing",
        "waiting for broker",
        "waiting for controller",
        "constructing request",
        "waiting for response from broker",
};



/**
 * @brief Admin API implementation.
 *
 * The public Admin API in librdkafka exposes a completely asynchronous
 * interface where the initial request API (e.g., ..CreateTopics())
 * is non-blocking and returns immediately, and the application polls
 * a ..queue_t for the result.
 *
 * The underlying handling of the request is also completely asynchronous
 * inside librdkafka, for two reasons:
 *  - everything is async in librdkafka so adding something new that isn't
 *    would mean that existing functionality will need to be changed if
 *    it should be able to work simultaneously (such as statistics, timers,
 *    etc). There is no functional value to making the admin API
 *    synchronous internally, even if it would simplify its implementation.
 *    So making it async allows the Admin API to be used with existing
 *    client types in existing applications without breakage.
 *  - the async approach allows multiple outstanding Admin API requests
 *    simultaneously.
 *
 * The internal async implementation relies on the following concepts:
 *  - it uses a single rko (rd_kafka_op_t) to maintain state.
 *  - the rko has a callback attached - called the worker callback.
 *  - the worker callback is a small state machine that triggers
 *    async operations (be it controller lookups, timeout timers,
 *    protocol transmits, etc).
 *  - the worker callback is only called on the rdkafka main thread.
 *  - the callback is triggered by different events and sources by enqueuing
 *    the rko on the rdkafka main ops queue.
 *
 *
 * Let's illustrate this with a DeleteTopics example. This might look
 * daunting, but it boils down to an asynchronous state machine being
 * triggered by enqueuing the rko op.
 *
 *  1. [app thread] The user constructs the input arguments,
 *     including a response rkqu queue and then calls DeleteTopics().
 *
 *  2. [app thread] DeleteTopics() creates a new internal op (rko) of type
 *     RD_KAFKA_OP_DELETETOPICS, makes a **copy** on the rko of all the
 *     input arguments (which allows the caller to free the originals
 *     whenever she likes). The rko op worker callback is set to the
 *     generic admin worker callback rd_kafka_admin_worker()
 *
 *  3. [app thread] DeleteTopics() enqueues the rko on librdkafka's main ops
 *     queue that is served by the rdkafka main thread in rd_kafka_thread_main()
 *
 *  4. [rdkafka main thread] The rko is dequeued by rd_kafka_q_serve and
 *     the rd_kafka_poll_cb() is called.
 *
 *  5. [rdkafka main thread] The rko_type switch case identifies the rko
 *     as an RD_KAFKA_OP_DELETETOPICS which is served by the op callback
 *     set in step 2.
 *
 *  6. [rdkafka main thread] The worker callback is called.
 *     After some initial checking of err==ERR__DESTROY events
 *     (which is used to clean up outstanding ops (etc) on termination),
 *     the code hits a state machine using rko_u.admin.request_state.
 *
 *  7. [rdkafka main thread] The initial state is RD_KAFKA_ADMIN_STATE_INIT
 *     where the worker validates the user input.
 *     An enqueue once (eonce) object is created - the use of this object
 *     allows having multiple outstanding async functions referencing the
 *     same underlying rko object, but only allowing the first one
 *     to trigger an event.
 *     A timeout timer is set up to trigger the eonce object when the
 *     full options.request_timeout has elapsed.
 *
 *  8. [rdkafka main thread] After initialization the state is updated
 *     to WAIT_BROKER or WAIT_CONTROLLER and the code falls through to
 *     looking up a specific broker or the controller broker and waiting for
 *     an active connection.
 *     Both the lookup and the waiting for an active connection are
 *     fully asynchronous, and the same eonce used for the timer is passed
 *     to the rd_kafka_broker_controller_async() or broker_async() functions
 *     which will trigger the eonce when a broker state change occurs.
 *     If the controller is already known (from metadata) and the connection
 *     is up a rkb broker object is returned and the eonce is not used,
 *     skip to step 11.
 *
 *  9. [rdkafka main thread] Upon metadata retrieval (which is triggered
 *     automatically by other parts of the code) the controller_id may be
 *     updated in which case the eonce is triggered.
 *     The eonce triggering enqueues the original rko on the rdkafka main
 *     ops queue again and we go to step 8 which will check if the controller
 *     connection is up.
 *
 * 10. [broker thread] If the controller_id is now known we wait for
 *     the corresponding broker's connection to come up. This signaling
 *     is performed from the broker thread upon broker state changes
 *     and uses the same eonce. The eonce triggering enqueues the original
 *     rko on the rdkafka main ops queue again we go to back to step 8
 *     to check if broker is now available.
 *
 * 11. [rdkafka main thread] Back in the worker callback we now have an
 *     rkb broker pointer (with reference count increased) for the controller
 *     with the connection up (it might go down while we're referencing it,
 *     but that does not stop us from enqueuing a protocol request).
 *
 * 12. [rdkafka main thread] A DeleteTopics protocol request buffer is
 *     constructed using the input parameters saved on the rko and the
 *     buffer is enqueued on the broker's transmit queue.
 *     The buffer is set up to provide the reply buffer on the rdkafka main
 *     ops queue (the same queue we are operating from) with a handler
 *     callback of rd_kafka_admin_handle_response().
 *     The state is updated to the RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE.
 *
 * 13. [broker thread] If the request times out, a response with error code
 *     (ERR__TIMED_OUT) is enqueued. Go to 16.
 *
 * 14. [broker thread] If a response is received, the response buffer
 *     is enqueued. Go to 16.
 *
 * 15. [rdkafka main thread] The buffer callback (..handle_response())
 *     is called, which attempts to extra the original rko from the eonce,
 *     but if the eonce has already been triggered by some other source
 *     (the timeout timer) the buffer callback simply returns and does nothing
 *     since the admin request is over and a result (probably a timeout)
 *     has been enqueued for the application.
 *     If the rko was still intact we temporarily set the reply buffer
 *     in the rko struct and call the worker callback. Go to 17.
 *
 * 16. [rdkafka main thread] The worker callback is called in state
 *     RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE without a response but with an error.
 *     An error result op is created and enqueued on the application's
 *     provided response rkqu queue.
 *     
 * 17. [rdkafka main thread] The worker callback is called in state
 *     RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE with a response buffer with no
 *     error set.
 *     The worker calls the response `parse()` callback to parse the response
 *     buffer and populates a result op (rko_result) with the response
 *     information (such as per-topic error codes, etc).
 *     The result op is returned to the worker.
 *
 * 18. [rdkafka main thread] The worker enqueues the result up (rko_result)
 *     on the application's provided response rkqu queue.
 *
 * 19. [app thread] The application calls rd_kafka_queue_poll() to
 *     receive the result of the operation. The result may have been
 *     enqueued in step 18 thanks to succesful completion, or in any
 *     of the earlier stages when an error was encountered.
 *
 * 20. [app thread] The application uses rd_kafka_event_DeleteTopics_result()
 *     to retrieve the request-specific result type.
 *
 * 21. Done.
 *
 */


/**
 * @brief Admin op callback types
 */
typedef rd_kafka_resp_err_t (rd_kafka_admin_Request_cb_t) (
        rd_kafka_broker_t *rkb,
        const rd_list_t *configs /*(ConfigResource_t*)*/,
        rd_kafka_AdminOptions_t *options,
        char *errstr, size_t errstr_size,
        rd_kafka_replyq_t replyq,
        rd_kafka_resp_cb_t *resp_cb,
        void *opaque)
        RD_WARN_UNUSED_RESULT;

typedef rd_kafka_resp_err_t (rd_kafka_admin_Response_parse_cb_t) (
        rd_kafka_op_t *rko_req,
        rd_kafka_op_t **rko_resultp,
        rd_kafka_buf_t *reply,
        char *errstr, size_t errstr_size)
        RD_WARN_UNUSED_RESULT;



/**
 * @struct Request-specific worker callbacks.
 */
struct rd_kafka_admin_worker_cbs {
        /**< Protocol request callback which is called
         *   to construct and send the request. */
        rd_kafka_admin_Request_cb_t *request;

        /**< Protocol response parser callback which is called
         *   to translate the response to a rko_result op. */
        rd_kafka_admin_Response_parse_cb_t *parse;
};


/* Forward declarations */
static void rd_kafka_AdminOptions_init (rd_kafka_t *rk,
                                        rd_kafka_AdminOptions_t *options);
static rd_kafka_op_res_t
rd_kafka_admin_worker (rd_kafka_t *rk, rd_kafka_q_t *rkq, rd_kafka_op_t *rko);
static rd_kafka_ConfigEntry_t *
rd_kafka_ConfigEntry_copy (const rd_kafka_ConfigEntry_t *src);
static void rd_kafka_ConfigEntry_free (void *ptr);
static void *rd_kafka_ConfigEntry_list_copy (const void *src, void *opaque);


/**
 * @name Common admin request code
 * @{
 *
 *
 */

/**
 * @brief Create a new admin_result op based on the request op \p rko_req
 */
static rd_kafka_op_t *rd_kafka_admin_result_new (const rd_kafka_op_t *rko_req) {
        rd_kafka_op_t *rko_result;

        rko_result = rd_kafka_op_new(RD_KAFKA_OP_ADMIN_RESULT);
        rko_result->rko_rk = rko_req->rko_rk;

        rko_result->rko_u.admin_result.opaque =
                rd_kafka_confval_get_ptr(&rko_req->rko_u.admin_request.
                                         options.opaque);
        rko_result->rko_u.admin_result.reqtype = rko_req->rko_type;
        rko_result->rko_evtype = rko_req->rko_u.admin_request.reply_event_type;

        return rko_result;
}


/**
 * @brief Set error code and error string on admin_result op \p rko.
 */
static void rd_kafka_admin_result_set_err0 (rd_kafka_op_t *rko,
                                            rd_kafka_resp_err_t err,
                                            const char *fmt, va_list ap) {
        char buf[512];

        rd_vsnprintf(buf, sizeof(buf), fmt, ap);

        rko->rko_err = err;

        if (rko->rko_u.admin_result.errstr)
                rd_free(rko->rko_u.admin_result.errstr);
        rko->rko_u.admin_result.errstr = rd_strdup(buf);

        rd_kafka_dbg(rko->rko_rk, ADMIN, "ADMINFAIL",
                     "Admin %s result error: %s",
                     rd_kafka_op2str(rko->rko_u.admin_result.reqtype),
                     rko->rko_u.admin_result.errstr);
}

/**
 * @sa rd_kafka_admin_result_set_err0
 */
static RD_UNUSED void rd_kafka_admin_result_set_err (rd_kafka_op_t *rko,
                                                     rd_kafka_resp_err_t err,
                                                     const char *fmt, ...) {
        va_list ap;

        va_start(ap, fmt);
        rd_kafka_admin_result_set_err0(rko, err, fmt, ap);
        va_end(ap);
}

/**
 * @brief Enqueue admin_result on application's queue.
 */
static RD_INLINE
void rd_kafka_admin_result_enq (rd_kafka_op_t *rko_req,
                                       rd_kafka_op_t *rko_result) {
        rd_kafka_replyq_enq(&rko_req->rko_u.admin_request.replyq, rko_result,
                            rko_req->rko_u.admin_request.replyq.version);
}

/**
 * @brief Set request-level error code and string in reply op.
 */
static void rd_kafka_admin_result_fail (rd_kafka_op_t *rko_req,
                                        rd_kafka_resp_err_t err,
                                        const char *fmt, ...) {
        va_list ap;
        rd_kafka_op_t *rko_result;

        rko_result = rd_kafka_admin_result_new(rko_req);

        va_start(ap, fmt);
        rd_kafka_admin_result_set_err0(rko_result, err, fmt, ap);
        va_end(ap);

        rd_kafka_admin_result_enq(rko_req, rko_result);
}



/**
 * @brief Return the topics list from a topic-related result object.
 */
static const rd_kafka_topic_result_t **
rd_kafka_admin_result_ret_topics (const rd_kafka_op_t *rko,
                                  size_t *cntp) {
        rd_kafka_op_type_t reqtype =
                rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_CREATETOPICS ||
                  reqtype == RD_KAFKA_OP_DELETETOPICS ||
                  reqtype == RD_KAFKA_OP_CREATEPARTITIONS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_topic_result_t **)rko->rko_u.admin_result.
                results.rl_elems;
}

/**
 * @brief Return the ConfigResource list from a config-related result object.
 */
static const rd_kafka_ConfigResource_t **
rd_kafka_admin_result_ret_resources (const rd_kafka_op_t *rko,
                                     size_t *cntp) {
        rd_kafka_op_type_t reqtype =
                rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_ALTERCONFIGS ||
                  reqtype == RD_KAFKA_OP_DESCRIBECONFIGS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_ConfigResource_t **)rko->rko_u.admin_result.
                results.rl_elems;
}




/**
 * @brief Create a new admin_request op of type \p optype and sets up the
 *        generic (type independent files).
 *
 *        The caller shall then populate the admin_request.args list
 *        and enqueue the op on rk_ops for further processing work.
 *
 * @param cbs Callbacks, must reside in .data segment.
 * @param options Optional options, may be NULL to use defaults.
 *
 * @locks none
 * @locality application thread
 */
static rd_kafka_op_t *
rd_kafka_admin_request_op_new (rd_kafka_t *rk,
                               rd_kafka_op_type_t optype,
                               rd_kafka_event_type_t reply_event_type,
                               const struct rd_kafka_admin_worker_cbs *cbs,
                               const rd_kafka_AdminOptions_t *options,
                               rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;

        rd_assert(rk);
        rd_assert(rkqu);
        rd_assert(cbs);

        rko = rd_kafka_op_new_cb(rk, optype, rd_kafka_admin_worker);

        rko->rko_u.admin_request.reply_event_type = reply_event_type;

        rko->rko_u.admin_request.cbs = (struct rd_kafka_admin_worker_cbs *)cbs;

        /* Make a copy of the options */
        if (options)
                rko->rko_u.admin_request.options = *options;
        else
                rd_kafka_AdminOptions_init(rk,
                                           &rko->rko_u.admin_request.options);

        /* Default to controller */
        rko->rko_u.admin_request.broker_id = -1;

        /* Calculate absolute timeout */
        rko->rko_u.admin_request.abs_timeout =
                rd_timeout_init(
                        rd_kafka_confval_get_int(&rko->rko_u.admin_request.
                                                 options.request_timeout));

        /* Setup enq-op-once, which is triggered by either timer code
         * or future wait-controller code. */
        rko->rko_u.admin_request.eonce =
                rd_kafka_enq_once_new(rko, RD_KAFKA_REPLYQ(rk->rk_ops, 0));

        /* The timer itself must be started from the rdkafka main thread,
         * not here. */

        /* Set up replyq */
        rd_kafka_set_replyq(&rko->rko_u.admin_request.replyq,
                            rkqu->rkqu_q, 0);

        rko->rko_u.admin_request.state = RD_KAFKA_ADMIN_STATE_INIT;
        return rko;
}


/**
 * @brief Timer timeout callback for the admin rko's eonce object.
 */
static void rd_kafka_admin_eonce_timeout_cb (rd_kafka_timers_t *rkts,
                                             void *arg) {
        rd_kafka_enq_once_t *eonce = arg;

        rd_kafka_enq_once_trigger(eonce, RD_KAFKA_RESP_ERR__TIMED_OUT,
                                  "timer timeout");
}



/**
 * @brief Common worker destroy to be called in destroy: label
 *        in worker.
 */
static void rd_kafka_admin_common_worker_destroy (rd_kafka_t *rk,
                                                  rd_kafka_op_t *rko) {
        int timer_was_stopped;

        /* Free resources for this op. */
        timer_was_stopped =
                rd_kafka_timer_stop(&rk->rk_timers,
                                    &rko->rko_u.admin_request.tmr, rd_true);


        if (rko->rko_u.admin_request.eonce) {
                /* Remove the stopped timer's eonce reference since its
                 * callback will not have fired if we stopped the timer. */
                if (timer_was_stopped)
                        rd_kafka_enq_once_del_source(rko->rko_u.admin_request.
                                                     eonce, "timeout timer");

                /* This is thread-safe to do even if there are outstanding
                 * timers or wait-controller references to the eonce
                 * since they only hold direct reference to the eonce,
                 * not the rko (the eonce holds a reference to the rko but
                 * it is cleared here). */
                rd_kafka_enq_once_destroy(rko->rko_u.admin_request.eonce);
                rko->rko_u.admin_request.eonce = NULL;
        }
}



/**
 * @brief Asynchronously look up a broker.
 *        To be called repeatedly from each invocation of the worker
 *        when in state RD_KAFKA_ADMIN_STATE_WAIT_BROKER until
 *        a valid rkb is returned.
 *
 * @returns the broker rkb with refcount increased, or NULL if not yet
 *          available.
 */
static rd_kafka_broker_t *
rd_kafka_admin_common_get_broker (rd_kafka_t *rk,
                                  rd_kafka_op_t *rko,
                                  int32_t broker_id) {
        rd_kafka_broker_t *rkb;

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: looking up broker %"PRId32,
                     rd_kafka_op2str(rko->rko_type), broker_id);

        /* Since we're iterating over this broker_async() call
         * (asynchronously) until a broker is availabe (or timeout)
         * we need to re-enable the eonce to be triggered again (which
         * is not necessary the first time we get here, but there
         * is no harm doing it then either). */
        rd_kafka_enq_once_reenable(rko->rko_u.admin_request.eonce,
                                   rko, RD_KAFKA_REPLYQ(rk->rk_ops, 0));

        /* Look up the broker asynchronously, if the broker
         * is not available the eonce is registered for broker
         * state changes which will cause our function to be called
         * again as soon as (any) broker state changes.
         * When we are called again we perform the broker lookup
         * again and hopefully get an rkb back, otherwise defer a new
         * async wait. Repeat until success or timeout. */
        if (!(rkb = rd_kafka_broker_get_async(
                      rk, broker_id, RD_KAFKA_BROKER_STATE_UP,
                      rko->rko_u.admin_request.eonce))) {
                /* Broker not available, wait asynchronously
                 * for broker metadata code to trigger eonce. */
                return NULL;
        }

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: broker %"PRId32" is %s",
                     rd_kafka_op2str(rko->rko_type), broker_id, rkb->rkb_name);

        return rkb;
}


/**
 * @brief Asynchronously look up the controller.
 *        To be called repeatedly from each invocation of the worker
 *        when in state RD_KAFKA_ADMIN_STATE_WAIT_CONTROLLER until
 *        a valid rkb is returned.
 *
 * @returns the controller rkb with refcount increased, or NULL if not yet
 *          available.
 */
static rd_kafka_broker_t *
rd_kafka_admin_common_get_controller (rd_kafka_t *rk,
                                      rd_kafka_op_t *rko) {
        rd_kafka_broker_t *rkb;

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: looking up controller",
                     rd_kafka_op2str(rko->rko_type));

        /* Since we're iterating over this controller_async() call
         * (asynchronously) until a controller is availabe (or timeout)
         * we need to re-enable the eonce to be triggered again (which
         * is not necessary the first time we get here, but there
         * is no harm doing it then either). */
        rd_kafka_enq_once_reenable(rko->rko_u.admin_request.eonce,
                                   rko, RD_KAFKA_REPLYQ(rk->rk_ops, 0));

        /* Look up the controller asynchronously, if the controller
         * is not available the eonce is registered for broker
         * state changes which will cause our function to be called
         * again as soon as (any) broker state changes.
         * When we are called again we perform the controller lookup
         * again and hopefully get an rkb back, otherwise defer a new
         * async wait. Repeat until success or timeout. */
        if (!(rkb = rd_kafka_broker_controller_async(
                      rk, RD_KAFKA_BROKER_STATE_UP,
                      rko->rko_u.admin_request.eonce))) {
                /* Controller not available, wait asynchronously
                 * for controller code to trigger eonce. */
                return NULL;
        }

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: controller %s",
                     rd_kafka_op2str(rko->rko_type), rkb->rkb_name);

        return rkb;
}



/**
 * @brief Handle response from broker by triggering worker callback.
 *
 * @param opaque is the eonce from the worker protocol request call.
 */
static void rd_kafka_admin_handle_response (rd_kafka_t *rk,
                                            rd_kafka_broker_t *rkb,
                                            rd_kafka_resp_err_t err,
                                            rd_kafka_buf_t *reply,
                                            rd_kafka_buf_t *request,
                                            void *opaque) {
        rd_kafka_enq_once_t *eonce = opaque;
        rd_kafka_op_t *rko;

        /* From ...add_source("send") */
        rko = rd_kafka_enq_once_disable(eonce);

        if (!rko) {
                /* The operation timed out and the worker was
                 * dismantled while we were waiting for broker response,
                 * do nothing - everything has been cleaned up. */
                rd_kafka_dbg(rk, ADMIN, "ADMIN",
                             "Dropping outdated %sResponse with return code %s",
                             request ?
                             rd_kafka_ApiKey2str(request->rkbuf_reqhdr.ApiKey):
                             "???",
                             rd_kafka_err2str(err));
                return;
        }

        /* Attach reply buffer to rko for parsing in the worker. */
        rd_assert(!rko->rko_u.admin_request.reply_buf);
        rko->rko_u.admin_request.reply_buf = reply;
        rko->rko_err = err;

        if (rko->rko_op_cb(rk, NULL, rko) == RD_KAFKA_OP_RES_HANDLED)
                rd_kafka_op_destroy(rko);

}



/**
 * @brief Common worker state machine handling regardless of request type.
 *
 * Tasks:
 *  - Sets up timeout on first call.
 *  - Checks for timeout.
 *  - Checks for and fails on errors.
 *  - Async Controller and broker lookups
 *  - Calls the Request callback
 *  - Calls the parse callback
 *  - Result reply
 *  - Destruction of rko
 *
 * rko->rko_err may be one of:
 * RD_KAFKA_RESP_ERR_NO_ERROR, or
 * RD_KAFKA_RESP_ERR__DESTROY for queue destruction cleanup, or
 * RD_KAFKA_RESP_ERR__TIMED_OUT if request has timed out,
 * or any other error code triggered by other parts of the code.
 *
 * @returns a hint to the op code whether the rko should be destroyed or not.
 */
static rd_kafka_op_res_t
rd_kafka_admin_worker (rd_kafka_t *rk, rd_kafka_q_t *rkq, rd_kafka_op_t *rko) {
        const char *name = rd_kafka_op2str(rko->rko_type);
        rd_ts_t timeout_in;
        rd_kafka_broker_t *rkb = NULL;
        rd_kafka_resp_err_t err;
        char errstr[512];

        if (rd_kafka_terminating(rk)) {
                rd_kafka_dbg(rk, ADMIN, name,
                             "%s worker called in state %s: "
                             "handle is terminating: %s",
                             name,
                             rd_kafka_admin_state_desc[rko->rko_u.
                                                       admin_request.state],
                             rd_kafka_err2str(rko->rko_err));
                goto destroy;
        }

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                goto destroy; /* rko being destroyed (silent) */

        rd_kafka_dbg(rk, ADMIN, name,
                     "%s worker called in state %s: %s",
                     name,
                     rd_kafka_admin_state_desc[rko->rko_u.admin_request.state],
                     rd_kafka_err2str(rko->rko_err));

        rd_assert(thrd_is_current(rko->rko_rk->rk_thread));

        /* Check for errors raised asynchronously (e.g., by timer) */
        if (rko->rko_err) {
                rd_kafka_admin_result_fail(
                        rko, rko->rko_err,
                        "Failed while %s: %s",
                        rd_kafka_admin_state_desc[rko->rko_u.
                                                  admin_request.state],
                        rd_kafka_err2str(rko->rko_err));
                goto destroy;
        }

        /* Check for timeout */
        timeout_in = rd_timeout_remains_us(rko->rko_u.admin_request.
                                           abs_timeout);
        if (timeout_in <= 0) {
                rd_kafka_admin_result_fail(
                        rko, RD_KAFKA_RESP_ERR__TIMED_OUT,
                        "Timed out %s",
                        rd_kafka_admin_state_desc[rko->rko_u.
                                                  admin_request.state]);
                goto destroy;
        }

 redo:
        switch (rko->rko_u.admin_request.state)
        {
        case RD_KAFKA_ADMIN_STATE_INIT:
        {
                int32_t broker_id;

                /* First call. */

                /* Set up timeout timer. */
                rd_kafka_enq_once_add_source(rko->rko_u.admin_request.eonce,
                                             "timeout timer");
                rd_kafka_timer_start_oneshot(&rk->rk_timers,
                                             &rko->rko_u.admin_request.tmr,
                                             timeout_in,
                                             rd_kafka_admin_eonce_timeout_cb,
                                             rko->rko_u.admin_request.eonce);

                /* Use explicitly specified broker_id, if available. */
                broker_id = (int32_t)rd_kafka_confval_get_int(
                        &rko->rko_u.admin_request.options.broker);

                if (broker_id != -1) {
                        rd_kafka_dbg(rk, ADMIN, name,
                                     "%s using explicitly "
                                     "set broker id %"PRId32
                                     " rather than %"PRId32,
                                     name, broker_id,
                                     rko->rko_u.admin_request.broker_id);
                        rko->rko_u.admin_request.broker_id = broker_id;
                }

                /* Look up controller or specific broker. */
                if (rko->rko_u.admin_request.broker_id != -1) {
                        /* Specific broker */
                        rko->rko_u.admin_request.state =
                                RD_KAFKA_ADMIN_STATE_WAIT_BROKER;
                } else {
                        /* Controller */
                        rko->rko_u.admin_request.state =
                                RD_KAFKA_ADMIN_STATE_WAIT_CONTROLLER;
                }
                goto redo;  /* Trigger next state immediately */
        }


        case RD_KAFKA_ADMIN_STATE_WAIT_BROKER:
                /* Broker lookup */
                if (!(rkb = rd_kafka_admin_common_get_broker(
                              rk, rko, rko->rko_u.admin_request.broker_id))) {
                        /* Still waiting for broker to become available */
                        return RD_KAFKA_OP_RES_KEEP;
                }

                rko->rko_u.admin_request.state =
                        RD_KAFKA_ADMIN_STATE_CONSTRUCT_REQUEST;
                goto redo;

        case RD_KAFKA_ADMIN_STATE_WAIT_CONTROLLER:
                if (!(rkb = rd_kafka_admin_common_get_controller(rk, rko))) {
                        /* Still waiting for controller to become available. */
                        return RD_KAFKA_OP_RES_KEEP;
                }

                rko->rko_u.admin_request.state =
                        RD_KAFKA_ADMIN_STATE_CONSTRUCT_REQUEST;
                goto redo;


        case RD_KAFKA_ADMIN_STATE_CONSTRUCT_REQUEST:
                /* Got broker, send protocol request. */

                /* Make sure we're called from a 'goto redo' where
                 * the rkb was set. */
                rd_assert(rkb);

                /* Still need to use the eonce since this worker may
                 * time out while waiting for response from broker, in which
                 * case the broker response will hit an empty eonce (ok). */
                rd_kafka_enq_once_add_source(rko->rko_u.admin_request.eonce,
                                             "send");

                /* Send request (async) */
                err = rko->rko_u.admin_request.cbs->request(
                        rkb,
                        &rko->rko_u.admin_request.args,
                        &rko->rko_u.admin_request.options,
                        errstr, sizeof(errstr),
                        RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                        rd_kafka_admin_handle_response,
                        rko->rko_u.admin_request.eonce);

                /* Loose broker refcount from get_broker(), get_controller() */
                rd_kafka_broker_destroy(rkb);

                if (err) {
                        rd_kafka_enq_once_del_source(
                                rko->rko_u.admin_request.eonce, "send");
                        rd_kafka_admin_result_fail(rko, err, "%s", errstr);
                        goto destroy;
                }

                rko->rko_u.admin_request.state =
                        RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE;

                /* Wait asynchronously for broker response, which will
                 * trigger the eonce and worker to be called again. */
                return RD_KAFKA_OP_RES_KEEP;


        case RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE:
        {
                rd_kafka_op_t *rko_result;

                /* Response received.
                 * Parse response and populate result to application */
                err = rko->rko_u.admin_request.cbs->parse(
                        rko, &rko_result,
                        rko->rko_u.admin_request.reply_buf,
                        errstr, sizeof(errstr));
                if (err) {
                        rd_kafka_admin_result_fail(
                                rko, err,
                                "%s worker failed to parse response: %s",
                                name, errstr);
                        goto destroy;
                }

                /* Enqueue result on application queue, we're done. */
                rd_kafka_admin_result_enq(rko, rko_result);

                goto destroy;
        }
        }

        return RD_KAFKA_OP_RES_KEEP;

 destroy:
        rd_kafka_admin_common_worker_destroy(rk, rko);
        return RD_KAFKA_OP_RES_HANDLED; /* trigger's op_destroy() */

}


/**@}*/


/**
 * @name Generic AdminOptions
 * @{
 *
 *
 */

rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_request_timeout (rd_kafka_AdminOptions_t *options,
                                           int timeout_ms,
                                           char *errstr, size_t errstr_size) {
        return rd_kafka_confval_set_type(&options->request_timeout,
                                         RD_KAFKA_CONFVAL_INT, &timeout_ms,
                                         errstr, errstr_size);
}


rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_operation_timeout (rd_kafka_AdminOptions_t *options,
                                             int timeout_ms,
                                             char *errstr, size_t errstr_size) {
        return rd_kafka_confval_set_type(&options->operation_timeout,
                                         RD_KAFKA_CONFVAL_INT, &timeout_ms,
                                         errstr, errstr_size);
}


rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_validate_only (rd_kafka_AdminOptions_t *options,
                                        int true_or_false,
                                        char *errstr, size_t errstr_size) {
        return rd_kafka_confval_set_type(&options->validate_only,
                                         RD_KAFKA_CONFVAL_INT, &true_or_false,
                                         errstr, errstr_size);
}

rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_incremental (rd_kafka_AdminOptions_t *options,
                                       int true_or_false,
                                       char *errstr, size_t errstr_size) {
        return rd_kafka_confval_set_type(&options->incremental,
                                         RD_KAFKA_CONFVAL_INT, &true_or_false,
                                         errstr, errstr_size);
}

rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_broker (rd_kafka_AdminOptions_t *options,
                                  int32_t broker_id,
                                  char *errstr, size_t errstr_size) {
        int ibroker_id = (int)broker_id;

        return rd_kafka_confval_set_type(&options->broker,
                                         RD_KAFKA_CONFVAL_INT,
                                         &ibroker_id,
                                         errstr, errstr_size);
}

void
rd_kafka_AdminOptions_set_opaque (rd_kafka_AdminOptions_t *options,
                                  void *opaque) {
        rd_kafka_confval_set_type(&options->opaque,
                                  RD_KAFKA_CONFVAL_PTR, opaque, NULL, 0);
}


/**
 * @brief Initialize and set up defaults for AdminOptions
 */
static void rd_kafka_AdminOptions_init (rd_kafka_t *rk,
                                        rd_kafka_AdminOptions_t *options) {
        rd_kafka_confval_init_int(&options->request_timeout, "request_timeout",
                                  0, 3600*1000,
                                  rk->rk_conf.admin.request_timeout_ms);

        if (!options->for_api ||
            !rd_strcasecmp(options->for_api, "CreateTopics") ||
            !rd_strcasecmp(options->for_api, "DeleteTopics") ||
            !rd_strcasecmp(options->for_api, "CreatePartitions"))
                rd_kafka_confval_init_int(&options->operation_timeout,
                                          "operation_timeout",
                                          -1, 3600*1000, 0);
        else
                rd_kafka_confval_disable(&options->operation_timeout,
                                         "operation_timeout");

        if (!options->for_api ||
            !rd_strcasecmp(options->for_api, "CreateTopics") ||
            !rd_strcasecmp(options->for_api, "CreatePartitions") ||
            !rd_strcasecmp(options->for_api, "AlterConfigs"))
                rd_kafka_confval_init_int(&options->validate_only,
                                          "validate_only",
                                          0, 1, 0);
        else
                rd_kafka_confval_disable(&options->validate_only,
                                         "validate_only");

        if (!options->for_api ||
            !rd_strcasecmp(options->for_api, "AlterConfigs"))
                rd_kafka_confval_init_int(&options->incremental,
                                          "incremental",
                                          0, 1, 0);
        else
                rd_kafka_confval_disable(&options->incremental,
                                         "incremental");

        rd_kafka_confval_init_int(&options->broker, "broker",
                                  0, INT32_MAX, -1);
        rd_kafka_confval_init_ptr(&options->opaque, "opaque");
}


rd_kafka_AdminOptions_t *rd_kafka_AdminOptions_new (rd_kafka_t *rk,
                                                    const char *for_api) {
        rd_kafka_AdminOptions_t *options;
        static const char *valid_apis[] = {
                "CreateTopics",
                "DeleteTopics",
                "CreatePartitions",
                "AlterConfigs",
                "DescribeConfigs",
                NULL,
        };

        if (for_api) {
                const char **v = valid_apis;

                while (*v) {
                        if (!rd_strcasecmp(*v, for_api))
                                break;
                        v++;
                }
                if (!*v) /* unmatched: for_api is unsupported */
                        return NULL;
        }

        options = rd_calloc(1, sizeof(*options));

        if (for_api)
                options->for_api = rd_strdup(for_api);

        rd_kafka_AdminOptions_init(rk, options);

        return options;
}

void rd_kafka_AdminOptions_destroy (rd_kafka_AdminOptions_t *options) {
        if (options->for_api)
                rd_free(options->for_api);
        rd_free(options);
}

/**@}*/






/**
 * @name CreateTopics
 * @{
 *
 *
 *
 */



rd_kafka_NewTopic_t *
rd_kafka_NewTopic_new (const char *topic,
                       int num_partitions,
                       int replication_factor,
                       char *errstr, size_t errstr_size) {
        rd_kafka_NewTopic_t *new_topic;

        if (!topic) {
                rd_snprintf(errstr, errstr_size, "Invalid topic name");
                return NULL;
        }

        if (num_partitions < 1 || num_partitions > RD_KAFKAP_PARTITIONS_MAX) {
                rd_snprintf(errstr, errstr_size, "num_partitions out of "
                            "expected range %d..%d",
                            1, RD_KAFKAP_PARTITIONS_MAX);
                return NULL;
        }

        if (replication_factor < -1 ||
            replication_factor > RD_KAFKAP_BROKERS_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "replication_factor out of expected range %d..%d",
                            -1, RD_KAFKAP_BROKERS_MAX);
                return NULL;
        }

        new_topic = rd_calloc(1, sizeof(*new_topic));
        new_topic->topic = rd_strdup(topic);
        new_topic->num_partitions = num_partitions;
        new_topic->replication_factor = replication_factor;

        /* List of int32 lists */
        rd_list_init(&new_topic->replicas, 0, rd_list_destroy_free);
        rd_list_prealloc_elems(&new_topic->replicas, 0,
                               num_partitions, 0/*nozero*/);

        /* List of ConfigEntrys */
        rd_list_init(&new_topic->config, 0, rd_kafka_ConfigEntry_free);

        return new_topic;

}


/**
 * @brief Topic name comparator for NewTopic_t
 */
static int rd_kafka_NewTopic_cmp (const void *_a, const void *_b) {
        const rd_kafka_NewTopic_t *a = _a, *b = _b;
        return strcmp(a->topic, b->topic);
}



/**
 * @brief Allocate a new NewTopic and make a copy of \p src
 */
static rd_kafka_NewTopic_t *
rd_kafka_NewTopic_copy (const rd_kafka_NewTopic_t *src) {
        rd_kafka_NewTopic_t *dst;

        dst = rd_kafka_NewTopic_new(src->topic, src->num_partitions,
                                    src->replication_factor, NULL, 0);
        rd_assert(dst);

        rd_list_destroy(&dst->replicas); /* created in .._new() */
        rd_list_init_copy(&dst->replicas, &src->replicas);
        rd_list_copy_to(&dst->replicas, &src->replicas,
                        rd_list_copy_preallocated, NULL);

        rd_list_init_copy(&dst->config, &src->config);
        rd_list_copy_to(&dst->config, &src->config,
                        rd_kafka_ConfigEntry_list_copy, NULL);

        return dst;
}

void rd_kafka_NewTopic_destroy (rd_kafka_NewTopic_t *new_topic) {
        rd_list_destroy(&new_topic->replicas);
        rd_list_destroy(&new_topic->config);
        rd_free(new_topic->topic);
        rd_free(new_topic);
}

static void rd_kafka_NewTopic_free (void *ptr) {
        rd_kafka_NewTopic_destroy(ptr);
}

void
rd_kafka_NewTopic_destroy_array (rd_kafka_NewTopic_t **new_topics,
                                 size_t new_topic_cnt) {
        size_t i;
        for (i = 0 ; i < new_topic_cnt ; i++)
                rd_kafka_NewTopic_destroy(new_topics[i]);
}


rd_kafka_resp_err_t
rd_kafka_NewTopic_set_replica_assignment (rd_kafka_NewTopic_t *new_topic,
                                          int32_t partition,
                                          int32_t *broker_ids,
                                          size_t broker_id_cnt,
                                          char *errstr, size_t errstr_size) {
        rd_list_t *rl;
        int i;

        if (new_topic->replication_factor != -1) {
                rd_snprintf(errstr, errstr_size,
                            "Specifying a replication factor and "
                            "a replica assignment are mutually exclusive");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        /* Replica partitions must be added consecutively starting from 0. */
        if (partition != rd_list_cnt(&new_topic->replicas)) {
                rd_snprintf(errstr, errstr_size,
                            "Partitions must be added in order, "
                            "starting at 0: expecting partition %d, "
                            "not %"PRId32,
                            rd_list_cnt(&new_topic->replicas), partition);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        if (broker_id_cnt > RD_KAFKAP_BROKERS_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "Too many brokers specified "
                            "(RD_KAFKAP_BROKERS_MAX=%d)",
                            RD_KAFKAP_BROKERS_MAX);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }


        rl = rd_list_init_int32(rd_list_new(0, NULL), (int)broker_id_cnt);

        for (i = 0 ; i < (int)broker_id_cnt ; i++)
                rd_list_set_int32(rl, i, broker_ids[i]);

        rd_list_add(&new_topic->replicas, rl);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Generic constructor of ConfigEntry which is also added to \p rl
 */
static rd_kafka_resp_err_t
rd_kafka_admin_add_config0 (rd_list_t *rl,
                            const char *name, const char *value,
                            rd_kafka_AlterOperation_t operation) {
        rd_kafka_ConfigEntry_t *entry;

        if (!name)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        entry = rd_calloc(1, sizeof(*entry));
        entry->kv = rd_strtup_new(name, value);
        entry->a.operation = operation;

        rd_list_add(rl, entry);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


rd_kafka_resp_err_t
rd_kafka_NewTopic_set_config (rd_kafka_NewTopic_t *new_topic,
                              const char *name, const char *value) {
        return rd_kafka_admin_add_config0(&new_topic->config, name, value,
                                          RD_KAFKA_ALTER_OP_ADD);
}



/**
 * @brief Parse CreateTopicsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_CreateTopicsResponse_parse (rd_kafka_op_t *rko_req,
                                     rd_kafka_op_t **rko_resultp,
                                     rd_kafka_buf_t *reply,
                                     char *errstr, size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_broker_t *rkb = reply->rkbuf_rkb;
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_op_t *rko_result = NULL;
        int32_t topic_cnt;
        int i;

        if (rd_kafka_buf_ApiVersion(reply) >= 2) {
                int32_t Throttle_Time;
                rd_kafka_buf_read_i32(reply, &Throttle_Time);
                rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);
        }

        /* #topics */
        rd_kafka_buf_read_i32(reply, &topic_cnt);

        if (topic_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                        reply,
                        "Received %"PRId32" topics in response "
                        "when only %d were requested", topic_cnt,
                        rd_list_cnt(&rko_req->rko_u.admin_request.args));


        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, topic_cnt,
                     rd_kafka_topic_result_free);

        for (i = 0 ; i < (int)topic_cnt ; i++) {
                rd_kafkap_str_t ktopic;
                int16_t error_code;
                rd_kafkap_str_t error_msg = RD_KAFKAP_STR_INITIALIZER;
                char *errstr = NULL;
                rd_kafka_topic_result_t *terr;
                rd_kafka_NewTopic_t skel;
                int orig_pos;

                rd_kafka_buf_read_str(reply, &ktopic);
                rd_kafka_buf_read_i16(reply, &error_code);

                if (rd_kafka_buf_ApiVersion(reply) >= 1)
                        rd_kafka_buf_read_str(reply, &error_msg);

                /* For non-blocking CreateTopicsRequests the broker
                 * will returned REQUEST_TIMED_OUT for topics
                 * that were triggered for creation -
                 * we hide this error code from the application
                 * since the topic creation is in fact in progress. */
                if (error_code == RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT &&
                    rd_kafka_confval_get_int(&rko_req->rko_u.
                                             admin_request.options.
                                             operation_timeout) <= 0) {
                        error_code = RD_KAFKA_RESP_ERR_NO_ERROR;
                        errstr = NULL;
                }

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                errstr = (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&errstr, &error_msg);

                } else {
                        errstr = NULL;
                }

                terr = rd_kafka_topic_result_new(ktopic.str,
                                                 RD_KAFKAP_STR_LEN(&ktopic),
                                                 error_code, errstr);

                /* As a convenience to the application we insert topic result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.topic = terr->topic;
                orig_pos = rd_list_index(&rko_req->rko_u.admin_request.args,
                                         &skel, rd_kafka_NewTopic_cmp);
                if (orig_pos == -1) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned topic %.*s that was not "
                                "included in the original request",
                                RD_KAFKAP_STR_PR(&ktopic));
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned topic %.*s multiple times",
                                RD_KAFKAP_STR_PR(&ktopic));
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            terr);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

 err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "CreateTopics response protocol parse failure: %s",
                    rd_kafka_err2str(err));

        return err;
}


void rd_kafka_CreateTopics (rd_kafka_t *rk,
                            rd_kafka_NewTopic_t **new_topics,
                            size_t new_topic_cnt,
                            const rd_kafka_AdminOptions_t *options,
                            rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
                rd_kafka_CreateTopicsRequest,
                rd_kafka_CreateTopicsResponse_parse,
        };

        rko = rd_kafka_admin_request_op_new(rk,
                                            RD_KAFKA_OP_CREATETOPICS,
                                            RD_KAFKA_EVENT_CREATETOPICS_RESULT,
                                            &cbs, options, rkqu);

        rd_list_init(&rko->rko_u.admin_request.args, (int)new_topic_cnt,
                     rd_kafka_NewTopic_free);

        for (i = 0 ; i < new_topic_cnt ; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_NewTopic_copy(new_topics[i]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}


/**
 * @brief Get an array of topic results from a CreateTopics result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_topic_result_t **
rd_kafka_CreateTopics_result_topics (
        const rd_kafka_CreateTopics_result_t *result,
        size_t *cntp) {
        return rd_kafka_admin_result_ret_topics((const rd_kafka_op_t *)result,
                                                cntp);
}

/**@}*/




/**
 * @name Delete topics
 * @{
 *
 *
 *
 *
 */

rd_kafka_DeleteTopic_t *rd_kafka_DeleteTopic_new (const char *topic) {
        size_t tsize = strlen(topic) + 1;
        rd_kafka_DeleteTopic_t *del_topic;

        /* Single allocation */
        del_topic = rd_malloc(sizeof(*del_topic) + tsize);
        del_topic->topic = del_topic->data;
        memcpy(del_topic->topic, topic, tsize);

        return del_topic;
}

void rd_kafka_DeleteTopic_destroy (rd_kafka_DeleteTopic_t *del_topic) {
        rd_free(del_topic);
}

static void rd_kafka_DeleteTopic_free (void *ptr) {
        rd_kafka_DeleteTopic_destroy(ptr);
}


void rd_kafka_DeleteTopic_destroy_array (rd_kafka_DeleteTopic_t **del_topics,
                                         size_t del_topic_cnt) {
        size_t i;
        for (i = 0 ; i < del_topic_cnt ; i++)
                rd_kafka_DeleteTopic_destroy(del_topics[i]);
}


/**
 * @brief Topic name comparator for DeleteTopic_t
 */
static int rd_kafka_DeleteTopic_cmp (const void *_a, const void *_b) {
        const rd_kafka_DeleteTopic_t *a = _a, *b = _b;
        return strcmp(a->topic, b->topic);
}

/**
 * @brief Allocate a new DeleteTopic and make a copy of \p src
 */
static rd_kafka_DeleteTopic_t *
rd_kafka_DeleteTopic_copy (const rd_kafka_DeleteTopic_t *src) {
        return rd_kafka_DeleteTopic_new(src->topic);
}







/**
 * @brief Parse DeleteTopicsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DeleteTopicsResponse_parse (rd_kafka_op_t *rko_req,
                                     rd_kafka_op_t **rko_resultp,
                                     rd_kafka_buf_t *reply,
                                     char *errstr, size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_broker_t *rkb = reply->rkbuf_rkb;
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_op_t *rko_result = NULL;
        int32_t topic_cnt;
        int i;

        if (rd_kafka_buf_ApiVersion(reply) >= 1) {
                int32_t Throttle_Time;
                rd_kafka_buf_read_i32(reply, &Throttle_Time);
                rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);
        }

        /* #topics */
        rd_kafka_buf_read_i32(reply, &topic_cnt);

        if (topic_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                        reply,
                        "Received %"PRId32" topics in response "
                        "when only %d were requested", topic_cnt,
                        rd_list_cnt(&rko_req->rko_u.admin_request.args));

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, topic_cnt,
                     rd_kafka_topic_result_free);

        for (i = 0 ; i < (int)topic_cnt ; i++) {
                rd_kafkap_str_t ktopic;
                int16_t error_code;
                rd_kafka_topic_result_t *terr;
                rd_kafka_NewTopic_t skel;
                int orig_pos;

                rd_kafka_buf_read_str(reply, &ktopic);
                rd_kafka_buf_read_i16(reply, &error_code);

                /* For non-blocking DeleteTopicsRequests the broker
                 * will returned REQUEST_TIMED_OUT for topics
                 * that were triggered for creation -
                 * we hide this error code from the application
                 * since the topic creation is in fact in progress. */
                if (error_code == RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT &&
                    rd_kafka_confval_get_int(&rko_req->rko_u.
                                             admin_request.options.
                                             operation_timeout) <= 0) {
                        error_code = RD_KAFKA_RESP_ERR_NO_ERROR;
                }

                terr = rd_kafka_topic_result_new(ktopic.str,
                                                 RD_KAFKAP_STR_LEN(&ktopic),
                                                 error_code,
                                                 error_code ?
                                                 rd_kafka_err2str(error_code) :
                                                 NULL);

                /* As a convenience to the application we insert topic result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.topic = terr->topic;
                orig_pos = rd_list_index(&rko_req->rko_u.admin_request.args,
                                         &skel, rd_kafka_DeleteTopic_cmp);
                if (orig_pos == -1) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned topic %.*s that was not "
                                "included in the original request",
                                RD_KAFKAP_STR_PR(&ktopic));
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned topic %.*s multiple times",
                                RD_KAFKAP_STR_PR(&ktopic));
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            terr);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

 err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "DeleteTopics response protocol parse failure: %s",
                    rd_kafka_err2str(err));

        return err;
}






void rd_kafka_DeleteTopics (rd_kafka_t *rk,
                            rd_kafka_DeleteTopic_t **del_topics,
                            size_t del_topic_cnt,
                            const rd_kafka_AdminOptions_t *options,
                            rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
                rd_kafka_DeleteTopicsRequest,
                rd_kafka_DeleteTopicsResponse_parse,
        };

        rko = rd_kafka_admin_request_op_new(rk,
                                            RD_KAFKA_OP_DELETETOPICS,
                                            RD_KAFKA_EVENT_DELETETOPICS_RESULT,
                                            &cbs, options, rkqu);

        rd_list_init(&rko->rko_u.admin_request.args, (int)del_topic_cnt,
                     rd_kafka_DeleteTopic_free);

        for (i = 0 ; i < del_topic_cnt ; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_DeleteTopic_copy(del_topics[i]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}


/**
 * @brief Get an array of topic results from a DeleteTopics result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_topic_result_t **
rd_kafka_DeleteTopics_result_topics (
        const rd_kafka_DeleteTopics_result_t *result,
        size_t *cntp) {
        return rd_kafka_admin_result_ret_topics((const rd_kafka_op_t *)result,
                                                cntp);
}




/**
 * @name Create partitions
 * @{
 *
 *
 *
 *
 */

rd_kafka_NewPartitions_t *rd_kafka_NewPartitions_new (const char *topic,
                                                      size_t new_total_cnt,
                                                      char *errstr,
                                                      size_t errstr_size) {
        size_t tsize = strlen(topic) + 1;
        rd_kafka_NewPartitions_t *newps;

        if (new_total_cnt < 1 || new_total_cnt > RD_KAFKAP_PARTITIONS_MAX) {
                rd_snprintf(errstr, errstr_size, "new_total_cnt out of "
                            "expected range %d..%d",
                            1, RD_KAFKAP_PARTITIONS_MAX);
                return NULL;
        }

        /* Single allocation */
        newps = rd_malloc(sizeof(*newps) + tsize);
        newps->total_cnt = new_total_cnt;
        newps->topic = newps->data;
        memcpy(newps->topic, topic, tsize);

        /* List of int32 lists */
        rd_list_init(&newps->replicas, 0, rd_list_destroy_free);
        rd_list_prealloc_elems(&newps->replicas, 0, new_total_cnt, 0/*nozero*/);

        return newps;
}

/**
 * @brief Topic name comparator for NewPartitions_t
 */
static int rd_kafka_NewPartitions_cmp (const void *_a, const void *_b) {
        const rd_kafka_NewPartitions_t *a = _a, *b = _b;
        return strcmp(a->topic, b->topic);
}


/**
 * @brief Allocate a new CreatePartitions and make a copy of \p src
 */
static rd_kafka_NewPartitions_t *
rd_kafka_NewPartitions_copy (const rd_kafka_NewPartitions_t *src) {
        rd_kafka_NewPartitions_t *dst;

        dst = rd_kafka_NewPartitions_new(src->topic, src->total_cnt, NULL, 0);

        rd_list_destroy(&dst->replicas); /* created in .._new() */
        rd_list_init_copy(&dst->replicas, &src->replicas);
        rd_list_copy_to(&dst->replicas, &src->replicas,
                        rd_list_copy_preallocated, NULL);

        return dst;
}

void rd_kafka_NewPartitions_destroy (rd_kafka_NewPartitions_t *newps) {
        rd_list_destroy(&newps->replicas);
        rd_free(newps);
}

static void rd_kafka_NewPartitions_free (void *ptr) {
        rd_kafka_NewPartitions_destroy(ptr);
}


void rd_kafka_NewPartitions_destroy_array (rd_kafka_NewPartitions_t **newps,
                                         size_t newps_cnt) {
        size_t i;
        for (i = 0 ; i < newps_cnt ; i++)
                rd_kafka_NewPartitions_destroy(newps[i]);
}





rd_kafka_resp_err_t
rd_kafka_NewPartitions_set_replica_assignment (rd_kafka_NewPartitions_t *newp,
                                               int32_t new_partition_idx,
                                               int32_t *broker_ids,
                                               size_t broker_id_cnt,
                                               char *errstr,
                                               size_t errstr_size) {
        rd_list_t *rl;
        int i;

        /* Replica partitions must be added consecutively starting from 0. */
        if (new_partition_idx != rd_list_cnt(&newp->replicas)) {
                rd_snprintf(errstr, errstr_size,
                            "Partitions must be added in order, "
                            "starting at 0: expecting partition "
                            "index %d, not %"PRId32,
                            rd_list_cnt(&newp->replicas), new_partition_idx);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        if (broker_id_cnt > RD_KAFKAP_BROKERS_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "Too many brokers specified "
                            "(RD_KAFKAP_BROKERS_MAX=%d)",
                            RD_KAFKAP_BROKERS_MAX);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        rl = rd_list_init_int32(rd_list_new(0, NULL), (int)broker_id_cnt);

        for (i = 0 ; i < (int)broker_id_cnt ; i++)
                rd_list_set_int32(rl, i, broker_ids[i]);

        rd_list_add(&newp->replicas, rl);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}




/**
 * @brief Parse CreatePartitionsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_CreatePartitionsResponse_parse (rd_kafka_op_t *rko_req,
                                         rd_kafka_op_t **rko_resultp,
                                         rd_kafka_buf_t *reply,
                                         char *errstr,
                                         size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_broker_t *rkb = reply->rkbuf_rkb;
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_op_t *rko_result = NULL;
        int32_t topic_cnt;
        int i;
        int32_t Throttle_Time;

        rd_kafka_buf_read_i32(reply, &Throttle_Time);
        rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);

        /* #topics */
        rd_kafka_buf_read_i32(reply, &topic_cnt);

        if (topic_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                        reply,
                        "Received %"PRId32" topics in response "
                        "when only %d were requested", topic_cnt,
                        rd_list_cnt(&rko_req->rko_u.admin_request.args));

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, topic_cnt,
                     rd_kafka_topic_result_free);

        for (i = 0 ; i < (int)topic_cnt ; i++) {
                rd_kafkap_str_t ktopic;
                int16_t error_code;
                char *errstr = NULL;
                rd_kafka_topic_result_t *terr;
                rd_kafka_NewTopic_t skel;
                rd_kafkap_str_t error_msg;
                int orig_pos;

                rd_kafka_buf_read_str(reply, &ktopic);
                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &error_msg);

                /* For non-blocking CreatePartitionsRequests the broker
                 * will returned REQUEST_TIMED_OUT for topics
                 * that were triggered for creation -
                 * we hide this error code from the application
                 * since the topic creation is in fact in progress. */
                if (error_code == RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT &&
                    rd_kafka_confval_get_int(&rko_req->rko_u.
                                             admin_request.options.
                                             operation_timeout) <= 0) {
                        error_code = RD_KAFKA_RESP_ERR_NO_ERROR;
                }

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                errstr = (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&errstr, &error_msg);
                }


                terr = rd_kafka_topic_result_new(ktopic.str,
                                                 RD_KAFKAP_STR_LEN(&ktopic),
                                                 error_code,
                                                 error_code ?
                                                 rd_kafka_err2str(error_code) :
                                                 NULL);

                /* As a convenience to the application we insert topic result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.topic = terr->topic;
                orig_pos = rd_list_index(&rko_req->rko_u.admin_request.args,
                                         &skel, rd_kafka_NewPartitions_cmp);
                if (orig_pos == -1) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned topic %.*s that was not "
                                "included in the original request",
                                RD_KAFKAP_STR_PR(&ktopic));
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned topic %.*s multiple times",
                                RD_KAFKAP_STR_PR(&ktopic));
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            terr);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

 err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "CreatePartitions response protocol parse failure: %s",
                    rd_kafka_err2str(err));

        return err;
}







void rd_kafka_CreatePartitions (rd_kafka_t *rk,
                                rd_kafka_NewPartitions_t **newps,
                                size_t newps_cnt,
                                const rd_kafka_AdminOptions_t *options,
                                rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
                rd_kafka_CreatePartitionsRequest,
                rd_kafka_CreatePartitionsResponse_parse,
        };

        rko = rd_kafka_admin_request_op_new(
                rk,
                RD_KAFKA_OP_CREATEPARTITIONS,
                RD_KAFKA_EVENT_CREATEPARTITIONS_RESULT,
                &cbs, options, rkqu);

        rd_list_init(&rko->rko_u.admin_request.args, (int)newps_cnt,
                     rd_kafka_NewPartitions_free);

        for (i = 0 ; i < newps_cnt ; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_NewPartitions_copy(newps[i]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}


/**
 * @brief Get an array of topic results from a CreatePartitions result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_topic_result_t **
rd_kafka_CreatePartitions_result_topics (
        const rd_kafka_CreatePartitions_result_t *result,
        size_t *cntp) {
        return rd_kafka_admin_result_ret_topics((const rd_kafka_op_t *)result,
                                                cntp);
}

/**@}*/




/**
 * @name ConfigEntry
 * @{
 *
 *
 *
 */

static void rd_kafka_ConfigEntry_destroy (rd_kafka_ConfigEntry_t *entry) {
        rd_strtup_destroy(entry->kv);
        rd_list_destroy(&entry->synonyms);
        rd_free(entry);
}


static void rd_kafka_ConfigEntry_free (void *ptr) {
        rd_kafka_ConfigEntry_destroy((rd_kafka_ConfigEntry_t *)ptr);
}


/**
 * @brief Create new ConfigEntry
 *
 * @param name Config entry name
 * @param name_len Length of name, or -1 to use strlen()
 * @param value Config entry value, or NULL
 * @param value_len Length of value, or -1 to use strlen()
 */
static rd_kafka_ConfigEntry_t *
rd_kafka_ConfigEntry_new0 (const char *name, size_t name_len,
                           const char *value, size_t value_len) {
        rd_kafka_ConfigEntry_t *entry;

        if (!name)
                return NULL;

        entry = rd_calloc(1, sizeof(*entry));
        entry->kv = rd_strtup_new0(name, name_len, value, value_len);

        rd_list_init(&entry->synonyms, 0, rd_kafka_ConfigEntry_free);

        entry->a.source = RD_KAFKA_CONFIG_SOURCE_UNKNOWN_CONFIG;

        return entry;
}

/**
 * @sa rd_kafka_ConfigEntry_new0
 */
static rd_kafka_ConfigEntry_t *
rd_kafka_ConfigEntry_new (const char *name, const char *value) {
        return rd_kafka_ConfigEntry_new0(name, -1, value, -1);
}




/**
 * @brief Allocate a new AlterConfigs and make a copy of \p src
 */
static rd_kafka_ConfigEntry_t *
rd_kafka_ConfigEntry_copy (const rd_kafka_ConfigEntry_t *src) {
        rd_kafka_ConfigEntry_t *dst;

        dst = rd_kafka_ConfigEntry_new(src->kv->name, src->kv->value);
        dst->a = src->a;

        rd_list_destroy(&dst->synonyms); /* created in .._new() */
        rd_list_init_copy(&dst->synonyms, &src->synonyms);
        rd_list_copy_to(&dst->synonyms, &src->synonyms,
                        rd_kafka_ConfigEntry_list_copy, NULL);

        return dst;
}

static void *rd_kafka_ConfigEntry_list_copy (const void *src, void *opaque) {
        return rd_kafka_ConfigEntry_copy((const rd_kafka_ConfigEntry_t *)src);
}


const char *rd_kafka_ConfigEntry_name (const rd_kafka_ConfigEntry_t *entry) {
        return entry->kv->name;
}

const char *
rd_kafka_ConfigEntry_value (const rd_kafka_ConfigEntry_t *entry) {
        return entry->kv->value;
}

rd_kafka_ConfigSource_t
rd_kafka_ConfigEntry_source (const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.source;
}

int rd_kafka_ConfigEntry_is_read_only (const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.is_readonly;
}

int rd_kafka_ConfigEntry_is_default (const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.is_default;
}

int rd_kafka_ConfigEntry_is_sensitive (const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.is_sensitive;
}

int rd_kafka_ConfigEntry_is_synonym (const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.is_synonym;
}

const rd_kafka_ConfigEntry_t **
rd_kafka_ConfigEntry_synonyms (const rd_kafka_ConfigEntry_t *entry,
                               size_t *cntp) {
        *cntp = rd_list_cnt(&entry->synonyms);
        if (!*cntp)
                return NULL;
        return (const rd_kafka_ConfigEntry_t **)entry->synonyms.rl_elems;

}


/**@}*/



/**
 * @name ConfigSource
 * @{
 *
 *
 *
 */

const char *
rd_kafka_ConfigSource_name (rd_kafka_ConfigSource_t confsource) {
        static const char *names[] = {
                "UNKNOWN_CONFIG",
                "DYNAMIC_TOPIC_CONFIG",
                "DYNAMIC_BROKER_CONFIG",
                "DYNAMIC_DEFAULT_BROKER_CONFIG",
                "STATIC_BROKER_CONFIG",
                "DEFAULT_CONFIG",
        };

        if ((unsigned int)confsource >=
            (unsigned int)RD_KAFKA_CONFIG_SOURCE__CNT)
                return "UNSUPPORTED";

        return names[confsource];
}

/**@}*/



/**
 * @name ConfigResource
 * @{
 *
 *
 *
 */

const char *
rd_kafka_ResourceType_name (rd_kafka_ResourceType_t restype) {
        static const char *names[] = {
                "UNKNOWN",
                "ANY",
                "TOPIC",
                "GROUP",
                "BROKER",
        };

        if ((unsigned int)restype >=
            (unsigned int)RD_KAFKA_RESOURCE__CNT)
                return "UNSUPPORTED";

        return names[restype];
}


rd_kafka_ConfigResource_t *
rd_kafka_ConfigResource_new (rd_kafka_ResourceType_t restype,
                             const char *resname) {
        rd_kafka_ConfigResource_t *config;
        size_t namesz = resname ? strlen(resname) : 0;

        if (!namesz || (int)restype < 0)
                return NULL;

        config = rd_calloc(1, sizeof(*config) + namesz + 1);
        config->name = config->data;
        memcpy(config->name, resname, namesz + 1);
        config->restype = restype;

        rd_list_init(&config->config, 8, rd_kafka_ConfigEntry_free);

        return config;
}

void rd_kafka_ConfigResource_destroy (rd_kafka_ConfigResource_t *config) {
        rd_list_destroy(&config->config);
        if (config->errstr)
                rd_free(config->errstr);
        rd_free(config);
}

static void rd_kafka_ConfigResource_free (void *ptr) {
        rd_kafka_ConfigResource_destroy((rd_kafka_ConfigResource_t *)ptr);
}


void rd_kafka_ConfigResource_destroy_array (rd_kafka_ConfigResource_t **config,
                                            size_t config_cnt) {
        size_t i;
        for (i = 0 ; i < config_cnt ; i++)
                rd_kafka_ConfigResource_destroy(config[i]);
}


/**
 * @brief Type and name comparator for ConfigResource_t
 */
static int rd_kafka_ConfigResource_cmp (const void *_a, const void *_b) {
        const rd_kafka_ConfigResource_t *a = _a, *b = _b;
        if (a->restype != b->restype)
                return a->restype - b->restype;
        return strcmp(a->name, b->name);
}

/**
 * @brief Allocate a new AlterConfigs and make a copy of \p src
 */
static rd_kafka_ConfigResource_t *
rd_kafka_ConfigResource_copy (const rd_kafka_ConfigResource_t *src) {
        rd_kafka_ConfigResource_t *dst;

        dst = rd_kafka_ConfigResource_new(src->restype, src->name);

        rd_list_destroy(&dst->config); /* created in .._new() */
        rd_list_init_copy(&dst->config, &src->config);
        rd_list_copy_to(&dst->config, &src->config,
                        rd_kafka_ConfigEntry_list_copy, NULL);

        return dst;
}


static void
rd_kafka_ConfigResource_add_ConfigEntry (rd_kafka_ConfigResource_t *config,
                                         rd_kafka_ConfigEntry_t *entry) {
        rd_list_add(&config->config, entry);
}


rd_kafka_resp_err_t
rd_kafka_ConfigResource_add_config (rd_kafka_ConfigResource_t *config,
                                    const char *name, const char *value) {
        if (!name || !*name || !value)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        return rd_kafka_admin_add_config0(&config->config, name, value,
                                          RD_KAFKA_ALTER_OP_ADD);
}

rd_kafka_resp_err_t
rd_kafka_ConfigResource_set_config (rd_kafka_ConfigResource_t *config,
                                    const char *name, const char *value) {
        if (!name || !*name || !value)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        return rd_kafka_admin_add_config0(&config->config, name, value,
                                          RD_KAFKA_ALTER_OP_SET);
}

rd_kafka_resp_err_t
rd_kafka_ConfigResource_delete_config (rd_kafka_ConfigResource_t *config,
                                       const char *name) {
        if (!name || !*name)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        return rd_kafka_admin_add_config0(&config->config, name, NULL,
                                          RD_KAFKA_ALTER_OP_DELETE);
}


const rd_kafka_ConfigEntry_t **
rd_kafka_ConfigResource_configs (const rd_kafka_ConfigResource_t *config,
                                 size_t *cntp) {
        *cntp = rd_list_cnt(&config->config);
        if (!*cntp)
                return NULL;
        return (const rd_kafka_ConfigEntry_t **)config->config.rl_elems;
}




rd_kafka_ResourceType_t
rd_kafka_ConfigResource_type (const rd_kafka_ConfigResource_t *config) {
        return config->restype;
}

const char *
rd_kafka_ConfigResource_name (const rd_kafka_ConfigResource_t *config) {
        return config->name;
}

rd_kafka_resp_err_t
rd_kafka_ConfigResource_error (const rd_kafka_ConfigResource_t *config) {
        return config->err;
}

const char *
rd_kafka_ConfigResource_error_string (const rd_kafka_ConfigResource_t *config) {
        if (!config->err)
                return NULL;
        if (config->errstr)
                return config->errstr;
        return rd_kafka_err2str(config->err);
}


/**
 * @brief Look in the provided ConfigResource_t* list for a resource of
 *        type BROKER and set its broker id in \p broker_id, returning
 *        RD_KAFKA_RESP_ERR_NO_ERROR.
 *
 *        If multiple BROKER resources are found RD_KAFKA_RESP_ERR__CONFLICT
 *        is returned and an error string is written to errstr.
 *
 *        If no BROKER resources are found RD_KAFKA_RESP_ERR_NO_ERROR
 *        is returned and \p broker_idp is set to -1.
 */
static rd_kafka_resp_err_t
rd_kafka_ConfigResource_get_single_broker_id (const rd_list_t *configs,
                                              int32_t *broker_idp,
                                              char *errstr,
                                              size_t errstr_size) {
        const rd_kafka_ConfigResource_t *config;
        int i;
        int32_t broker_id = -1;

        RD_LIST_FOREACH(config, configs, i) {
                char *endptr;
                long int r;

                if (config->restype != RD_KAFKA_RESOURCE_BROKER)
                        continue;

                if (broker_id != -1) {
                        rd_snprintf(errstr, errstr_size,
                                    "Only one ConfigResource of type BROKER "
                                    "is allowed per call");
                        return RD_KAFKA_RESP_ERR__CONFLICT;
                }

                /* Convert string broker-id to int32 */
                r = (int32_t)strtol(config->name, &endptr, 10);
                if (r == LONG_MIN || r == LONG_MAX || config->name == endptr ||
                    r < 0) {
                        rd_snprintf(errstr, errstr_size,
                                    "Expected an int32 broker_id for "
                                    "ConfigResource(type=BROKER, name=%s)",
                                    config->name);
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;
                }

                broker_id = r;

                /* Keep scanning to make sure there are no duplicate
                 * BROKER resources. */
        }

        *broker_idp = broker_id;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**@}*/



/**
 * @name AlterConfigs
 * @{
 *
 *
 *
 */



/**
 * @brief Parse AlterConfigsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_AlterConfigsResponse_parse (rd_kafka_op_t *rko_req,
                                     rd_kafka_op_t **rko_resultp,
                                     rd_kafka_buf_t *reply,
                                     char *errstr, size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_broker_t *rkb = reply->rkbuf_rkb;
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_op_t *rko_result = NULL;
        int32_t res_cnt;
        int i;
        int32_t Throttle_Time;

        rd_kafka_buf_read_i32(reply, &Throttle_Time);
        rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);

        rd_kafka_buf_read_i32(reply, &res_cnt);

        if (res_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args)) {
                rd_snprintf(errstr, errstr_size,
                            "Received %"PRId32" ConfigResources in response "
                            "when only %d were requested", res_cnt,
                            rd_list_cnt(&rko_req->rko_u.admin_request.args));
                return RD_KAFKA_RESP_ERR__BAD_MSG;
        }

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, res_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0 ; i < (int)res_cnt ; i++) {
                int16_t error_code;
                rd_kafkap_str_t error_msg;
                int8_t res_type;
                rd_kafkap_str_t kres_name;
                char *res_name;
                char *errstr = NULL;
                rd_kafka_ConfigResource_t *config;
                rd_kafka_ConfigResource_t skel;
                int orig_pos;

                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &error_msg);
                rd_kafka_buf_read_i8(reply, &res_type);
                rd_kafka_buf_read_str(reply, &kres_name);
                RD_KAFKAP_STR_DUPA(&res_name, &kres_name);

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                errstr = (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&errstr, &error_msg);
                }

                config = rd_kafka_ConfigResource_new(res_type, res_name);
                if (!config) {
                        rd_kafka_log(rko_req->rko_rk, LOG_ERR,
                                     "ADMIN", "AlterConfigs returned "
                                     "unsupported ConfigResource #%d with "
                                     "type %d and name \"%s\": ignoring",
                                     i, res_type, res_name);
                        continue;
                }

                config->err = error_code;
                if (errstr)
                        config->errstr = rd_strdup(errstr);

                /* As a convenience to the application we insert result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.restype = config->restype;
                skel.name = config->name;
                orig_pos = rd_list_index(&rko_req->rko_u.admin_request.args,
                                         &skel, rd_kafka_ConfigResource_cmp);
                if (orig_pos == -1) {
                        rd_kafka_ConfigResource_destroy(config);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned ConfigResource %d,%s "
                                "that was not "
                                "included in the original request",
                                res_type, res_name);
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_ConfigResource_destroy(config);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned ConfigResource %d,%s "
                                "multiple times",
                                res_type, res_name);
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            config);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

 err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "AlterConfigs response protocol parse failure: %s",
                    rd_kafka_err2str(err));

        return err;
}




void rd_kafka_AlterConfigs (rd_kafka_t *rk,
                            rd_kafka_ConfigResource_t **configs,
                            size_t config_cnt,
                            const rd_kafka_AdminOptions_t *options,
                            rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        rd_kafka_resp_err_t err;
        char errstr[256];
        static const struct rd_kafka_admin_worker_cbs cbs = {
                rd_kafka_AlterConfigsRequest,
                rd_kafka_AlterConfigsResponse_parse,
        };

        rko = rd_kafka_admin_request_op_new(
                rk,
                RD_KAFKA_OP_ALTERCONFIGS,
                RD_KAFKA_EVENT_ALTERCONFIGS_RESULT,
                &cbs, options, rkqu);

        rd_list_init(&rko->rko_u.admin_request.args, (int)config_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0 ; i < config_cnt ; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_ConfigResource_copy(configs[i]));

        /* If there's a BROKER resource in the list we need to
         * speak directly to that broker rather than the controller.
         *
         * Multiple BROKER resources are not allowed.
         */
        err = rd_kafka_ConfigResource_get_single_broker_id(
                &rko->rko_u.admin_request.args,
                &rko->rko_u.admin_request.broker_id,
                errstr, sizeof(errstr));
        if (err) {
                rd_kafka_admin_result_fail(rko, err, "%s", errstr);
                rd_kafka_admin_common_worker_destroy(rk, rko);
                return;
        }

        rd_kafka_q_enq(rk->rk_ops, rko);
}


const rd_kafka_ConfigResource_t **
rd_kafka_AlterConfigs_result_resources (
        const rd_kafka_AlterConfigs_result_t *result,
        size_t *cntp) {
        return rd_kafka_admin_result_ret_resources(
                (const rd_kafka_op_t *)result, cntp);
}

/**@}*/




/**
 * @name DescribeConfigs
 * @{
 *
 *
 *
 */


/**
 * @brief Parse DescribeConfigsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DescribeConfigsResponse_parse (rd_kafka_op_t *rko_req,
                                        rd_kafka_op_t **rko_resultp,
                                        rd_kafka_buf_t *reply,
                                        char *errstr, size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_broker_t *rkb = reply->rkbuf_rkb;
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_op_t *rko_result = NULL;
        int32_t res_cnt;
        int i;
        int32_t Throttle_Time;
        rd_kafka_ConfigResource_t *config = NULL;
        rd_kafka_ConfigEntry_t *entry = NULL;

        rd_kafka_buf_read_i32(reply, &Throttle_Time);
        rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);

        /* #resources */
        rd_kafka_buf_read_i32(reply, &res_cnt);

        if (res_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                        reply,
                        "Received %"PRId32" ConfigResources in response "
                        "when only %d were requested", res_cnt,
                        rd_list_cnt(&rko_req->rko_u.admin_request.args));

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, res_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0 ; i < (int)res_cnt ; i++) {
                int16_t error_code;
                rd_kafkap_str_t error_msg;
                int8_t res_type;
                rd_kafkap_str_t kres_name;
                char *res_name;
                char *errstr = NULL;
                rd_kafka_ConfigResource_t skel;
                int orig_pos;
                int32_t entry_cnt;
                int ci;

                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &error_msg);
                rd_kafka_buf_read_i8(reply, &res_type);
                rd_kafka_buf_read_str(reply, &kres_name);
                RD_KAFKAP_STR_DUPA(&res_name, &kres_name);

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                errstr = (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&errstr, &error_msg);
                }

                config = rd_kafka_ConfigResource_new(res_type, res_name);
                if (!config) {
                        rd_kafka_log(rko_req->rko_rk, LOG_ERR,
                                     "ADMIN", "DescribeConfigs returned "
                                     "unsupported ConfigResource #%d with "
                                     "type %d and name \"%s\": ignoring",
                                     i, res_type, res_name);
                        continue;
                }

                config->err = error_code;
                if (errstr)
                        config->errstr = rd_strdup(errstr);

                /* #config_entries */
                rd_kafka_buf_read_i32(reply, &entry_cnt);

                for (ci = 0 ; ci < (int)entry_cnt ; ci++) {
                        rd_kafkap_str_t config_name, config_value;
                        int32_t syn_cnt;
                        int si;

                        rd_kafka_buf_read_str(reply, &config_name);
                        rd_kafka_buf_read_str(reply, &config_value);

                        entry = rd_kafka_ConfigEntry_new0(
                                config_name.str,
                                RD_KAFKAP_STR_LEN(&config_name),
                                config_value.str,
                                RD_KAFKAP_STR_LEN(&config_value));

                        rd_kafka_buf_read_bool(reply, &entry->a.is_readonly);

                        /* ApiVersion 0 has is_default field, while
                         * ApiVersion 1 has source field.
                         * Convert between the two so they look the same
                         * to the caller. */
                        if (rd_kafka_buf_ApiVersion(reply) == 0) {
                                rd_kafka_buf_read_bool(reply,
                                                       &entry->a.is_default);
                                if (entry->a.is_default)
                                        entry->a.source =
                                                RD_KAFKA_CONFIG_SOURCE_DEFAULT_CONFIG;
                        } else {
                                int8_t config_source;
                                rd_kafka_buf_read_i8(reply, &config_source);
                                entry->a.source = config_source;

                                if (entry->a.source ==
                                    RD_KAFKA_CONFIG_SOURCE_DEFAULT_CONFIG)
                                        entry->a.is_default = 1;

                        }

                        rd_kafka_buf_read_bool(reply, &entry->a.is_sensitive);


                        if (rd_kafka_buf_ApiVersion(reply) == 1) {
                                /* #config_synonyms (ApiVersion 1) */
                                rd_kafka_buf_read_i32(reply, &syn_cnt);

                                if (syn_cnt > 100000)
                                        rd_kafka_buf_parse_fail(
                                                reply,
                                                "Broker returned %"PRId32
                                                " config synonyms for "
                                                "ConfigResource %d,%s: "
                                                "limit is 100000",
                                                syn_cnt,
                                                config->restype,
                                                config->name);

                                if (syn_cnt > 0)
                                        rd_list_grow(&entry->synonyms, syn_cnt);

                        } else {
                                /* No synonyms in ApiVersion 0 */
                                syn_cnt = 0;
                        }



                        /* Read synonyms (ApiVersion 1) */
                        for (si = 0 ; si < (int)syn_cnt ; si++) {
                                rd_kafkap_str_t syn_name, syn_value;
                                int8_t syn_source;
                                rd_kafka_ConfigEntry_t *syn_entry;

                                rd_kafka_buf_read_str(reply, &syn_name);
                                rd_kafka_buf_read_str(reply, &syn_value);
                                rd_kafka_buf_read_i8(reply, &syn_source);

                                syn_entry = rd_kafka_ConfigEntry_new0(
                                        syn_name.str,
                                        RD_KAFKAP_STR_LEN(&syn_name),
                                        syn_value.str,
                                        RD_KAFKAP_STR_LEN(&syn_value));
                                if (!syn_entry)
                                        rd_kafka_buf_parse_fail(
                                                reply,
                                                "Broker returned invalid "
                                                "synonym #%d "
                                                "for ConfigEntry #%d (%s) "
                                                "and ConfigResource %d,%s: "
                                                "syn_name.len %d, "
                                                "syn_value.len %d",
                                                si, ci, entry->kv->name,
                                                config->restype, config->name,
                                                (int)syn_name.len,
                                                (int)syn_value.len);

                                syn_entry->a.source = syn_source;
                                syn_entry->a.is_synonym = 1;

                                rd_list_add(&entry->synonyms, syn_entry);
                        }

                        rd_kafka_ConfigResource_add_ConfigEntry(
                                config, entry);
                        entry = NULL;
                }

                /* As a convenience to the application we insert result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.restype = config->restype;
                skel.name = config->name;
                orig_pos = rd_list_index(&rko_req->rko_u.admin_request.args,
                                         &skel, rd_kafka_ConfigResource_cmp);
                if (orig_pos == -1) {
                        rd_kafka_ConfigResource_destroy(config);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned ConfigResource %d,%s "
                                "that was not "
                                "included in the original request",
                                res_type, res_name);
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_ConfigResource_destroy(config);
                        rd_kafka_buf_parse_fail(
                                reply,
                                "Broker returned ConfigResource %d,%s "
                                "multiple times",
                                res_type, res_name);
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            config);
                config = NULL;
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

 err_parse:
        if (entry)
                rd_kafka_ConfigEntry_destroy(entry);
        if (config)
                rd_kafka_ConfigResource_destroy(config);

        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "DescribeConfigs response protocol parse failure: %s",
                    rd_kafka_err2str(err));

        return err;
}



void rd_kafka_DescribeConfigs (rd_kafka_t *rk,
                               rd_kafka_ConfigResource_t **configs,
                               size_t config_cnt,
                               const rd_kafka_AdminOptions_t *options,
                               rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        rd_kafka_resp_err_t err;
        char errstr[256];
        static const struct rd_kafka_admin_worker_cbs cbs = {
                rd_kafka_DescribeConfigsRequest,
                rd_kafka_DescribeConfigsResponse_parse,
        };

        rko = rd_kafka_admin_request_op_new(
                rk,
                RD_KAFKA_OP_DESCRIBECONFIGS,
                RD_KAFKA_EVENT_DESCRIBECONFIGS_RESULT,
                &cbs, options, rkqu);

        rd_list_init(&rko->rko_u.admin_request.args, (int)config_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0 ; i < config_cnt ; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_ConfigResource_copy(configs[i]));

        /* If there's a BROKER resource in the list we need to
         * speak directly to that broker rather than the controller.
         *
         * Multiple BROKER resources are not allowed.
         */
        err = rd_kafka_ConfigResource_get_single_broker_id(
                &rko->rko_u.admin_request.args,
                &rko->rko_u.admin_request.broker_id,
                errstr, sizeof(errstr));
        if (err) {
                rd_kafka_admin_result_fail(rko, err, "%s", errstr);
                rd_kafka_admin_common_worker_destroy(rk, rko);
                return;
        }

        rd_kafka_q_enq(rk->rk_ops, rko);
}




const rd_kafka_ConfigResource_t **
rd_kafka_DescribeConfigs_result_resources (
        const rd_kafka_DescribeConfigs_result_t *result,
        size_t *cntp) {
        return rd_kafka_admin_result_ret_resources(
                (const rd_kafka_op_t *)result, cntp);
}

/**@}*/
