/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file unlang/xlat.c
 * @brief Integration between the unlang interpreter and xlats
 *
 * @copyright 2018 The FreeRADIUS server project
 * @copyright 2018 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>

#include <ctype.h>
#include <freeradius-devel/unlang/mod_action.h>
#include <freeradius-devel/unlang/xlat_priv.h>
#include <freeradius-devel/util/debug.h>
#include "unlang_priv.h"	/* Fixme - Should create a proper semi-public interface for the interpret */

/** State of an xlat expansion
 *
 * State of one level of nesting within an xlat expansion.
 */
typedef struct {
	TALLOC_CTX		*ctx;				//!< to allocate boxes and values in.
	TALLOC_CTX		*event_ctx;			//!< for temporary events
	xlat_exp_head_t const	*head;		       		//!< of the xlat list
	xlat_exp_t const	*exp;				//!< current one we're evaluating
	fr_dcursor_t		values;				//!< Values aggregated so far.

	rindent_t		indent;				//!< indentation

	void			*env_data;			//!< Expanded per call environment tmpls.
	/*
	 *	For func and alternate
	 */
	fr_value_box_list_t	out;				//!< Head of the result of a nested
								///< expansion.
	xlat_func_t		resume;				//!< called on resume
	xlat_func_signal_t	signal;				//!< called on signal
	fr_signal_t		sigmask;			//!< Signals to block
	void			*rctx;				//!< for resume / signal

	unlang_result_t		*p_result;			//!< If set, where to record the result
								///< of the execution.
} unlang_frame_state_xlat_t;

/** Wrap an #fr_timer_t providing data needed for unlang events
 *
 */
typedef struct {
	request_t			*request;		//!< Request this event pertains to.
	int				fd;			//!< File descriptor to wait on.
	fr_unlang_xlat_timeout_t	timeout;		//!< Function to call on timeout.
	fr_unlang_xlat_fd_event_t	fd_read;		//!< Function to call when FD is readable.
	fr_unlang_xlat_fd_event_t	fd_write;		//!< Function to call when FD is writable.
	fr_unlang_xlat_fd_event_t	fd_error;		//!< Function to call when FD has errored.
	xlat_inst_t			*inst;			//!< xlat instance data.
	xlat_thread_inst_t		*thread;		//!< Thread specific xlat instance.
	void const			*rctx;			//!< rctx data to pass to callbacks.
	fr_timer_t			*ev;			//!< Event in this worker's event heap.
} unlang_xlat_event_t;

typedef struct {
	request_t			*request;
	xlat_inst_t			*inst;			//!< xlat instance data.
	xlat_thread_inst_t		*thread;		//!< Thread specific xlat instance.

	fr_unlang_xlat_retry_t		retry_cb;		//!< callback to run on timeout
	void				*rctx;			//!< rctx data to pass to timeout callback

	fr_timer_t			*ev;			//!< retry timer just for this xlat
	fr_retry_t			retry;			//!< retry timers, etc.
} unlang_xlat_retry_t;

/** Frees an unlang event, removing it from the request's event loop
 *
 * @param[in] ev	The event to free.
 *
 * @return 0
 */
static int _unlang_xlat_event_free(unlang_xlat_event_t *ev)
{
	FR_TIMER_DELETE(&(ev->ev));

	if (ev->fd >= 0) {
		(void) fr_event_fd_delete(unlang_interpret_event_list(ev->request), ev->fd, FR_EVENT_FILTER_IO);
	}

	return 0;
}

/** Call the callback registered for a timeout event
 *
 * @param[in] tl	the event timer was inserted into.
 * @param[in] now	The current time, as held by the event_list.
 * @param[in] uctx	unlang_module_event_t structure holding callbacks.
 *
 */
