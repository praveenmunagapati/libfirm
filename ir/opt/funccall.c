/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   Optimization of function calls.
 * @author  Michael Beck
 */
#include "config.h"

#include "opt_init.h"

#include "irnode_t.h"
#include "irgraph_t.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "dbginfo_t.h"
#include "irflag_t.h"
#include "irloop_t.h"
#include "ircons.h"
#include "iredges_t.h"
#include "irpass_t.h"
#include "iroptimize.h"
#include "analyze_irg_args.h"
#include "irhooks.h"
#include "raw_bitset.h"
#include "debug.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg;)

/**
 * The walker environment for updating function calls.
 */
typedef struct env_t {
	size_t   n_calls_SymConst;
	size_t   n_calls_Sel;
	ir_node  *float_const_call_list;    /**< The list of all floating const function calls that will be changed. */
	ir_node  *nonfloat_const_call_list; /**< The list of all non-floating const function calls that will be changed. */
	ir_node  *pure_call_list;           /**< The list of all pure function calls that will be changed. */
	ir_node  *nothrow_call_list;        /**< The list of all nothrow function calls that will be changed. */
	ir_node  *proj_list;                /**< The list of all potential Proj nodes that must be fixed. */
} env_t;

/** Ready IRG's are marked in the ready set. */
static unsigned *ready_set;

/** IRG's that are in progress are marked here. */
static unsigned *busy_set;

/**
 * We misuse the mtp_property_inherited flag as temporary here.
 * The is ok, as we cannot set or get it anyway using the
 * get_addtional_properties API.
 */
#define mtp_temporary  mtp_property_inherited

/**
 * Walker: Collect all calls to const and pure functions
 * to lists. Collect all Proj(Call) nodes into a Proj list.
 */
static void collect_const_and_pure_calls(ir_node *node, void *env)
{
	env_t     *ctx = (env_t*)env;
	ir_node   *call;
	ir_node   *ptr;
	ir_entity *ent;
	unsigned  and_prop, or_prop, prop;

	if (is_Call(node)) {
		call = node;

		/* set the link to NULL for all non-const/pure calls */
		set_irn_link(call, NULL);
		ptr = get_Call_ptr(call);
		if (is_SymConst_addr_ent(ptr)) {
			ent = get_SymConst_entity(ptr);

			prop = get_entity_additional_properties(ent);
			if ((prop & (mtp_property_const|mtp_property_pure)) == 0)
				return;
			++ctx->n_calls_SymConst;
		} else if (get_opt_closed_world() &&
		           is_Sel(ptr) &&
		           get_irg_callee_info_state(get_irn_irg(node)) == irg_callee_info_consistent) {
			/* If all possible callees are const functions, we can remove the memory edge. */
			size_t i, n_callees = get_Call_n_callees(call);
			if (n_callees == 0) {
				/* This is kind of strange:  dying code or a Call that will raise an exception
				   when executed as there is no implementation to call.  So better not
				   optimize. */
				return;
			}

			/* note that const function are a subset of pure ones */
			and_prop = mtp_property_const | mtp_property_pure;
			or_prop  = 0;
			for (i = 0; i < n_callees; ++i) {
				ent = get_Call_callee(call, i);
				if (ent == unknown_entity) {
					/* we don't know which entity is called here */
					return;
				}
				prop      = get_entity_additional_properties(ent);
				and_prop &= prop;
				or_prop  &= prop;
				if (and_prop == mtp_no_property)
					return;
			}
			prop = and_prop | (or_prop & mtp_property_has_loop);
			++ctx->n_calls_Sel;
		} else
			return;

		/* ok, if we get here we found a call to a const or a pure function */
		if (prop & mtp_property_pure) {
			set_irn_link(call, ctx->pure_call_list);
			ctx->pure_call_list = call;
		} else {
			if (prop & mtp_property_has_loop) {
				set_irn_link(call, ctx->nonfloat_const_call_list);
				ctx->nonfloat_const_call_list = call;
			} else {
				set_irn_link(call, ctx->float_const_call_list);
				ctx->float_const_call_list = call;
			}
		}
	} else if (is_Proj(node)) {
		/*
		 * Collect all memory and exception Proj's from
		 * calls.
		 */
		call = get_Proj_pred(node);
		if (! is_Call(call))
			return;

		/* collect the Proj's in the Proj list */
		switch (get_Proj_proj(node)) {
		case pn_Call_M:
		case pn_Call_X_except:
		case pn_Call_X_regular:
			set_irn_link(node, ctx->proj_list);
			ctx->proj_list = node;
			break;
		default:
			break;
		}
	}
}  /* collect_const_and_pure_calls */