static void unlang_xlat_event_timeout_handler(UNUSED fr_timer_list_t *tl, fr_time_t now, void *uctx)
{
	unlang_xlat_event_t *ev = talloc_get_type_abort(uctx, unlang_xlat_event_t);

	/*
	 *	If the timeout's fired then the xlat must necessarily
	 *	be yielded, so it's fine to pass in its rctx.
	 *
	 *	It should be able to free the rctx if it wants to.
	 *	We never free it explicitly, and instead rely on
	 *	talloc parenting.
	 */
	ev->timeout(XLAT_CTX(ev->inst->data,
			     ev->thread->data,
			     NULL,
			     ev->thread->mctx, NULL,
			     UNCONST(void *, ev->rctx)),
			     ev->request, now);

	/* Remove old references from the request */
	talloc_free(ev);
}

/** Add a timeout for an xlat handler
 *
 * @note The timeout is automatically removed when the xlat is cancelled or resumed.
 *
 * @param[in] request	the request
 * @param[in] callback	to run when the timeout hits
 * @param[in] rctx	passed to the callback
 * @param[in] when	when the timeout fires
 * @return
 *	- <0 on error
 *	- 0 on success
 */
int unlang_xlat_timeout_add(request_t *request,
			    fr_unlang_xlat_timeout_t callback, void const *rctx, fr_time_t when)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_xlat_event_t		*ev;
	unlang_frame_state_xlat_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_xlat_t);

	fr_assert(stack->depth > 0);
	fr_assert(frame->instruction->type == UNLANG_TYPE_XLAT);

	if (!state->event_ctx) MEM(state->event_ctx = talloc_zero(state, bool));

	MEM(ev = talloc_zero(state->event_ctx, unlang_xlat_event_t));
	ev->request = request;
	ev->fd = -1;
	ev->timeout = callback;
	fr_assert(state->exp->type == XLAT_FUNC);
	ev->inst = state->exp->call.inst;
	ev->thread = xlat_thread_instance_find(state->exp);
	ev->rctx = rctx;

	if (fr_timer_at(request, unlang_interpret_event_list(request)->tl,
			&ev->ev, when,
			false, unlang_xlat_event_timeout_handler, ev) < 0) {
		RPEDEBUG("Failed inserting event");
		talloc_free(ev);
		return -1;
	}

	talloc_set_destructor(ev, _unlang_xlat_event_free);

	return 0;
}

/** Push a pre-compiled xlat onto the stack for evaluation
 *
 * @param[in] ctx		To allocate value boxes and values in.
 * @param[out] p_result		If set, rcodes and priorities will be written here and
 *				not evaluated by the unlang interpreter.
 * @param[out] out		Where to write the result of the expansion.
 * @param[in] request		to push xlat onto.
 * @param[in] xlat		head of list
 * @param[in] node		to evaluate.
 * @param[in] top_frame		Set to UNLANG_TOP_FRAME if the interpreter should return.
 *				Set to UNLANG_SUB_FRAME if the interprer should continue.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int unlang_xlat_push_internal(TALLOC_CTX *ctx, unlang_result_t *p_result, fr_value_box_list_t *out,
				     request_t *request, xlat_exp_head_t const *xlat, xlat_exp_t *node, bool top_frame)
{
	/** Static instruction for performing xlat evaluations
	 *
	 */
	static unlang_t xlat_instruction = {
		.type = UNLANG_TYPE_XLAT,
		.name = "xlat",
		.debug_name = "xlat",
		.actions = MOD_ACTIONS_FAIL_TIMEOUT_RETURN,
	};

	unlang_frame_state_xlat_t	*state;
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame;

	/*
	 *	Push a new xlat eval frame onto the stack
	 */
	if (unlang_interpret_push(p_result, request, &xlat_instruction,
				  FRAME_CONF(RLM_MODULE_NOT_SET, top_frame), UNLANG_NEXT_STOP) < 0) return -1;
	frame = &stack->frame[stack->depth];

	/*
	 *	Allocate its state, and setup a cursor for the xlat nodes
	 */
	MEM(frame->state = state = talloc_zero(stack, unlang_frame_state_xlat_t));
	state->head = xlat;
	state->exp = node;
	state->p_result = p_result;
	state->ctx = ctx;

	if (node) switch (node->type) {
	case XLAT_GROUP:
	case XLAT_BOX:
		break;

	case XLAT_TMPL:
		if (tmpl_is_data(node->vpt)) break;
		FALL_THROUGH;

	default:
		RDEBUG("| %s", node->fmt);
		break;
	}

	/*
	 *	Initialise the input and output lists
	 */
	fr_dcursor_init(&state->values, fr_value_box_list_dlist_head(out));
	fr_value_box_list_init(&state->out);

	return 0;
}