/**
 * Fix the list of collected Calls.
 *
 * @param irg  the graph that contained calls to pure functions
 * @param ctx  context
 */
static void fix_const_call_lists(ir_graph *irg, env_t *ctx)
{
	ir_node *call, *next, *mem, *proj;
	int exc_changed = 0;

	/* First step: fix all calls by removing their memory input and let
	 * them floating.
	 * The original memory input is preserved in their link fields. */
	for (call = ctx->float_const_call_list; call != NULL; call = next) {
		next = (ir_node*)get_irn_link(call);
		mem  = get_Call_mem(call);

		set_irn_link(call, mem);
		set_Call_mem(call, get_irg_no_mem(irg));

		/*
		 * Unfortunately we cannot simply set the node to 'float'.
		 * There is a reason for that:
		 *
		 * - The call might be inside a loop/if that is NOT entered
		 *   and calls a endless function. Setting the call to float
		 *   would allow to move it out from the loop/if causing this
		 *   function be called even if the loop/if is not entered ...
		 *
		 * This could be fixed using post-dominators for calls and Pin nodes
		 * but need some more analyzes to ensure that a call that potential
		 * never returns is not executed before some code that generates
		 * observable states...
		 */

		/* finally, this call can float */
		set_irn_pinned(call, op_pin_state_floats);
		hook_func_call(irg, call);
	}

	/* Last step: fix all Proj's */
	for (proj = ctx->proj_list; proj != NULL; proj = next) {
		next = (ir_node*)get_irn_link(proj);
		call = get_Proj_pred(proj);
		mem  = (ir_node*)get_irn_link(call);

		/* beware of calls in the pure call list */
		if (!mem || is_Call(mem))
			continue;
		assert(get_irn_mode(mem) == mode_M);

		switch (get_Proj_proj(proj)) {
		case pn_Call_M: {
			/* in dead code there might be cycles where proj == mem */
			if (proj != mem)
				exchange(proj, mem);
			 break;
		}
		case pn_Call_X_except:
			exc_changed = 1;
			exchange(proj, new_r_Bad(irg, mode_X));
			break;
		case pn_Call_X_regular: {
			ir_node *block = get_nodes_block(call);
			exc_changed = 1;
			exchange(proj, new_r_Jmp(block));
			break;
		}
		default:
			break;
		}
	}

	if (exc_changed) {
		/* ... including exception edges */
		clear_irg_state(irg, IR_GRAPH_STATE_CONSISTENT_DOMINANCE
		                   | IR_GRAPH_STATE_CONSISTENT_LOOPINFO);
	}
}  /* fix_const_call_list */

/**
 * Walker: Collect all calls to nothrow functions
 * to lists. Collect all Proj(Call) nodes into a Proj list.
 */
static void collect_nothrow_calls(ir_node *node, void *env)
{
	env_t *ctx = (env_t*)env;
	ir_node *call, *ptr;
	ir_entity *ent;
	unsigned prop;

	if (is_Call(node)) {
		call = node;

		/* set the link to NULL for all non-const/pure calls */
		set_irn_link(call, NULL);
		ptr = get_Call_ptr(call);
		if (is_SymConst_addr_ent(ptr)) {
			ent = get_SymConst_entity(ptr);

			prop = get_entity_additional_properties(ent);
			if ((prop & mtp_property_nothrow) == 0)
				return;
			++ctx->n_calls_SymConst;
		} else if (get_opt_closed_world() &&
		           is_Sel(ptr) &&
		           get_irg_callee_info_state(get_irn_irg(node)) == irg_callee_info_consistent) {
			/* If all possible callees are nothrow functions, we can remove the exception edge. */
			size_t i, n_callees = get_Call_n_callees(call);
			if (n_callees == 0) {
				/* This is kind of strange:  dying code or a Call that will raise an exception
				   when executed as there is no implementation to call.  So better not
				   optimize. */
				return;
			}

			/* note that const function are a subset of pure ones */
			prop = mtp_property_nothrow;
			for (i = 0; i < n_callees; ++i) {
				ent = get_Call_callee(call, i);
				if (ent == unknown_entity) {
					/* we don't know which entity is called here */
					return;
				}
				prop &= get_entity_additional_properties(ent);
				if (prop == mtp_no_property)
					return;
			}
			++ctx->n_calls_Sel;
		} else
			return;

		/* ok, if we get here we found a call to a nothrow function */
		set_irn_link(call, ctx->nothrow_call_list);
		ctx->nothrow_call_list = call;
	} else if (is_Proj(node)) {
		/*
		 * Collect all memory and exception Proj's from
		 * calls.
		 */
		call = get_Proj_pred(node);
		if (! is_Call(call))
			return;

		/* collect the Proj's in the Proj list */
		switch (get_Proj_proj(node)) {
		case pn_Call_M:
		case pn_Call_X_except:
		case pn_Call_X_regular:
			set_irn_link(node, ctx->proj_list);
			ctx->proj_list = node;
			break;
		default:
			break;
		}
	}
}  /* collect_nothrow_calls */

/**
 * Fix the list of collected nothrow Calls.
 *
 * @param irg        the graph that contained calls to pure functions
 * @param call_list  the list of all call sites of const functions
 * @param proj_list  the list of all memory/exception Proj's of this call sites
 */
static void fix_nothrow_call_list(ir_graph *irg, ir_node *call_list, ir_node *proj_list)
{
	ir_node *call, *next, *proj;
	int exc_changed = 0;

	/* First step: go through the list of calls and mark them. */
	for (call = call_list; call; call = next) {
		next = (ir_node*)get_irn_link(call);

		/* current_ir_graph is in memory anyway, so it's a good marker */
		set_irn_link(call, &current_ir_graph);
		hook_func_call(irg, call);
	}

	/* Second step: Remove all exception Proj's */
	for (proj = proj_list; proj; proj = next) {
		next = (ir_node*)get_irn_link(proj);
		call = get_Proj_pred(proj);

		/* handle only marked calls */
		if (get_irn_link(call) != &current_ir_graph)
			continue;

		/* kill any exception flow */
		switch (get_Proj_proj(proj)) {
		case pn_Call_X_except:
			exc_changed = 1;
			exchange(proj, new_r_Bad(irg, mode_X));
			break;
		case pn_Call_X_regular: {
			ir_node *block = get_nodes_block(call);
			exc_changed = 1;
			exchange(proj, new_r_Jmp(block));
			break;
		}
		default:
			break;
		}
	}

	/* changes were done ... */
	if (exc_changed) {
		/* ... including exception edges */
		clear_irg_state(irg, IR_GRAPH_STATE_CONSISTENT_DOMINANCE
		                   | IR_GRAPH_STATE_CONSISTENT_LOOPINFO);
	}
}  /* fix_nothrow_call_list */

/* marking */
#define SET_IRG_READY(irg)  rbitset_set(ready_set, get_irg_idx(irg))
#define IS_IRG_READY(irg)   rbitset_is_set(ready_set, get_irg_idx(irg))
#define SET_IRG_BUSY(irg)   rbitset_set(busy_set, get_irg_idx(irg))
#define CLEAR_IRG_BUSY(irg) rbitset_clear(busy_set, get_irg_idx(irg))
#define IS_IRG_BUSY(irg)    rbitset_is_set(busy_set, get_irg_idx(irg))

/* forward */
static mtp_additional_properties check_const_or_pure_function(ir_graph *irg, int top);

/**
 * Calculate the bigger property of two. Handle the temporary flag right.
 */
static mtp_additional_properties max_property(mtp_additional_properties a,
                                              mtp_additional_properties b)
{
	mtp_additional_properties r;
	mtp_additional_properties t = (a | b) & mtp_temporary;
	a &= ~mtp_temporary;
	b &= ~mtp_temporary;

	if (a == mtp_no_property || b == mtp_no_property)
		return mtp_no_property;
	r = a > b ? a : b;
	return r | t;
}  /* max_property */

/**
 * Follow the memory chain starting at node and determine
 * the mtp_property.
 *
 * @return mtp_property_const if only calls of const functions are detected
 *         mtp_property_pure  if only Loads and const/pure calls detected
 *         mtp_no_property    else
 */