/** Push a pre-compiled xlat onto the stack for evaluation
 *
 * @param[in] ctx		To allocate value boxes and values in.
 * @param[out] p_result	        The frame result
 * @param[out] out		Where to write the result of the expansion.
 * @param[in] request		to push xlat onto.
 * @param[in] xlat		to evaluate.
 * @param[in] top_frame		Set to UNLANG_TOP_FRAME if the interpreter should return.
 *				Set to UNLANG_SUB_FRAME if the interprer should continue.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int unlang_xlat_push(TALLOC_CTX *ctx, unlang_result_t *p_result, fr_value_box_list_t *out,
		     request_t *request, xlat_exp_head_t const *xlat, bool top_frame)
{
	(void) talloc_get_type_abort_const(xlat, xlat_exp_head_t);

	return unlang_xlat_push_internal(ctx, p_result, out, request, xlat, xlat_exp_head(xlat), top_frame);
}

/** Push a pre-compiled xlat onto the stack for evaluation
 *
 * @param[in] ctx		To allocate value boxes and values in.
 * @param[out] p_result		If set, and execution succeeds, true will be written
 *				here.  If execution fails, false will be written.
 * @param[out] out		Where to write the result of the expansion.
 * @param[in] request		to push xlat onto.
 * @param[in] node		to evaluate.  Only this node will be evaluated.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int unlang_xlat_push_node(TALLOC_CTX *ctx, unlang_result_t *p_result, fr_value_box_list_t *out,
			  request_t *request, xlat_exp_t *node)
{
	return unlang_xlat_push_internal(ctx, p_result, out, request, NULL, node, UNLANG_TOP_FRAME);
}

static unlang_action_t unlang_xlat_repeat(unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_frame_state_xlat_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_xlat_t);
	xlat_action_t			xa;
	xlat_exp_head_t const		*child = NULL;

	/*
	 *	If the xlat is a function with a method_env, expand it before calling the function.
	 */
	if ((state->exp->type == XLAT_FUNC) && state->exp->call.inst->call_env && !state->env_data) {
		unlang_action_t ua = call_env_expand(state, request, NULL, &state->env_data,
						     state->exp->call.inst->call_env);
		switch (ua) {
		case UNLANG_ACTION_FAIL:
			goto fail;

		case UNLANG_ACTION_PUSHED_CHILD:
			frame_repeat(frame, unlang_xlat_repeat);
			return UNLANG_ACTION_PUSHED_CHILD;

		default:
			break;
		}
	}

	xa = xlat_frame_eval_repeat(state->ctx, &state->values, &child,
				    request, state->head, &state->exp, state->env_data, &state->out);
	switch (xa) {
	case XLAT_ACTION_PUSH_CHILD:
		fr_assert(child);

		repeatable_set(frame);	/* Was cleared by the interpreter */

		/*
		 *	Clear out the results of any previous expansions
		 *	at this level.  A frame may be used to evaluate
		 *	multiple sibling nodes.
		 */
		fr_value_box_list_talloc_free(&state->out);
		if (unlang_xlat_push(state->ctx, p_result, &state->out, request, child, false) < 0) {
			REXDENT();
			return UNLANG_ACTION_STOP_PROCESSING;
		}
		return UNLANG_ACTION_PUSHED_CHILD;

	case XLAT_ACTION_PUSH_UNLANG:
		repeatable_set(frame);	/* Call the xlat code on the way back down */
		return UNLANG_ACTION_PUSHED_CHILD;

	case XLAT_ACTION_YIELD:
		if (!state->resume) {
			RWDEBUG("Missing call to unlang_xlat_yield()");
			goto fail;
		}
		repeatable_set(frame);
		return UNLANG_ACTION_YIELD;

	case XLAT_ACTION_DONE:
		*p_result = UNLANG_RESULT_RCODE(RLM_MODULE_OK);
		REXDENT();
		return UNLANG_ACTION_CALCULATE_RESULT;

	case XLAT_ACTION_FAIL:
	fail:
		*p_result = UNLANG_RESULT_RCODE(RLM_MODULE_FAIL);
		REXDENT();
		return UNLANG_ACTION_CALCULATE_RESULT;

	default:
		fr_assert(0);
		goto fail;
	}
}