static mtp_additional_properties follow_mem_(ir_node *node)
{
	mtp_additional_properties mode = mtp_property_const;
	mtp_additional_properties m;
	ir_node  *ptr;
	int i;

	for (;;) {
		if (mode == mtp_no_property)
			return mtp_no_property;

		if (irn_visited_else_mark(node))
			return mode;

		switch (get_irn_opcode(node)) {
		case iro_Proj:
			node = get_Proj_pred(node);
			break;

		case iro_NoMem:
			/* finish here */
			return mode;

		case iro_Phi:
		case iro_Sync:
			/* do a dfs search */
			for (i = get_irn_arity(node) - 1; i >= 0; --i) {
				m    = follow_mem_(get_irn_n(node, i));
				mode = max_property(mode, m);
				if (mode == mtp_no_property)
					return mtp_no_property;
			}
			return mode;

		case iro_Load:
			/* Beware volatile Loads are NOT allowed in pure functions. */
			if (get_Load_volatility(node) == volatility_is_volatile)
				return mtp_no_property;
			mode = max_property(mode, mtp_property_pure);
			node = get_Load_mem(node);
			break;

		case iro_Call:
			/* A call is only tolerable if its either constant or pure. */
			ptr = get_Call_ptr(node);
			if (is_SymConst_addr_ent(ptr)) {
				ir_entity *ent = get_SymConst_entity(ptr);
				ir_graph  *irg = get_entity_irg(ent);

				if (irg == NULL) {
					m = get_entity_additional_properties(ent) & (mtp_property_const|mtp_property_pure);
					mode = max_property(mode, m);
				} else {
					/* we have a graph, analyze it. */
					m = check_const_or_pure_function(irg, /*top=*/0);
					mode = max_property(mode, m);
				}
			} else
				return mtp_no_property;
			node = get_Call_mem(node);
			break;

		default:
			return mtp_no_property;
		}
	}
}

/**
 * Follow the memory chain starting at node and determine
 * the mtp_property.
 *
 * @return mtp_property_const if only calls of const functions are detected
 *         mtp_property_pure  if only Loads and const/pure calls detected
 *         mtp_no_property else
 */
static mtp_additional_properties follow_mem(ir_node *node, mtp_additional_properties mode)
{
	mtp_additional_properties m = follow_mem_(node);
	return max_property(mode, m);
}

/**
 * Check if a graph represents a const or a pure function.
 *
 * @param irg  the graph to check
 * @param top  if set, this is the top call
 */
static mtp_additional_properties check_const_or_pure_function(ir_graph *irg, int top)
{
	ir_node *end, *endbl;
	int j;
	ir_entity *entity   = get_irg_entity(irg);
	ir_type   *type     = get_entity_type(entity);
	size_t     n_params = get_method_n_params(type);
	size_t     i;
	mtp_additional_properties may_be_const = mtp_property_const;
	mtp_additional_properties prop = get_irg_additional_properties(irg);

	/* libfirm handles aggregate parameters by passing around pointers to
	 * stuff in memory, so if we have compound parameters we are never const */
	for (i = 0; i < n_params; ++i) {
		ir_type *param = get_method_param_type(type, i);
		if (is_compound_type(param)) {
			prop        &= ~mtp_property_const;
			may_be_const = mtp_no_property;
		}
	}

	if (prop & mtp_property_const) {
		/* already marked as a const function */
		return mtp_property_const;
	}
	if (prop & mtp_property_pure) {
		/* already marked as a pure function */
		return mtp_property_pure;
	}

	if (IS_IRG_READY(irg)) {
		/* already checked */
		return mtp_no_property;
	}
	if (IS_IRG_BUSY(irg)) {
		/* We are still evaluate this method.
		 * The function (indirectly) calls itself and thus may not terminate.
		 */
		return mtp_no_property;
	}
	SET_IRG_BUSY(irg);

	end   = get_irg_end(irg);
	endbl = get_nodes_block(end);
	prop  = may_be_const;

	ir_reserve_resources(irg, IR_RESOURCE_IRN_VISITED);
	inc_irg_visited(irg);
	/* mark the initial mem: recursion of follow_mem() stops here */
	mark_irn_visited(get_irg_initial_mem(irg));

	/* visit every Return */
	for (j = get_Block_n_cfgpreds(endbl) - 1; j >= 0; --j) {
		ir_node   *node = get_Block_cfgpred(endbl, j);
		unsigned   code = get_irn_opcode(node);
		ir_node   *mem;

		/* Bad nodes usually do NOT produce anything, so it's ok */
		if (code == iro_Bad)
			continue;

		if (code == iro_Return) {
			mem = get_Return_mem(node);

			/* Bad nodes usually do NOT produce anything, so it's ok */
			if (is_Bad(mem))
				continue;

			if (mem != get_irg_initial_mem(irg))
				prop = max_property(prop, follow_mem(mem, prop));
		} else {
			/* Exception found. Cannot be const or pure. */
			prop = mtp_no_property;
			break;
		}
		if (prop == mtp_no_property)
			break;
	}

	if (prop != mtp_no_property) {
		/* check, if a keep-alive exists */
		for (j = get_End_n_keepalives(end) - 1; j >= 0; --j) {
			ir_node *kept = get_End_keepalive(end, j);

			if (is_Block(kept)) {
				prop = mtp_no_property;
				break;
			}

			if (mode_M != get_irn_mode(kept))
				continue;

			prop = max_property(prop, follow_mem(kept, prop));
			if (prop == mtp_no_property)
				break;
		}
	}

	if (top) {
		/* Set the property only if we are at top-level. */
		if (prop != mtp_no_property) {
			add_irg_additional_properties(irg, prop);
		}
		SET_IRG_READY(irg);
	}
	CLEAR_IRG_BUSY(irg);
	ir_free_resources(irg, IR_RESOURCE_IRN_VISITED);
	return prop;
}  /* check_const_or_pure_function */

/**
 * Handle calls to const functions.
 *
 * @param ctx  context
 */
static void handle_const_Calls(env_t *ctx)
{
	size_t i, n;

	ctx->n_calls_SymConst = 0;
	ctx->n_calls_Sel      = 0;

	/* all calls of const functions can be transformed */
	for (i = 0, n = get_irp_n_irgs(); i < n; ++i) {
		ir_graph *irg  = get_irp_irg(i);

		ctx->float_const_call_list    = NULL;
		ctx->nonfloat_const_call_list = NULL;
		ctx->pure_call_list           = NULL;
		ctx->proj_list                = NULL;

		ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
		irg_walk_graph(irg, NULL, collect_const_and_pure_calls, ctx);

		if (ctx->float_const_call_list != NULL)
			fix_const_call_lists(irg, ctx);
		ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
	}
}  /* handle_const_Calls */

/**
 * Handle calls to nothrow functions.
 *
 * @param ctx  context
 */
static void handle_nothrow_Calls(env_t *ctx)
{
	size_t i, n;

	ctx->n_calls_SymConst = 0;
	ctx->n_calls_Sel      = 0;

	/* all calls of const functions can be transformed */
	for (i = 0, n = get_irp_n_irgs(); i < n; ++i) {
		ir_graph *irg  = get_irp_irg(i);

		ctx->nothrow_call_list = NULL;
		ctx->proj_list         = NULL;

		ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
		irg_walk_graph(irg, NULL, collect_nothrow_calls, ctx);

		if (ctx->nothrow_call_list)
			fix_nothrow_call_list(irg, ctx->nothrow_call_list, ctx->proj_list);
		ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
	}
}

/**
 * Check, whether a given node represents a return value of
 * a malloc like function (ie, new heap allocated memory).
 *
 * @param node  the node to check
 */
static int is_malloc_call_result(const ir_node *node)
{
	if (is_Alloc(node) && get_Alloc_where(node) == heap_alloc) {
		/* Firm style high-level allocation */
		return 1;
	}
	/* TODO: check mtp_malloc */
	return 0;
}

/**
 * Update a property depending on a call property.
 */
static mtp_additional_properties update_property(mtp_additional_properties orig_prop, mtp_additional_properties call_prop)
{
	mtp_additional_properties t = (orig_prop | call_prop) & mtp_temporary;
	mtp_additional_properties r = orig_prop & call_prop;
	return r | t;
}

/**
 * Check if a node is stored.
 */