/** Stub function for calling the xlat interpreter
 *
 * Calls the xlat interpreter and translates its wants and needs into
 * unlang_action_t codes.
 */
static unlang_action_t unlang_xlat(UNUSED unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_frame_state_xlat_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_xlat_t);
	xlat_action_t			xa;
	xlat_exp_head_t const		*child = NULL;

	RINDENT_SAVE(state, request);
	RINDENT();

	xa = xlat_frame_eval(state->ctx, &state->values, &child, request, state->head, &state->exp);
	switch (xa) {
	case XLAT_ACTION_PUSH_CHILD:
		fr_assert(child);

		frame_repeat(frame, unlang_xlat_repeat);

		/*
		 *	Clear out the results of any previous expansions
		 *	at this level.  A frame may be used to evaluate
		 *	multiple sibling nodes.
		 */
		fr_value_box_list_talloc_free(&state->out);
		if (unlang_xlat_push(state->ctx, p_result, &state->out, request, child, false) < 0) {
			RINDENT_RESTORE(request, state);
			return UNLANG_ACTION_STOP_PROCESSING;
		}
		return UNLANG_ACTION_PUSHED_CHILD;

	case XLAT_ACTION_PUSH_UNLANG:
		repeatable_set(frame);	/* Call the xlat code on the way back down */
		return UNLANG_ACTION_PUSHED_CHILD;

	case XLAT_ACTION_YIELD:
		if (!state->resume) {
			RWDEBUG("Missing call to unlang_xlat_yield()");
			goto fail;
		}
		repeatable_set(frame);
		return UNLANG_ACTION_YIELD;

	case XLAT_ACTION_DONE:
		*p_result = UNLANG_RESULT_RCODE(RLM_MODULE_OK);
		RINDENT_RESTORE(request, state);
		return UNLANG_ACTION_CALCULATE_RESULT;

	case XLAT_ACTION_FAIL:
	fail:
		*p_result = UNLANG_RESULT_RCODE(RLM_MODULE_FAIL);
		RINDENT_RESTORE(request, state);
		return UNLANG_ACTION_CALCULATE_RESULT;

	default:
		fr_assert(0);
		goto fail;
	}
}

/** Send a signal (usually stop) to a request that's running an xlat expansions
 *
 * This is typically called via an "async" action, i.e. an action
 * outside of the normal processing of the request.
 *
 * If there is no #xlat_func_signal_t callback defined, the action is ignored.
 *
 * @param[in] request		The current request.
 * @param[in] frame		The current stack frame.
 * @param[in] action		What the request should do (the type of signal).
 */
static void unlang_xlat_signal(request_t *request, unlang_stack_frame_t *frame, fr_signal_t action)
{
	unlang_frame_state_xlat_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_xlat_t);

	/*
	 *	Delete timers, etc. when the xlat is cancelled.
	 */
	if (action == FR_SIGNAL_CANCEL) {
		TALLOC_FREE(state->event_ctx);
	}

	if (!state->signal || (state->sigmask & action)) return;

	xlat_signal(state->signal, state->exp, request, state->rctx, action);
}

/** Called when we're ready to resume processing the request
 *
 * @param[in] p_result	the result of the xlat function.
 *			  - RLM_MODULE_OK on success.
 *			  - RLM_MODULE_FAIL on failure.
 * @param[in] request	to resume processing.
 * @param[in] frame	the current stack frame.
 * @return
 *	- UNLANG_ACTION_YIELD if additional asynchronous
 *	  operations need to be performed.
 *	- UNLANG_ACTION_CALCULATE_RESULT if done.
 */
static unlang_action_t unlang_xlat_resume(unlang_result_t *p_result, request_t *request, unlang_stack_frame_t *frame)
{
	unlang_frame_state_xlat_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_xlat_t);
	xlat_action_t			xa;
	xlat_exp_head_t const		*child = NULL;

	fr_assert(state->resume != NULL);

	/*
	 *	Delete timers, etc. when the xlat is resumed.
	 */
	TALLOC_FREE(state->event_ctx);

	xa = xlat_frame_eval_resume(state->ctx, &state->values, &child, request, state->head, &state->exp,
				    &state->out, state->resume, state->rctx);
	switch (xa) {
	case XLAT_ACTION_YIELD:
		repeatable_set(frame);
		return UNLANG_ACTION_YIELD;

	case XLAT_ACTION_DONE:
		*p_result = UNLANG_RESULT_RCODE(RLM_MODULE_OK);
		RINDENT_RESTORE(request, state);
		return UNLANG_ACTION_CALCULATE_RESULT;

	case XLAT_ACTION_PUSH_UNLANG:
		repeatable_set(frame);
		return UNLANG_ACTION_PUSHED_CHILD;

	case XLAT_ACTION_PUSH_CHILD:
		fr_assert(child);

		repeatable_set(frame);	/* Was cleared by the interpreter */

		/*
		 *	Clear out the results of any previous expansions
		 *	at this level.  A frame may be used to evaluate
		 *	multiple sibling nodes.
		 */
		fr_value_box_list_talloc_free(&state->out);
		if (unlang_xlat_push(state->ctx, state->p_result, &state->out, request, child, false) < 0) {
			RINDENT_RESTORE(request, state);
			return UNLANG_ACTION_STOP_PROCESSING;
		}
		return UNLANG_ACTION_PUSHED_CHILD;

	case XLAT_ACTION_FAIL:
		*p_result = UNLANG_RESULT_RCODE(RLM_MODULE_FAIL);
		RINDENT_RESTORE(request, state);
		return UNLANG_ACTION_CALCULATE_RESULT;
	/* DON'T SET DEFAULT */
	}

	fr_assert(0);		/* Garbage xlat action */

	*p_result = UNLANG_RESULT_RCODE(RLM_MODULE_FAIL);
	RINDENT_RESTORE(request, state);
	return UNLANG_ACTION_CALCULATE_RESULT;
}

/** Yield a request back to the interpreter from within a module
 *
 * This passes control of the request back to the unlang interpreter, setting
 * callbacks to execute when the request is 'signalled' asynchronously, or whatever
 * timer or I/O event the module was waiting for occurs.
 *
 * @note The module function which calls #unlang_module_yield should return control
 *	of the C stack to the unlang interpreter immediately after calling #unlang_xlat_yield.
 *	A common pattern is to use ``return unlang_xlat_yield(...)``.
 *
 * @param[in] request		The current request.
 * @param[in] resume		Called on unlang_interpret_mark_runnable().
 * @param[in] signal		Called on unlang_action().
 * @param[in] sigmask		Signals to block.
 * @param[in] rctx		to pass to the callbacks.
 * @return always returns XLAT_ACTION_YIELD
 */
xlat_action_t unlang_xlat_yield(request_t *request,
				xlat_func_t resume, xlat_func_signal_t signal, fr_signal_t sigmask,
				void *rctx)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_xlat_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_xlat_t);

	frame->process = unlang_xlat_resume;

	/*
	 *	Over-ride whatever functions were there before.
	 */
	state->resume = resume;
	state->signal = signal;
	state->sigmask = sigmask;
	state->rctx = rctx;

	return XLAT_ACTION_YIELD;
}

/** Frees an unlang event, removing it from the request's event loop
 *
 * @param[in] ev	The event to free.
 *
 * @return 0
 */
static int _unlang_xlat_retry_free(unlang_xlat_retry_t *ev)
{
	FR_TIMER_DELETE(&(ev->ev));

	return 0;
}

/** Call the callback registered for a timeout event
 *
 * @param[in] tl	the event timer was inserted into.
 * @param[in] now	The current time, as held by the event_list.
 * @param[in] uctx	unlang_module_event_t structure holding callbacks.
 *
 */