static int is_stored(const ir_node *n)
{
	const ir_edge_t *edge;
	const ir_node   *ptr;

	foreach_out_edge(n, edge) {
		const ir_node *succ = get_edge_src_irn(edge);

		switch (get_irn_opcode(succ)) {
		case iro_Return:
		case iro_Load:
		case iro_Cmp:
			/* ok */
			break;
		case iro_Store:
			if (get_Store_value(succ) == n)
				return 1;
			/* ok if its only the address input */
			break;
		case iro_Sel:
		case iro_Cast:
		case iro_Confirm:
			if (is_stored(succ))
				return 1;
			break;
		case iro_Call:
			ptr = get_Call_ptr(succ);
			if (is_SymConst_addr_ent(ptr)) {
				ir_entity *ent = get_SymConst_entity(ptr);
				size_t    i;

				/* we know the called entity */
				for (i = get_Call_n_params(succ); i > 0;) {
					if (get_Call_param(succ, --i) == n) {
						/* n is the i'th param of the call */
						if (get_method_param_access(ent, i) & ptr_access_store) {
							/* n is store in ent */
							return 1;
						}
					}
				}
			} else {
				/* unknown call address */
				return 1;
			}
			break;
		default:
			/* bad, potential alias */
			return 1;
		}
	}
	return 0;
}  /* is_stored */

/**
 * Check that the return value of an irg is not stored anywhere.
 *
 * return ~mtp_property_malloc if return values are stored, ~0 else
 */
static mtp_additional_properties check_stored_result(ir_graph *irg)
{
	ir_node  *end_blk = get_irg_end_block(irg);
	int      i;
	mtp_additional_properties res = ~mtp_no_property;
	int      old_edges = edges_assure_kind(irg, EDGE_KIND_NORMAL);

	for (i = get_Block_n_cfgpreds(end_blk) - 1; i >= 0; --i) {
		ir_node *pred = get_Block_cfgpred(end_blk, i);
		size_t  j;

		if (! is_Return(pred))
			continue;
		for (j = get_Return_n_ress(pred); j > 0;) {
			const ir_node *irn = get_Return_res(pred, --j);

			if (is_stored(irn)) {
				/* bad, might create an alias */
				res = ~mtp_property_malloc;
				goto finish;
			}
		}
	}
finish:
	if (! old_edges)
		edges_deactivate_kind(irg, EDGE_KIND_NORMAL);
	return res;
}

/**
 * Check if a graph represents a nothrow or a malloc function.
 *
 * @param irg  the graph to check
 * @param top  if set, this is the top call
 */