static void unlang_xlat_event_retry_handler(UNUSED fr_timer_list_t *tl, fr_time_t now, void *uctx)
{
	unlang_xlat_retry_t	*ev = talloc_get_type_abort(uctx, unlang_xlat_retry_t);
	request_t		*request = ev->request;

	switch (fr_retry_next(&ev->retry, now)) {
	case FR_RETRY_CONTINUE:
		/*
		 *	Call the module retry handler, with the state of the retry.  On MRD / MRC, the
		 *	module is made runnable again, and the "resume" function is called.
		 */
		ev->retry_cb(XLAT_CTX(ev->inst->data,
				      ev->thread->data,
				      NULL,
				      ev->thread->mctx, NULL,
				      UNCONST(void *, ev->rctx)),
			     ev->request, &ev->retry);

		/*
		 *	Reset the timer.
		 */
		if (fr_timer_at(ev, unlang_interpret_event_list(request)->tl, &ev->ev, ev->retry.next,
				false, unlang_xlat_event_retry_handler, request) < 0) {
			RPEDEBUG("Failed inserting event");
			talloc_free(ev);
			unlang_interpret_mark_runnable(request);
		}
		return;

	case FR_RETRY_MRD:
		RDEBUG("Reached max_rtx_duration (%pVs > %pVs) - sending timeout",
			fr_box_time_delta(fr_time_sub(now, ev->retry.start)), fr_box_time_delta(ev->retry.config->mrd));
		break;

	case FR_RETRY_MRC:
		RDEBUG("Reached max_rtx_count %u- sending timeout",
		        ev->retry.config->mrc);
		break;
	}

	/*
	 *	Run the retry handler on MRD / MRC, too.
	 */
	ev->retry_cb(XLAT_CTX(ev->inst->data,
			      ev->thread->data,
			      NULL,
			      ev->thread->mctx, NULL,
			      UNCONST(void *, ev->rctx)),
		     ev->request, &ev->retry);

	/*
	 *	On final timeout, always mark the request as runnable.
	 */
	talloc_free(ev);
	unlang_interpret_mark_runnable(request);
}


/** Yield a request back to the interpreter, with retries
 *
 * This passes control of the request back to the unlang interpreter, setting
 * callbacks to execute when the request is 'signalled' asynchronously, or when
 * the retry timer hits.
 *
 * @note The module function which calls #unlang_module_yield_to_retry should return control
 *	of the C stack to the unlang interpreter immediately after calling #unlang_module_yield_to_retry.
 *	A common pattern is to use ``return unlang_module_yield_to_retry(...)``.
 *
 * @param[in] request		The current request.
 * @param[in] resume		Called on unlang_interpret_mark_runnable().
 * @param[in] retry		Called on when a retry timer hits
 * @param[in] signal		Called on unlang_action().
 * @param[in] sigmask		Set of signals to block.
 * @param[in] rctx		to pass to the callbacks.
 * @param[in] retry_cfg		to set up the retries
 * @return
 *	- XLAT_ACTION_YIELD on success
 *	- XLAT_ACTION_FAIL on failure
 */
xlat_action_t unlang_xlat_yield_to_retry(request_t *request, xlat_func_t resume, fr_unlang_xlat_retry_t retry,
					 xlat_func_signal_t signal, fr_signal_t sigmask, void *rctx,
					 fr_retry_config_t const *retry_cfg)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_xlat_retry_t		*ev;
	unlang_frame_state_xlat_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_xlat_t);

	fr_assert(stack->depth > 0);
	fr_assert(frame->instruction->type == UNLANG_TYPE_XLAT);

	if (!state->event_ctx) MEM(state->event_ctx = talloc_zero(state, bool));

	MEM(ev = talloc_zero(state->event_ctx, unlang_xlat_retry_t));

	ev->request = request;
	fr_assert(state->exp->type == XLAT_FUNC);
	ev->inst = state->exp->call.inst;
	ev->thread = xlat_thread_instance_find(state->exp);
	ev->retry_cb = retry;
	ev->rctx = rctx;

	fr_retry_init(&ev->retry, fr_time(), retry_cfg);

	if (fr_timer_at(request, unlang_interpret_event_list(request)->tl,
			&ev->ev, ev->retry.next,
			false, unlang_xlat_event_retry_handler, ev) < 0) {
		RPEDEBUG("Failed inserting event");
		talloc_free(ev);
		return XLAT_ACTION_FAIL;
	}

	talloc_set_destructor(ev, _unlang_xlat_retry_free);

	return unlang_xlat_yield(request, resume, signal, sigmask, rctx);
}