static mtp_additional_properties check_nothrow_or_malloc(ir_graph *irg, int top)
{
	mtp_additional_properties curr_prop = mtp_property_malloc | mtp_property_nothrow;
	ir_node                  *end_blk   = get_irg_end_block(irg);
	ir_entity *ent;
	ir_type   *mtp;
	int       i;

	if (IS_IRG_READY(irg)) {
		/* already checked */
		return get_irg_additional_properties(irg);
	}
	if (IS_IRG_BUSY(irg)) {
		/* we are still evaluate this method. Be optimistic,
		return the best possible so far but mark the result as temporary. */
		return mtp_temporary | mtp_property_malloc | mtp_property_nothrow;
	}
	SET_IRG_BUSY(irg);

	ent = get_irg_entity(irg);
	mtp = get_entity_type(ent);

	if (get_method_n_ress(mtp) <= 0)
		curr_prop &= ~mtp_property_malloc;

	for (i = get_Block_n_cfgpreds(end_blk) - 1; i >= 0; --i) {
		ir_node *pred = get_Block_cfgpred(end_blk, i);

		if (is_Return(pred)) {
			if (curr_prop & mtp_property_malloc) {
				size_t j;

				/* check, if malloc is called here */
				for (j = get_Return_n_ress(pred); j > 0;) {
					ir_node *res = get_Return_res(pred, --j);

					/* skip Confirms and Casts */
					res = skip_HighLevel_ops(res);
					/* skip Proj's */
					while (is_Proj(res))
						res = get_Proj_pred(res);
					if (is_malloc_call_result(res)) {
						/* ok, this is a malloc */
					} else if (is_Call(res)) {
						ir_node *ptr = get_Call_ptr(res);

						if (is_SymConst_addr_ent(ptr)) {
							/* a direct call */
							ir_entity *ent    = get_SymConst_entity(ptr);
							ir_graph  *callee = get_entity_irg(ent);

							if (callee == irg) {
								/* A self-recursive call. The property did not depend on this call. */
							} else if (callee != NULL) {
								mtp_additional_properties prop = check_nothrow_or_malloc(callee, /*top=*/0);
								curr_prop = update_property(curr_prop, prop);
							} else {
								curr_prop = update_property(curr_prop, get_entity_additional_properties(ent));
							}
						} else if (get_opt_closed_world() &&
						           is_Sel(ptr) &&
						           get_irg_callee_info_state(irg) == irg_callee_info_consistent) {
							/* check if all possible callees are malloc functions. */
							size_t i, n_callees = get_Call_n_callees(res);
							if (n_callees == 0) {
								/* This is kind of strange:  dying code or a Call that will raise an exception
								   when executed as there is no implementation to call.  So better not
								   optimize. */
								curr_prop &= ~mtp_property_malloc;
								continue;
							}

							for (i = 0; i < n_callees; ++i) {
								ir_entity *ent = get_Call_callee(res, i);
								if (ent == unknown_entity) {
									/* we don't know which entity is called here */
									curr_prop &= ~mtp_property_malloc;
									break;
								}
								if ((get_entity_additional_properties(ent) & mtp_property_malloc) == 0) {
									curr_prop &= ~mtp_property_malloc;
									break;
								}
							}
							/* if we pass the for cycle, malloc is still ok */
						} else {
							/* unknown call */
							curr_prop &= ~mtp_property_malloc;
						}
					} else {
						/* unknown return value */
						curr_prop &= ~mtp_property_malloc;
					}
				}
			}
		} else if (curr_prop & mtp_property_nothrow) {
			/* exception flow detected */
			pred = skip_Proj(pred);

			if (is_Call(pred)) {
				ir_node *ptr = get_Call_ptr(pred);

				if (is_SymConst_addr_ent(ptr)) {
					/* a direct call */
					ir_entity *ent    = get_SymConst_entity(ptr);
					ir_graph  *callee = get_entity_irg(ent);

					if (callee == irg) {
						/* A self-recursive call. The property did not depend on this call. */
					} else if (callee != NULL) {
						/* Note: we check here for nothrow only, so do NOT reset the malloc property */
						mtp_additional_properties prop = check_nothrow_or_malloc(callee, /*top=*/0) | mtp_property_malloc;
						curr_prop = update_property(curr_prop, prop);
					} else {
						if ((get_entity_additional_properties(ent) & mtp_property_nothrow) == 0)
							curr_prop &= ~mtp_property_nothrow;
					}
				} else if (get_opt_closed_world() &&
				           is_Sel(ptr) &&
				           get_irg_callee_info_state(irg) == irg_callee_info_consistent) {
					/* check if all possible callees are nothrow functions. */
					size_t i, n_callees = get_Call_n_callees(pred);
					if (n_callees == 0) {
						/* This is kind of strange:  dying code or a Call that will raise an exception
						   when executed as there is no implementation to call.  So better not
						   optimize. */
						curr_prop &= ~mtp_property_nothrow;
						continue;
					}

					for (i = 0; i < n_callees; ++i) {
						ir_entity *ent = get_Call_callee(pred, i);
						if (ent == unknown_entity) {
							/* we don't know which entity is called here */
							curr_prop &= ~mtp_property_nothrow;
							break;
						}
						if ((get_entity_additional_properties(ent) & mtp_property_nothrow) == 0) {
							curr_prop &= ~mtp_property_nothrow;
							break;
						}
					}
					/* if we pass the for cycle, nothrow is still ok */
				} else {
					/* unknown call */
					curr_prop &= ~mtp_property_nothrow;
				}
			} else {
				/* real exception flow possible. */
				curr_prop &= ~mtp_property_nothrow;
			}
		}
		if ((curr_prop & ~mtp_temporary) == mtp_no_property) {
			/* no need to search further */
			break;
		}
	}

	if (curr_prop & mtp_property_malloc) {
		/*
		 * Note that the malloc property means not only return newly allocated
		 * memory, but also that this memory is ALIAS FREE.
		 * To ensure that, we do NOT allow that the returned memory is somewhere
		 * stored.
	     */
		curr_prop &= check_stored_result(irg);
	}

	if (curr_prop != mtp_no_property) {
		if (top || (curr_prop & mtp_temporary) == 0) {
			/* We use the temporary flag here to mark an optimistic result.
			   Set the property only if we are sure that it does NOT base on
			   temporary results OR if we are at top-level. */
			add_irg_additional_properties(irg, curr_prop & ~mtp_temporary);
			SET_IRG_READY(irg);
		}
	}
	if (top)
		SET_IRG_READY(irg);
	CLEAR_IRG_BUSY(irg);
	return curr_prop;
}  /* check_nothrow_or_malloc */