/** Evaluate a "pure" (or not impure) xlat
 *
 * @param[in] ctx		To allocate value boxes and values in.
 * @param[out] out		Where to write the result of the expansion.
 * @param[in] request		to push xlat onto.
 * @param[in] xlat		to evaluate.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int unlang_xlat_eval(TALLOC_CTX *ctx, fr_value_box_list_t *out, request_t *request, xlat_exp_head_t const *xlat)
{
	unlang_result_t result = UNLANG_RESULT_NOT_SET;

	if (xlat->flags.impure_func) {
		fr_strerror_const("Expansion requires async operations");
		return -1;
	}

	if (unlang_xlat_push(ctx, &result, out, request, xlat, UNLANG_TOP_FRAME) < 0) return -1;

	(void) unlang_interpret(request, UNLANG_REQUEST_RUNNING);

	if (!XLAT_RESULT_SUCCESS(&result)) return -1;

	return 0;
}

/** Evaluate a "pure" (or not impure) xlat
 *
 * @param[in] ctx		To allocate value boxes and values in.
 * @param[out] vb		output value-box
 * @param[in] type		expected type
 * @param[in] enumv		enum for type
 * @param[in] request		to push xlat onto.
 * @param[in] xlat		to evaluate.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int unlang_xlat_eval_type(TALLOC_CTX *ctx, fr_value_box_t *vb, fr_type_t type, fr_dict_attr_t const *enumv, request_t *request, xlat_exp_head_t const *xlat)
{
	fr_value_box_t *src;
	fr_value_box_list_t list;

	if (fr_type_is_structural(type)) {
		fr_strerror_const("Invalid type for output of evaluation");
		return -1;
	}

	fr_value_box_list_init(&list);

	if (unlang_xlat_eval(ctx, &list, request, xlat) < 0) return -1;

	fr_value_box_init(vb, type, NULL, false);

	switch (type) {
	default:
		/*
		 *	Take only the first entry from the list.
		 */
		src = fr_value_box_list_head(&list);
		if (!src) {
			fr_strerror_const("Expression returned no results");
		fail:
			fr_value_box_list_talloc_free(&list);
			return -1;
		}

		if (fr_value_box_cast(ctx, vb, type, enumv, src) < 0) goto fail;
		fr_value_box_list_talloc_free(&list);
		break;

	case FR_TYPE_STRING:
	case FR_TYPE_OCTETS:
		/*
		 *	No output: create an empty string.
		 *
		 *	The "concat in place" function returns an error for empty input, which is arguably not
		 *	what we want to do here.
		 */
		if (fr_value_box_list_empty(&list)) {
			break;
		}

		if (fr_value_box_list_concat_in_place(ctx, vb, &list, type, FR_VALUE_BOX_LIST_FREE_BOX, false, SIZE_MAX) < 0) {
			goto fail;
		}
		break;
	}

	return 0;
}

static void unlang_xlat_dump(request_t *request, unlang_stack_frame_t *frame)
{
	unlang_frame_state_xlat_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_xlat_t);
	xlat_exp_t const		*exp = state->exp;

	if (exp) RDEBUG("expression     %s", exp->fmt);
}
/** Register xlat operation with the interpreter
 *
 */
void unlang_xlat_init(void)
{
	unlang_register(&(unlang_op_t){
			.name = "xlat",
			.type = UNLANG_TYPE_XLAT,
			.flag = UNLANG_OP_FLAG_INTERNAL,

			.interpret = unlang_xlat,
			.signal = unlang_xlat_signal,
			.dump = unlang_xlat_dump,

			.frame_state_size = sizeof(unlang_frame_state_xlat_t),
			.frame_state_type = "unlang_frame_state_xlat_t",
		});
}