/**
 * When a function was detected as "const", it might be moved out of loops.
 * This might be dangerous if the graph can contain endless loops.
 */
static void check_for_possible_endless_loops(ir_graph *irg)
{
	ir_loop *root_loop;
	assure_loopinfo(irg);

	root_loop = get_irg_loop(irg);
	if (root_loop->flags & loop_outer_loop)
		add_irg_additional_properties(irg, mtp_property_has_loop);
}

/*
 * optimize function calls by handling const functions
 */
void optimize_funccalls(void)
{
	size_t i, n;
	size_t last_idx;
	env_t  ctx;
	size_t num_const   = 0;
	size_t num_pure    = 0;
	size_t num_nothrow = 0;
	size_t num_malloc  = 0;

	/* prepare: mark all graphs as not analyzed */
	last_idx  = get_irp_last_idx();
	ready_set = rbitset_malloc(last_idx);
	busy_set  = rbitset_malloc(last_idx);

	/* first step: detect, which functions are nothrow or malloc */
	DB((dbg, LEVEL_2, "Detecting nothrow and malloc properties ...\n"));
	for (i = 0, n = get_irp_n_irgs(); i < n; ++i) {
		ir_graph *irg = get_irp_irg(i);
		unsigned prop = check_nothrow_or_malloc(irg, /*top=*/1);

		if (prop & mtp_property_nothrow) {
			++num_nothrow;
			DB((dbg, LEVEL_2, "%+F has the nothrow property\n", irg));
		} else if (prop & mtp_property_malloc) {
			++num_malloc;
			DB((dbg, LEVEL_2, "%+F has the malloc property\n", irg));
		}
	}

	/* second step: remove exception edges: this must be done before the
	   detection of const and pure functions take place. */
	handle_nothrow_Calls(&ctx);
	DB((dbg, LEVEL_1, "Detected %zu nothrow graphs, %zu malloc graphs.\n", num_nothrow, num_malloc));
	DB((dbg, LEVEL_1, "Optimizes %zu(SymConst) + %zu(Sel) calls to nothrow functions.\n",
		ctx.n_calls_SymConst, ctx.n_calls_Sel));

	rbitset_clear_all(ready_set, last_idx);
	rbitset_clear_all(busy_set, last_idx);

	/* third step: detect, which functions are const or pure */
	DB((dbg, LEVEL_2, "Detecting const and pure properties ...\n"));
	for (i = 0, n = get_irp_n_irgs(); i < n; ++i) {
		ir_graph *irg = get_irp_irg(i);
		unsigned prop = check_const_or_pure_function(irg, /*top=*/1);

		if (prop & mtp_property_const) {
			++num_const;
			DB((dbg, LEVEL_2, "%+F has the const property\n", irg));
			check_for_possible_endless_loops(irg);
		} else if (prop & mtp_property_pure) {
			++num_pure;
			DB((dbg, LEVEL_2, "%+F has the pure property\n", irg));
		}
	}

	handle_const_Calls(&ctx);
	DB((dbg, LEVEL_1, "Detected %zu const graphs, %zu pure graphs.\n", num_const, num_pure));
	DB((dbg, LEVEL_1, "Optimizes %u(SymConst) + %u(Sel) calls to const functions.\n",
		   ctx.n_calls_SymConst, ctx.n_calls_Sel));

	xfree(busy_set);
	xfree(ready_set);
}

/* initialize the funccall optimization */
void firm_init_funccalls(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.opt.funccalls");
}

/* Creates an ir_prog pass for optimize_funccalls. */
ir_prog_pass_t *optimize_funccalls_pass(const char *name)
{
	return def_prog_pass(name ? name : "funccall", optimize_funccalls);
}
