/**
 * Scheduling algorithms.
 * An ILP scheduler based on
 * "ILP-based Instruction Scheduling for IA-64"
 * by Daniel Kaestner and Sebastian Winkel
 *
 * @date   22.10.2005
 * @author Christian Wuerdig
 * @cvs-id $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WITH_ILP

#include <math.h>

#ifndef _WIN32
#include <strings.h>
#endif /* _WIN32 */

#include "irnode_t.h"
#include "irgwalk.h"
#include "irbitset.h"
#include "irphase_t.h"
#include "height.h"
#include "iredges.h"
#include "pdeq.h"
#include "debug.h"
#include "irtools.h"
#include "irdump.h"
#include "plist.h"

#include <lpp/lpp.h>
#include <lpp/lpp_net.h>

#ifdef WITH_LIBCORE
#include <libcore/lc_opts.h>
#include <libcore/lc_opts_enum.h>
#include <libcore/lc_timing.h>
#endif /* WITH_LIBCORE */

#include "be.h"
#include "benode_t.h"
#include "besched_t.h"
#include "beilpsched.h"

typedef struct _ilpsched_options_t {
	unsigned limit_dead;
	unsigned time_limit;
	char     log_file[1024];
} ilpsched_options_t;

typedef struct _unit_type_info_t {
	int                            n_units;
	const be_execution_unit_type_t *tp;
} unit_type_info_t;

/**
 * holding the ILP variables of the different types
 */
typedef struct _ilp_var_types_t {
	int *x;   /* x_{nt}^k variables */
	int *a;   /* a_{nt}^k variables */
	int *d;   /* d_{nt}^k variables */
	int *y;   /* y_{nt}^k variables */
} ilp_var_types_t;

/* attributes for a node */
typedef struct _ilpsched_node_attr_t {
	unsigned asap;                     /**< The ASAP scheduling control step */
	unsigned alap;                     /**< The ALAP scheduling control step */
	unsigned latency;                  /**< Latency of this node (needed for sorting) */
	unsigned sched_point;              /**< the step in which the node is finally scheduled */
	unsigned visit_idx;                /**< Index of the node having visited this node last */
	unsigned consumer_idx;             /**< Index of the node having counted this node as consumer last */
	unsigned n_consumer;               /**< Number of consumers */
	ir_node  **block_consumer;         /**< List of consumer being in the same block */
	waitq    *projkeeps;               /**< A List of Projs and Keeps belonging to this node */
	unsigned block_idx     : 30;       /**< A unique per block index */
	unsigned alap_changed  : 1;        /**< the current ALAP has changed, revisit preds */
	unsigned is_dummy_node : 1;        /**< this node is assigned to DUMMY unit */
	bitset_t *transitive_block_nodes;  /**< Set of transitive block nodes (predecessors
											for ASAP, successors for ALAP */
	unsigned n_unit_types;             /**< number of allowed execution unit types */
	unit_type_info_t *type_info;       /**< list of allowed execution unit types */
	ilp_var_types_t  ilp_vars;         /**< the different ILP variables */
} ilpsched_node_attr_t;

/* attributes for a block */
typedef struct _ilpsched_block_attr_t {
	unsigned block_last_idx;        /**< The highest node index in block so far */
	unsigned n_interesting_nodes;   /**< The number of nodes interesting for scheduling */
	unsigned max_steps;             /**< Upper bound for block execution */
	plist_t  *root_nodes;           /**< A list of nodes having no user in current block */
	ir_node  *head_ilp_nodes;       /**< A linked list of nodes which will contribute to ILP */
} ilpsched_block_attr_t;

typedef union _ilpsched_attr_ {
	ilpsched_node_attr_t  node_attr;
	ilpsched_block_attr_t block_attr;
} ilpsched_attr_t;

/* A irn for the phase and it's attributes (either node or block) */
typedef struct {
	ir_node         *irn;
	ilpsched_attr_t attr;
} be_ilpsched_irn_t;

/* The ILP scheduling environment */
typedef struct {
	phase_t              ph;            /**< The phase */
	ir_graph             *irg;          /**< The current irg */
	heights_t            *height;       /**< The heights object of the irg */
	void                 *irg_env;      /**< An environment for the irg scheduling, provided by the backend */
	void                 *block_env;    /**< An environment for scheduling a block, provided by the backend */
	const arch_env_t     *arch_env;
	const arch_isa_t     *isa;          /**< The ISA */
	const be_main_env_t  *main_env;
	const be_machine_t   *cpu;          /**< the current abstract machine */
	ilpsched_options_t   *opts;         /**< the ilp options for current irg */
	const ilp_sched_selector_t *sel;    /**< The ILP sched selector provided by the backend */
	DEBUG_ONLY(firm_dbg_module_t *dbg);
} be_ilpsched_env_t;

/* convenience macros to handle phase irn data */
#define get_ilpsched_irn(ilpsched_env, irn) (phase_get_or_set_irn_data(&(ilpsched_env)->ph, (irn)))
#define is_ilpsched_block(node)             (is_Block((node)->irn))
#define get_ilpsched_block_attr(block)      (&(block)->attr.block_attr)
#define get_ilpsched_node_attr(node)        (&(node)->attr.node_attr)

/* iterate over a list of ir_nodes linked by link field */
#define foreach_linked_irns(head, iter) for ((iter) = (head); (iter); (iter) = get_irn_link((iter)))

/* check if node is considered for ILP scheduling */
#define consider_for_sched(isa, irn) \
	(! (is_Block(irn)            ||  \
		is_normal_Proj(isa, irn) ||  \
		is_Phi(irn)              ||  \
		is_NoMem(irn)            ||  \
		is_Jmp(irn)              ||  \
		is_End(irn)                  \
		))

/* gives the valid scheduling time step interval for a node */
#define VALID_SCHED_INTERVAL(na) ((na)->alap - (na)->asap + 1)

/* gives the valid interval where a node can die */
#define VALID_KILL_INTERVAL(ba, na) ((ba)->max_steps - (na)->asap + 1)

/* gives the corresponding ILP variable for given node, unit and time step */
#define ILPVAR_IDX(na, unit, control_step) \
	((unit) * VALID_SCHED_INTERVAL((na)) + (control_step) - (na)->asap + 1)

/* gives the corresponding dead nodes ILP variable for given node, unit and time step */
#define ILPVAR_IDX_DEAD(ba, na, unit, control_step) \
	((unit) * VALID_KILL_INTERVAL((ba), (na)) + (control_step) - (na)->asap + 1)

/* check if a double value is within an epsilon environment of 0 */
#define LPP_VALUE_IS_0(dbl) (fabs((dbl)) <= 1e-10)

#ifdef WITH_LIBCORE
	#define ilp_timer_push(t)         lc_timer_push((t))
	#define ilp_timer_pop()           lc_timer_pop()
	#define ilp_timer_elapsed_usec(t) lc_timer_elapsed_usec((t))
#else /* WITH_LIBCORE */
	#define ilp_timer_push(t)
	#define ilp_timer_pop()
	#define ilp_timer_elapsed_usec(t) 0.0
#endif /* WITH_LIBCORE */

/* option variable */
static ilpsched_options_t ilp_opts = {
	70,    /* if we have more than 70 nodes: use alive nodes constraint */
	300,   /* 300 sec per block time limit */
	""     /* no log file */
};

#ifdef WITH_LIBCORE
/* ILP options */
static const lc_opt_table_entry_t ilpsched_option_table[] = {
	LC_OPT_ENT_INT("limit_dead", "Upto how many nodes the dead node constraint should be used", &ilp_opts.limit_dead),
	LC_OPT_ENT_INT("time_limit", "ILP time limit per block", &ilp_opts.time_limit),
	LC_OPT_ENT_STR("lpp_log",    "LPP logfile (stderr and stdout are supported)", ilp_opts.log_file, sizeof(ilp_opts.log_file)),
	{ NULL }
};
#endif /* WITH_LIBCORE */

/*
	We need this global variable as we compare nodes dependent on heights,
	but we cannot pass any information to the qsort compare function.
*/
static heights_t *glob_heights;

/**
 * Check if irn is a Proj, which has no execution units assigned.
 * @return 1 if irn is a Proj having no execution units assigned, 0 otherwise
 */
static INLINE int is_normal_Proj(const arch_isa_t *isa, const ir_node *irn) {
	return is_Proj(irn) && (arch_isa_get_allowed_execution_units(isa, irn) == NULL);
}

/**
 * Skips normal Projs.
 * @return predecessor if irn is a normal Proj, otherwise irn.
 */
static INLINE ir_node *skip_normal_Proj(const arch_isa_t *isa, ir_node *irn) {
	if (is_normal_Proj(isa, irn))
		return get_Proj_pred(irn);
	return irn;
}

static INLINE int fixed_latency(const ilp_sched_selector_t *sel, ir_node *irn, void *env) {
	unsigned lat = be_ilp_sched_latency(sel, irn, env);
	if (lat == 0 && ! is_Proj(irn) && ! be_is_Keep(irn))
		lat = 1;
	return lat;
}


/**
 * Compare scheduling time steps of two be_ilpsched_irn's.
 */
static int cmp_ilpsched_irn(const void *a, const void *b) {
	be_ilpsched_irn_t    *n1   = *(be_ilpsched_irn_t **)a;
	be_ilpsched_irn_t    *n2   = *(be_ilpsched_irn_t **)b;
	ilpsched_node_attr_t *n1_a = get_ilpsched_node_attr(n1);
	ilpsched_node_attr_t *n2_a = get_ilpsched_node_attr(n2);

	if (n1_a->sched_point == n2_a->sched_point) {
		ir_node *irn_a = n1->irn;
		ir_node *irn_b = n2->irn;

		if (heights_reachable_in_block(glob_heights, irn_a, irn_b))
			return 1;
		if (heights_reachable_in_block(glob_heights, irn_b, irn_a))
			return -1;

		/*
			Ok, timestep is equal and the nodes are parallel,
			so check latency and schedule high latency first.
		*/
		return QSORT_CMP(n2_a->latency, n1_a->latency);
	}
	else
		return QSORT_CMP(n1_a->sched_point, n2_a->sched_point);
}

/**
 * In case there is no phase information for irn, initialize it.
 */
static void *init_ilpsched_irn(phase_t *ph, ir_node *irn, void *old) {
	be_ilpsched_irn_t *res = old ? old : phase_alloc(ph, sizeof(res[0]));

	if (res == old) {
		/* if we have already some data: check for reinitialization */

		if (! is_Block(irn)) {
			ilpsched_node_attr_t *na = get_ilpsched_node_attr(res);

			if (! na->transitive_block_nodes) {
				ir_node               *block      = get_nodes_block(irn);
				be_ilpsched_irn_t     *block_node = phase_get_or_set_irn_data(ph, block);
				ilpsched_block_attr_t *ba         = get_ilpsched_block_attr(block_node);

				/* we are called after the block indices have been build: create bitset */
				na->transitive_block_nodes = bitset_obstack_alloc(phase_obst(ph), ba->block_last_idx);
			}
			else {
				/* we are called from reinit block data: clear the bitset */
				bitset_clear_all(na->transitive_block_nodes);
				na->visit_idx    = 0;
				na->alap_changed = 1;
			}
		}
		return old;
	}

	res->irn = irn;

	/* set ilpsched irn attributes (either block or irn) */
	if (is_Block(irn)) {
		ilpsched_block_attr_t *ba = get_ilpsched_block_attr(res);

		ba->n_interesting_nodes = 0;
		ba->block_last_idx      = 0;
		ba->root_nodes          = plist_new();
		ba->head_ilp_nodes      = NULL;
		ba->max_steps           = 0;
	}
	else {
		ilpsched_node_attr_t *na = get_ilpsched_node_attr(res);
		memset(na, 0, sizeof(*na));
	}

	return res;
}

/**
 * Assign a per block unique number to each node.
 */
static void build_block_idx(ir_node *irn, void *walk_env) {
	be_ilpsched_env_t     *env = walk_env;
	be_ilpsched_irn_t     *node, *block_node;
	ilpsched_node_attr_t  *na;
	ilpsched_block_attr_t *ba;

	if (! consider_for_sched(env->arch_env->isa, irn))
		return;

	node       = get_ilpsched_irn(env, irn);
	na         = get_ilpsched_node_attr(node);
	block_node = get_ilpsched_irn(env, get_nodes_block(irn));
	ba         = get_ilpsched_block_attr(block_node);

	na->block_idx = ba->block_last_idx++;
}

/********************************************************
 *                              __        _
 *                             / /       | |
 *   __ _ ___  __ _ _ __      / /    __ _| | __ _ _ __
 *  / _` / __|/ _` | '_ \    / /    / _` | |/ _` | '_ \
 * | (_| \__ \ (_| | |_) |  / /    | (_| | | (_| | |_) |
 *  \__,_|___/\__,_| .__/  /_/      \__,_|_|\__,_| .__/
 *                 | |                           | |
 *                 |_|                           |_|
 ********************************************************/

/**
 * Add all nodes having no user in current block to last_nodes list.
 */
static void collect_alap_root_nodes(ir_node *irn, void *walk_env) {
	ir_node               *block;
	const ir_edge_t       *edge;
	be_ilpsched_irn_t     *block_node, *node;
	ilpsched_block_attr_t *ba;
	ilpsched_node_attr_t  *na;
	int                   i, j;
	be_ilpsched_env_t     *env           = walk_env;
	int                   has_block_user = 0;
	unsigned              n_consumer     = 0;
	ir_edge_kind_t        ekind[2]       = { EDGE_KIND_NORMAL, EDGE_KIND_DEP };
	ir_node               **consumer;
	int                   idx;

	if (! consider_for_sched(env->arch_env->isa, irn))
		return;

	block    = get_nodes_block(irn);
    idx      = get_irn_idx(irn);
	consumer = NEW_ARR_F(ir_node *, 0);

	DBG((env->dbg, LEVEL_3, "%+F (%+F) is interesting, examining ... ", irn, block));

	/* check data and dependency out edges */
	for (i = 0; i < 2 && ! has_block_user; ++i) {
		foreach_out_edge_kind(irn, edge, ekind[i]) {
			ir_node *user = get_edge_src_irn(edge);

			if (is_normal_Proj(env->arch_env->isa, user)) {
				const ir_edge_t *user_edge;

				if (get_irn_mode(user) == mode_X)
					continue;

				/* The ABI ensures, that there will be no ProjT nodes in the graph. */
				for (j = 0; j < 2; ++j) {
					foreach_out_edge_kind(user, user_edge, ekind[j]) {
						ir_node *real_user = get_edge_src_irn(user_edge);

						if (! is_Phi(real_user) && ! is_Block(real_user)) {
							be_ilpsched_irn_t    *node = get_ilpsched_irn(env, real_user);
							ilpsched_node_attr_t *ua   = get_ilpsched_node_attr(node);

							/* skip already visited nodes */
							if (ua->consumer_idx == idx)
								continue;

							/* check if node has user in this block and collect the user if it's a data user */
							if (get_nodes_block(real_user) == block) {
								if (i == 0 && j == 0)
									ARR_APP1(ir_node *, consumer, real_user);
								has_block_user = 1;
							}

							/* only count data consumer */
							if (i == 0)
								n_consumer++;

							/* mark user as visited by this node */
							ua->consumer_idx = idx;
						}
					}
				}
			}
			else if (is_Block(user)) {
				continue;
			}
			else if (! is_Phi(user)) {
				be_ilpsched_irn_t    *node = get_ilpsched_irn(env, user);
				ilpsched_node_attr_t *ua   = get_ilpsched_node_attr(node);

				/* skip already visited nodes */
				if (ua->consumer_idx == idx)
					continue;

				/* check if node has user in this block and collect the user if it's a data user */
				if (get_nodes_block(user) == block) {
					if (i == 0)
						ARR_APP1(ir_node *, consumer, user);
					has_block_user = 1;
				}

				/* only count data consumer */
				if (i == 0)
					n_consumer++;

				/* mark user visited by this node */
				ua->consumer_idx = idx;
			}
		}
	}

	block_node = get_ilpsched_irn(env, block);
	ba         = get_ilpsched_block_attr(block_node);

	ba->n_interesting_nodes++;

	/* current irn has no user inside this block, add to queue */
	if (! has_block_user) {
		DB((env->dbg, LEVEL_3, "root node\n"));
		plist_insert_back(ba->root_nodes, irn);
	}
	else {
		DB((env->dbg, LEVEL_3, "normal node\n"));
	}

	/* record number of all consumer and the consumer within the same block */
	node = get_ilpsched_irn(env, irn);
	na   = get_ilpsched_node_attr(node);
	na->n_consumer     = n_consumer;
	na->block_consumer = NEW_ARR_D(ir_node *, phase_obst(&env->ph), ARR_LEN(consumer));
	memcpy(na->block_consumer, consumer, ARR_LEN(consumer) * sizeof(na->block_consumer[0]));
	DEL_ARR_F(consumer);
}

/**
 * Calculate the ASAP scheduling step for current irn.
 */
static void calculate_irn_asap(ir_node *irn, void *walk_env) {
	be_ilpsched_env_t     *env = walk_env;
	int                   i;
	ir_node               *block;
	be_ilpsched_irn_t     *node, *block_node;
	ilpsched_node_attr_t  *na;
	ilpsched_block_attr_t *ba;

	/* These nodes are handled separate */
	if (! consider_for_sched(env->arch_env->isa, irn))
		return;

	DBG((env->dbg, LEVEL_2, "Calculating ASAP of node %+F ... ", irn));

	block    = get_nodes_block(irn);
	node     = get_ilpsched_irn(env, irn);
	na       = get_ilpsched_node_attr(node);
	na->asap = 1;

	for (i = get_irn_ins_or_deps(irn) - 1; i >= 0; --i) {
		ir_node *pred = skip_normal_Proj(env->arch_env->isa, get_irn_in_or_dep(irn, i));

		/* check for greatest distance to top */
		if (! is_Phi(pred) && ! is_NoMem(pred) && get_nodes_block(pred) == block) {
			be_ilpsched_irn_t    *pred_node = get_ilpsched_irn(env, pred);
			ilpsched_node_attr_t *pna       = get_ilpsched_node_attr(pred_node);
			unsigned             lat;

			lat         = fixed_latency(env->sel, pred, env->block_env);
			na->latency = lat;
			na->asap    = MAX(na->asap, pna->asap + lat);
		}
	}

	/* add node to ILP node list and update max_steps */
	block_node = get_ilpsched_irn(env, block);
	ba         = get_ilpsched_block_attr(block_node);

	set_irn_link(irn, ba->head_ilp_nodes);
	ba->head_ilp_nodes = irn;
	ba->max_steps     += fixed_latency(env->sel, irn, env->block_env);

	DB((env->dbg, LEVEL_2, "%u\n", na->asap));
}

/**
 * Calculate the ALAP scheduling step of all irns in current block.
 * Depends on max_steps being calculated.
 */
static void calculate_block_alap(ir_node *block, void *walk_env) {
	be_ilpsched_env_t     *env        = walk_env;
	be_ilpsched_irn_t     *block_node = get_ilpsched_irn(env, block);
	ilpsched_block_attr_t *ba         = get_ilpsched_block_attr(block_node);
	waitq                 *cur_queue  = new_waitq();
	plist_element_t       *el;

	assert(is_Block(block));

	DBG((env->dbg, LEVEL_2, "Calculating ALAP for nodes in %+F (%u nodes, %u max steps)\n",
		block, ba->n_interesting_nodes, ba->max_steps));

	/* TODO: Might be faster to use out edges and call phase_reinit_single_irn_data */
	//phase_reinit_block_irn_data(&env->ph, block);

	/* init start queue */
	foreach_plist(ba->root_nodes, el) {
		waitq_put(cur_queue, plist_element_get_value(el));
	}

	/* repeat until all nodes are processed */
	while (! waitq_empty(cur_queue)) {
		waitq *next_queue = new_waitq();

		/* process all nodes in current step */
		while (! waitq_empty(cur_queue)) {
			ir_node              *cur_irn = waitq_get(cur_queue);
			be_ilpsched_irn_t    *node    = get_ilpsched_irn(env, cur_irn);
			ilpsched_node_attr_t *na      = get_ilpsched_node_attr(node);
			int                  i;

			/* cur_node has no alap set -> it's a root node, set to max alap */
			if (na->alap == 0) {
				na->alap = ba->max_steps;
				DBG((env->dbg, LEVEL_2, "setting ALAP of node %+F to %u, handling preds:\n",
					cur_irn, na->alap));
			}
			else {
				DBG((env->dbg, LEVEL_2, "ALAP of node %+F is %u, handling preds:\n",
					cur_irn, na->alap));
			}

			/* set the alap's of all predecessors */
			for (i = get_irn_ins_or_deps(cur_irn) - 1; i >= 0; --i) {
				ir_node *pred = skip_normal_Proj(env->arch_env->isa, get_irn_in_or_dep(cur_irn, i));

				/* check for greatest distance to bottom */
				if (! is_Phi(pred) && ! is_NoMem(pred) && get_nodes_block(pred) == block) {
					be_ilpsched_irn_t    *pred_node = get_ilpsched_irn(env, pred);
					ilpsched_node_attr_t *pna       = get_ilpsched_node_attr(pred_node);
					unsigned             lat;

					/* mark the predecessor as visited by current irn */
					if (pna->visit_idx == get_irn_idx(cur_irn) && ! na->alap_changed)
						continue;
					pna->visit_idx = get_irn_idx(cur_irn);

					lat = fixed_latency(env->sel, pred, env->block_env);

					/* set ALAP of current pred */
					if (pna->alap == 0) {
						/* current ALAP is 0: node has not yet been visited */
						pna->alap_changed = 1;
						pna->alap         = na->alap - lat;
					}
					else if (pna->alap > na->alap - lat) {
						/* we found a longer path to root node: change ALAP */
						pna->alap         = na->alap - lat;
						pna->alap_changed = 1;
					}
					else {
						/* current ALAP is best found so far: keep it */
						pna->alap_changed = 0;
					}

					DBG((env->dbg, LEVEL_2, "\tsetting ALAP of node %+F to %u\n", pred, pna->alap));

					/* enqueue node for next iteration */
					if (get_irn_ins_or_deps(pred) > 0)
						waitq_put(next_queue, pred);
				}
			}
		}

		/* prepare for next iteration */
		del_waitq(cur_queue);
		cur_queue = next_queue;
	}
}

/**
 * We can free the list of root nodes here.
 */
static void clear_unwanted_data(ir_node *block, void *walk_env) {
	be_ilpsched_env_t     *env        = walk_env;
	be_ilpsched_irn_t     *block_node = get_ilpsched_irn(env, block);
	ilpsched_block_attr_t *ba         = get_ilpsched_block_attr(block_node);

	plist_free(ba->root_nodes);
	ba->root_nodes = NULL;
}

/**
 * Refine the {ASAP(n), ALAP(n)} interval for the nodes.
 * Set the ASAP/ALAP times of Projs and Keeps to their ancestor ones.
 */
static void refine_asap_alap_times(ir_node *irn, void *walk_env) {
	be_ilpsched_env_t    *env  = walk_env;
	ir_node              *pred = irn;
	be_ilpsched_irn_t    *node, *pred_node;
	ilpsched_node_attr_t *na, *pna;

	if (! consider_for_sched(env->arch_env->isa, irn))
		return;

	if (! is_Proj(irn) && ! be_is_Keep(irn))
		return;

	/* go to the ancestor */
	if (be_is_Keep(irn))
		pred = get_irn_n(irn, 0);
	pred = skip_Proj(pred);

	node      = get_ilpsched_irn(env, irn);
	pred_node = get_ilpsched_irn(env, pred);
	na        = get_ilpsched_node_attr(node);
	pna       = get_ilpsched_node_attr(pred_node);

	na->asap = pna->asap;
	na->alap = pna->alap;

	/* record all Projs and Keeps for this node */
	if (! pna->projkeeps)
		pna->projkeeps = new_waitq();
	waitq_put(pna->projkeeps, irn);

	DBG((env->dbg, LEVEL_2, "fixing ASAP/ALAP of %+F to %u/%u\n", irn, na->asap, na->alap));
}

/*******************************************
 *           _              _       _
 *          | |            | |     | |
 *  ___  ___| |__   ___  __| |_   _| | ___
 * / __|/ __| '_ \ / _ \/ _` | | | | |/ _ \
 * \__ \ (__| | | |  __/ (_| | |_| | |  __/
 * |___/\___|_| |_|\___|\__,_|\__,_|_|\___|
 *
 *******************************************/

static INLINE void check_for_keeps(waitq *keeps, ir_node *block, ir_node *irn) {
	const ir_edge_t *edge;

	foreach_out_edge(irn, edge) {
		ir_node *user = get_edge_src_irn(edge);

		if (be_is_Keep(user)) {
			assert(get_nodes_block(user) == block && "Keep must not be in different block.");
			waitq_put(keeps, user);
		}
	}
}

/**
 * Inserts @p irn before @p before into schedule and notifies backend.
 */
static INLINE void notified_sched_add_before(be_ilpsched_env_t *env,
	ir_node *before, ir_node *irn, unsigned cycle)
{
	be_ilp_sched_node_scheduled(env->sel, irn, cycle, env->block_env);
	sched_add_before(before, irn);
}

/**
 * Adds a node, it's Projs (in case of mode_T nodes) and
 * it's Keeps to schedule.
 */
static void add_to_sched(be_ilpsched_env_t *env, ir_node *block, ir_node *irn, unsigned cycle) {
	const ir_edge_t *edge;
	waitq           *keeps = new_waitq();

	/* mode_M nodes are not scheduled */
	if (get_irn_mode(irn) == mode_M)
		return;

	if (! sched_is_scheduled(irn))
		notified_sched_add_before(env, block, irn, cycle);

	/* add Projs */
	if (get_irn_mode(irn) == mode_T) {
		foreach_out_edge(irn, edge) {
			ir_node *user = get_edge_src_irn(edge);

			if (to_appear_in_schedule(user) || get_irn_mode(user) == mode_b)
				notified_sched_add_before(env, block, user, cycle);

			check_for_keeps(keeps, block, user);
		}
	}
	else {
		check_for_keeps(keeps, block, irn);
	}

	/* add Keeps */
	while (! waitq_empty(keeps)) {
		ir_node *keep = waitq_get(keeps);
		if (! sched_is_scheduled(keep))
			notified_sched_add_before(env, block, keep, cycle);
	}

	del_waitq(keeps);
}

/**
 * Schedule all nodes in the given block, according to the ILP solution.
 */
static void apply_solution(be_ilpsched_env_t *env, lpp_t *lpp, ir_node *block) {
	be_ilpsched_irn_t     *block_node = get_ilpsched_irn(env, block);
	ilpsched_block_attr_t *ba         = get_ilpsched_block_attr(block_node);
	sched_info_t          *info       = get_irn_sched_info(block);
	be_ilpsched_irn_t     **sched_nodes;
	unsigned              i, l;
	ir_node               *cfop, *irn;
	const ir_edge_t       *edge;

	/* init block schedule list */
	INIT_LIST_HEAD(&info->list);
	info->scheduled = 1;

	/* collect nodes and their scheduling time step */
	sched_nodes = NEW_ARR_F(be_ilpsched_irn_t *, 0);
	if (ba->n_interesting_nodes == 0) {
		/* ignore */
	}
	else if (ba->n_interesting_nodes == 1) {
		be_ilpsched_irn_t *node = get_ilpsched_irn(env, ba->head_ilp_nodes);

		/* add the single node */
		ARR_APP1(be_ilpsched_irn_t *, sched_nodes, node);
	}
	else {
		/* check all nodes for their positive solution */
		foreach_linked_irns(ba->head_ilp_nodes, irn) {
			be_ilpsched_irn_t    *node;
			ilpsched_node_attr_t *na;
			int                  tp_idx, found;
			unsigned             cur_var, t;

			node    = get_ilpsched_irn(env, irn);
			na      = get_ilpsched_node_attr(node);
			cur_var = 0;
			found   = 0;

			/* go over all variables of a node until the non-zero one is found */
			for (tp_idx = na->n_unit_types - 1; ! found && tp_idx >= 0; --tp_idx) {
				for (t = na->asap - 1; ! found && t <= na->alap - 1; ++t) {
					double val = lpp_get_var_sol(lpp, na->ilp_vars.x[cur_var++]);

					/* check, if variable is set to one (it's not zero then :) */
					if (! LPP_VALUE_IS_0(val)) {
						na->sched_point = t;
						ARR_APP1(be_ilpsched_irn_t *, sched_nodes, node);
						DBG((env->dbg, LEVEL_1, "Schedpoint of %+F is %u at unit type %s\n",
							irn, t, na->type_info[tp_idx].tp->name));
						found = 1;
					}
				}
			}
		}

		glob_heights = heights_new(env->irg);
		/* sort nodes ascending by scheduling time step */
		qsort(sched_nodes, ARR_LEN(sched_nodes), sizeof(sched_nodes[0]), cmp_ilpsched_irn);
		heights_free(glob_heights);
	}

	/* make all Phis ready and remember the single cf op */
	cfop = NULL;
	foreach_out_edge(block, edge) {
		irn = get_edge_src_irn(edge);

		switch (get_irn_opcode(irn)) {
			case iro_Phi:
				add_to_sched(env, block, irn, 0);
				break;
			case iro_Start:
			case iro_End:
			case iro_Proj:
			case iro_Bad:
				break;
			default:
				if (is_cfop(irn)) {
					assert(cfop == NULL && "Highlander - there can be only one");
					cfop = irn;
				}
			break;
		}
	}

	/* add all nodes from list */
	for (i = 0, l = ARR_LEN(sched_nodes); i < l; ++i) {
		ilpsched_node_attr_t *na = get_ilpsched_node_attr(sched_nodes[i]);
		add_to_sched(env, block, sched_nodes[i]->irn, na->sched_point);
	}

	/* schedule control flow node if not already done */
	if (cfop && ! sched_is_scheduled(cfop))
		add_to_sched(env, block, cfop, 0);

	DEL_ARR_F(sched_nodes);
}

/***************************************************************
 *   _____ _      _____     _____           _   _
 *  |_   _| |    |  __ \   / ____|         | | (_)
 *    | | | |    | |__) | | (___   ___  ___| |_ _  ___  _ __
 *    | | | |    |  ___/   \___ \ / _ \/ __| __| |/ _ \| '_ \
 *   _| |_| |____| |       ____) |  __/ (__| |_| | (_) | | | |
 *  |_____|______|_|      |_____/ \___|\___|\__|_|\___/|_| |_|
 *
 ***************************************************************/

/**
 * Check if node can be executed on given unit type.
 */
static INLINE int is_valid_unit_type_for_node(const be_execution_unit_type_t *tp, be_ilpsched_irn_t *node) {
	int                  i;
	ilpsched_node_attr_t *na = get_ilpsched_node_attr(node);

	for (i = na->n_unit_types - 1; i >= 0; --i) {
		if (na->type_info[i].tp == tp)
			return i;
	}

	return -1;
}

/************************************************
 *                   _       _     _
 *                  (_)     | |   | |
 *  __   ____ _ _ __ _  __ _| |__ | | ___  ___
 *  \ \ / / _` | '__| |/ _` | '_ \| |/ _ \/ __|
 *   \ V / (_| | |  | | (_| | |_) | |  __/\__ \
 *    \_/ \__,_|_|  |_|\__,_|_.__/|_|\___||___/
 *
 ************************************************/

/**
 * Create the following variables:
 * - x_{nt}^k    binary     weigthed with: t
 *      node n is scheduled at time step t to unit type k
 * ==>> These variables represent the schedule
 *
 * - d_{nt}^k    binary     weighted with: t
 *      node n dies at time step t on unit type k
 * - a_{nt}^k    binary     weighted with num_nodes
 *      node n is alive at time step t on unit type k
 *
 * - y_{nt}^k    binary     weighted with: num_nodes^2
 *      node n is scheduled at time step t to unit type k
 *      although all units of this type are occupied
 * ==>> These variables represent the register pressure
 *
 */
static void create_variables(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node, struct obstack *var_obst) {
	char                  buf[1024];
	ir_node               *irn;
	unsigned              num_block_var, num_nodes;
	ilpsched_block_attr_t *ba      = get_ilpsched_block_attr(block_node);
	unsigned              weigth_y = ba->n_interesting_nodes * ba->n_interesting_nodes;
#ifdef WITH_LIBCORE
	lc_timer_t            *t_var   = lc_timer_register("beilpsched_var", "create ilp variables");
#endif /* WITH_LIBCORE */

	ilp_timer_push(t_var);
	num_block_var = num_nodes = 0;
	foreach_linked_irns(ba->head_ilp_nodes, irn) {
		const be_execution_unit_t ***execunits = arch_isa_get_allowed_execution_units(env->arch_env->isa, irn);
		be_ilpsched_irn_t         *node;
		ilpsched_node_attr_t      *na;
		unsigned                  n_unit_types, tp_idx, unit_idx, n_var, cur_unit;
		unsigned                  cur_var_ad, cur_var_x, cur_var_y, num_ad;

		/* count number of available unit types for this node */
		for (n_unit_types = 0; execunits[n_unit_types]; ++n_unit_types)
			/* just count */ ;

		node = get_ilpsched_irn(env, irn);
		na   = get_ilpsched_node_attr(node);

		na->n_unit_types = n_unit_types;
		na->type_info    = NEW_ARR_D(unit_type_info_t, var_obst, n_unit_types);

		/* fill the type info array */
		for (tp_idx = 0; tp_idx < n_unit_types; ++tp_idx) {
			for (unit_idx = 0; execunits[tp_idx][unit_idx]; ++unit_idx) {
				/* beware: we also count number of available units here */
				if (be_machine_is_dummy_unit(execunits[tp_idx][unit_idx]))
					na->is_dummy_node = 1;
			}

			na->type_info[tp_idx].tp      = execunits[tp_idx][0]->tp;
			na->type_info[tp_idx].n_units = unit_idx;
		}

		/* allocate space for ilp variables */
		na->ilp_vars.x = NEW_ARR_D(int, var_obst, n_unit_types * VALID_SCHED_INTERVAL(na));
		memset(na->ilp_vars.x, -1, ARR_LEN(na->ilp_vars.x) * sizeof(na->ilp_vars.x[0]));

		/* we need these variables only for "real" nodes */
		if (! na->is_dummy_node) {
			na->ilp_vars.y = NEW_ARR_D(int, var_obst, n_unit_types * VALID_SCHED_INTERVAL(na));
			memset(na->ilp_vars.y, -1, ARR_LEN(na->ilp_vars.y) * sizeof(na->ilp_vars.y[0]));

			num_ad = ba->max_steps - na->asap + 1;

			if (ba->n_interesting_nodes > env->opts->limit_dead) {
				na->ilp_vars.a = NEW_ARR_D(int, var_obst, n_unit_types * num_ad);
				memset(na->ilp_vars.a, -1, ARR_LEN(na->ilp_vars.a) * sizeof(na->ilp_vars.a[0]));
			}
			else {
				na->ilp_vars.d = NEW_ARR_D(int, var_obst, n_unit_types * num_ad);
				memset(na->ilp_vars.d, -1, ARR_LEN(na->ilp_vars.d) * sizeof(na->ilp_vars.d[0]));
			}
		}

		DBG((env->dbg, LEVEL_3, "\thandling %+F (asap %u, alap %u, unit types %u):\n",
			irn, na->asap, na->alap, na->n_unit_types));

		cur_var_x = cur_var_ad = cur_var_y = cur_unit = n_var = 0;
		/* create variables */
		for (tp_idx = 0; tp_idx < n_unit_types; ++tp_idx) {
			unsigned t;

			for (t = na->asap - 1; t <= na->alap - 1; ++t) {
				/* x_{nt}^k variables */
				snprintf(buf, sizeof(buf), "x_n%u_%s_%u",
					get_irn_idx(irn), na->type_info[tp_idx].tp->name, t);
				na->ilp_vars.x[cur_var_x++] = lpp_add_var(lpp, buf, lpp_binary, (double)(t + 1));
				DBG((env->dbg, LEVEL_4, "\t\tcreated ILP variable %s\n", buf));
				/* variable counter */
				n_var++;
				num_block_var++;

				if (! na->is_dummy_node) {
					/* y_{nt}^k variables */
					snprintf(buf, sizeof(buf), "y_n%u_%s_%u",
						get_irn_idx(irn), na->type_info[tp_idx].tp->name, t);
					na->ilp_vars.y[cur_var_y++] = lpp_add_var(lpp, buf, lpp_binary, (double)(weigth_y));
					DBG((env->dbg, LEVEL_4, "\t\tcreated ILP variable %s\n", buf));

					/* variable counter */
					n_var++;
					num_block_var++;
				}
			}

			/* a node can die at any step t: asap(n) <= t <= U */
			if (! na->is_dummy_node) {
				for (t = na->asap - 1; t <= ba->max_steps; ++t) {

					if (ba->n_interesting_nodes > env->opts->limit_dead) {
						/* a_{nt}^k variables */
						snprintf(buf, sizeof(buf), "a_n%u_%s_%u",
							get_irn_idx(irn), na->type_info[tp_idx].tp->name, t);
						na->ilp_vars.a[cur_var_ad++] = lpp_add_var(lpp, buf, lpp_binary, (double)(ba->n_interesting_nodes));
					}
					else {
						/* d_{nt}^k variables */
						snprintf(buf, sizeof(buf), "d_n%u_%s_%u",
							get_irn_idx(irn), na->type_info[tp_idx].tp->name, t);
						na->ilp_vars.d[cur_var_ad++] = lpp_add_var(lpp, buf, lpp_binary, (double)(t + 1));
					}
					DBG((env->dbg, LEVEL_4, "\t\tcreated ILP variable %s\n", buf));

					/* variable counter */
					n_var++;
					num_block_var++;
				}
			}
		}

		DB((env->dbg, LEVEL_3, "%u variables created\n", n_var));
		num_nodes++;
	}
	ilp_timer_pop();
	DBG((env->dbg, LEVEL_1, "... %u variables for %u nodes created (%g sec)\n",
		num_block_var, num_nodes, ilp_timer_elapsed_usec(t_var) / 1000000.0));
}

/*******************************************************
 *                      _             _       _
 *                     | |           (_)     | |
 *   ___ ___  _ __  ___| |_ _ __ __ _ _ _ __ | |_ ___
 *  / __/ _ \| '_ \/ __| __| '__/ _` | | '_ \| __/ __|
 * | (_| (_) | | | \__ \ |_| | | (_| | | | | | |_\__ \
 *  \___\___/|_| |_|___/\__|_|  \__,_|_|_| |_|\__|___/
 *
 *******************************************************/

/**
 * Create following ILP constraints:
 * - the assignment constraints:
 *     assure each node is executed once by exactly one (allowed) execution unit
 * - the dead node assignment constraints:
 *     assure a node can only die at most once
 * - the precedence constraints:
 *     assure that no data dependencies are violated
 */
static void create_assignment_and_precedence_constraints(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node) {
	unsigned              num_cst_assign, num_cst_prec, num_cst_dead;
	char                  buf[1024];
	ir_node               *irn;
	ilpsched_block_attr_t *ba            = get_ilpsched_block_attr(block_node);
	bitset_t              *bs_block_irns = bitset_alloca(ba->block_last_idx);
#ifdef WITH_LIBCORE
	lc_timer_t            *t_cst_assign  = lc_timer_register("beilpsched_cst_assign",      "create assignment constraints");
	lc_timer_t            *t_cst_dead    = lc_timer_register("beilpsched_cst_assign_dead", "create dead node assignment constraints");
	lc_timer_t            *t_cst_prec    = lc_timer_register("beilpsched_cst_prec",        "create precedence constraints");
#endif /* WITH_LIBCORE */

	num_cst_assign = num_cst_prec = num_cst_dead = 0;
	foreach_linked_irns(ba->head_ilp_nodes, irn) {
		int                  cst, tp_idx, i;
		unsigned             cur_var;
		be_ilpsched_irn_t    *node;
		ilpsched_node_attr_t *na;

		node    = get_ilpsched_irn(env, irn);
		na      = get_ilpsched_node_attr(node);
		cur_var = 0;

		/* the assignment constraint */
		ilp_timer_push(t_cst_assign);
		snprintf(buf, sizeof(buf), "assignment_cst_n%u", get_irn_idx(irn));
		cst = lpp_add_cst_uniq(lpp, buf, lpp_equal, 1.0);
		DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
		num_cst_assign++;

		lpp_set_factor_fast_bulk(lpp, cst, na->ilp_vars.x, ARR_LEN(na->ilp_vars.x), 1.0);
		ilp_timer_pop();

		/* the dead node assignment constraint */
		if (! na->is_dummy_node && ba->n_interesting_nodes <= env->opts->limit_dead) {
			ilp_timer_push(t_cst_dead);
			snprintf(buf, sizeof(buf), "dead_node_assign_cst_n%u", get_irn_idx(irn));
			cst = lpp_add_cst_uniq(lpp, buf, lpp_less, 1.0);
			DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));

			lpp_set_factor_fast_bulk(lpp, cst, na->ilp_vars.d, ARR_LEN(na->ilp_vars.d), 1.0);
			ilp_timer_pop();
		}

		/* We have separate constraints for Projs and Keeps */
		// ILP becomes infeasible ?!?
//		if (is_Proj(irn) || be_is_Keep(irn))
//			continue;

		/* the precedence constraints */
		ilp_timer_push(t_cst_prec);
		bs_block_irns = bitset_clear_all(bs_block_irns);
		for (i = get_irn_ins_or_deps(irn) - 1; i >= 0; --i) {
			ir_node              *pred = skip_normal_Proj(env->arch_env->isa, get_irn_in_or_dep(irn, i));
			unsigned             t_low, t_high, t;
			be_ilpsched_irn_t    *pred_node;
			ilpsched_node_attr_t *pna;
			unsigned             delay;

			if (is_Phi(pred) || block_node->irn != get_nodes_block(pred) || is_NoMem(pred))
				continue;

			pred_node = get_ilpsched_irn(env, pred);
			pna       = get_ilpsched_node_attr(pred_node);

			assert(pna->asap > 0 && pna->alap >= pna->asap && "Invalid scheduling interval.");

			if (! bitset_is_set(bs_block_irns, pna->block_idx))
				bitset_set(bs_block_irns, pna->block_idx);
			else
				continue;

			/* irn = n, pred = m */
			delay  = fixed_latency(env->sel, pred, env->block_env);
			t_low  = MAX(na->asap, pna->asap + delay - 1);
			t_high = MIN(na->alap, pna->alap + delay - 1);
			for (t = t_low - 1; t <= t_high - 1; ++t) {
				unsigned tn, tm;
				int      *tmp_var_idx = NEW_ARR_F(int, 0);

				snprintf(buf, sizeof(buf), "precedence_n%u_n%u_%u", get_irn_idx(pred), get_irn_idx(irn), t);
				cst = lpp_add_cst_uniq(lpp, buf, lpp_less, 1.0);
				DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
				num_cst_prec++;

				/* lpp_set_factor_fast_bulk needs variables sorted ascending by index */
				if (na->ilp_vars.x[0] < pna->ilp_vars.x[0]) {
					/* node variables have smaller index than pred variables */
					for (tp_idx = na->n_unit_types - 1; tp_idx >= 0; --tp_idx) {
						for (tn = na->asap - 1; tn <= t; ++tn) {
							unsigned idx = ILPVAR_IDX(na, tp_idx, tn);
							ARR_APP1(int, tmp_var_idx, na->ilp_vars.x[idx]);
						}
					}

					for (tp_idx = pna->n_unit_types - 1; tp_idx >= 0; --tp_idx) {
						for (tm = t - delay + 1; tm < pna->alap; ++tm) {
							unsigned idx = ILPVAR_IDX(pna, tp_idx, tm);
							ARR_APP1(int, tmp_var_idx, pna->ilp_vars.x[idx]);
						}
					}
				}
				else {
					/* pred variables have smaller index than node variables */
					for (tp_idx = pna->n_unit_types - 1; tp_idx >= 0; --tp_idx) {
						for (tm = t - delay + 1; tm < pna->alap; ++tm) {
							unsigned idx = ILPVAR_IDX(pna, tp_idx, tm);
							ARR_APP1(int, tmp_var_idx, pna->ilp_vars.x[idx]);
						}
					}

					for (tp_idx = na->n_unit_types - 1; tp_idx >= 0; --tp_idx) {
						for (tn = na->asap - 1; tn <= t; ++tn) {
							unsigned idx = ILPVAR_IDX(na, tp_idx, tn);
							ARR_APP1(int, tmp_var_idx, na->ilp_vars.x[idx]);
						}
					}
				}

				if (ARR_LEN(tmp_var_idx) > 0)
					lpp_set_factor_fast_bulk(lpp, cst, tmp_var_idx, ARR_LEN(tmp_var_idx), 1.0);

				DEL_ARR_F(tmp_var_idx);
			}
		}
		ilp_timer_pop();
	}
	DBG((env->dbg, LEVEL_1, "\t%u assignement constraints (%g sec)\n",
		num_cst_assign, ilp_timer_elapsed_usec(t_cst_assign) / 1000000.0));
	DBG((env->dbg, LEVEL_1, "\t%u precedence constraints (%g sec)\n",
		num_cst_prec, ilp_timer_elapsed_usec(t_cst_prec) / 1000000.0));
}

/**
 * Create ILP resource constraints:
 * - assure that for each time step not more instructions are scheduled
 *   to the same unit types as units of this type are available
 */
static void create_ressource_constraints(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node) {
	int                   glob_type_idx;
	char                  buf[1024];
	unsigned              num_cst_resrc = 0;
	ilpsched_block_attr_t *ba           = get_ilpsched_block_attr(block_node);
#ifdef WITH_LIBCORE
	lc_timer_t            *t_cst_rsrc   = lc_timer_register("beilpsched_cst_rsrc",   "create resource constraints");
#endif /* WITH_LIBCORE */

	ilp_timer_push(t_cst_rsrc);
	for (glob_type_idx = env->cpu->n_unit_types - 1; glob_type_idx >= 0; --glob_type_idx) {
		unsigned                 t;
		be_execution_unit_type_t *cur_tp = &env->cpu->unit_types[glob_type_idx];

		/* BEWARE: the DUMMY unit type is not in CPU, so it's skipped automatically */

		/* check each time step */
		for (t = 0; t < ba->max_steps; ++t) {
			ir_node *irn;
			int     cst;
			int     *tmp_var_idx = NEW_ARR_F(int, 0);

			snprintf(buf, sizeof(buf), "resource_cst_%s_%u", cur_tp->name, t);
			cst = lpp_add_cst_uniq(lpp, buf, lpp_less, (double)cur_tp->n_units);
			DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
			num_cst_resrc++;

			foreach_linked_irns(ba->head_ilp_nodes, irn) {
				be_ilpsched_irn_t    *node = get_ilpsched_irn(env, irn);
				ilpsched_node_attr_t *na   = get_ilpsched_node_attr(node);
				int                  tp_idx;

				tp_idx = is_valid_unit_type_for_node(cur_tp, node);

				if (tp_idx >= 0 && t >= na->asap - 1 && t <= na->alap - 1) {
					int cur_var = ILPVAR_IDX(na, tp_idx, t);
					ARR_APP1(int, tmp_var_idx, na->ilp_vars.x[cur_var]);
				}
			}

			/* set constraints if we have some */
			if (ARR_LEN(tmp_var_idx) > 0)
				lpp_set_factor_fast_bulk(lpp, cst, tmp_var_idx, ARR_LEN(tmp_var_idx), 1.0);

			DEL_ARR_F(tmp_var_idx);
		}
	}
	ilp_timer_pop();
	DBG((env->dbg, LEVEL_1, "\t%u resource constraints (%g sec)\n",
		num_cst_resrc, ilp_timer_elapsed_usec(t_cst_rsrc) / 1000000.0));
}

/**
 * Create ILP bundle constraints:
 * - assure, at most bundle_size * bundles_per_cycle instructions
 *   can be started at a certain point.
 */
static void create_bundle_constraints(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node) {
	char                  buf[1024];
	unsigned              t;
	unsigned              num_cst_bundle = 0;
	unsigned              n_instr_max    = env->cpu->bundle_size * env->cpu->bundels_per_cycle;
	ilpsched_block_attr_t *ba            = get_ilpsched_block_attr(block_node);
#ifdef WITH_LIBCORE
	lc_timer_t            *t_cst_bundle  = lc_timer_register("beilpsched_cst_bundle", "create bundle constraints");
#endif /* WITH_LIBCORE */

	ilp_timer_push(t_cst_bundle);
	for (t = 0; t < ba->max_steps; ++t) {
		ir_node *irn;
		int     cst;
		int     *tmp_var_idx = NEW_ARR_F(int, 0);

		snprintf(buf, sizeof(buf), "bundle_cst_%u", t);
		cst = lpp_add_cst_uniq(lpp, buf, lpp_less, (double)n_instr_max);
		DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
		num_cst_bundle++;

		foreach_linked_irns(ba->head_ilp_nodes, irn) {
			be_ilpsched_irn_t    *node;
			ilpsched_node_attr_t *na;
			int                  tp_idx;

			/* Projs and Keeps do not contribute to bundle size */
			if (is_Proj(irn) || be_is_Keep(irn))
				continue;

			node = get_ilpsched_irn(env, irn);
			na   = get_ilpsched_node_attr(node);

			/* nodes assigned to DUMMY unit do not contribute to bundle size */
			if (na->is_dummy_node)
				continue;

			if (t >= na->asap - 1 && t <= na->alap - 1) {
				for (tp_idx = na->n_unit_types - 1; tp_idx >= 0; --tp_idx) {
					int idx = ILPVAR_IDX(na, tp_idx, t);
					ARR_APP1(int, tmp_var_idx, na->ilp_vars.x[idx]);
				}
			}
		}

		if (ARR_LEN(tmp_var_idx) > 0)
			lpp_set_factor_fast_bulk(lpp, cst, tmp_var_idx, ARR_LEN(tmp_var_idx), 1.0);

		DEL_ARR_F(tmp_var_idx);
	}
	ilp_timer_pop();
	DBG((env->dbg, LEVEL_1, "\t%u bundle constraints (%g sec)\n",
		num_cst_bundle, ilp_timer_elapsed_usec(t_cst_bundle) / 1000000.0));
}

/**
 * Create ILP dying nodes constraints:
 * - set variable d_{nt}^k to 1 if nodes n dies at step t on unit k
 */
static void create_dying_nodes_constraint(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node) {
	char                  buf[1024];
	unsigned              t;
	unsigned              num_cst = 0;
	ilpsched_block_attr_t *ba     = get_ilpsched_block_attr(block_node);
#ifdef WITH_LIBCORE
	lc_timer_t            *t_cst  = lc_timer_register("beilpsched_cst_dying_nodes", "create dying nodes constraints");
#endif /* WITH_LIBCORE */

	ilp_timer_push(t_cst);
	/* check all time_steps */
	for (t = 0; t < ba->max_steps; ++t) {
		ir_node *irn;

		/* for all nodes */
		foreach_linked_irns(ba->head_ilp_nodes, irn) {
			be_ilpsched_irn_t    *node = get_ilpsched_irn(env, irn);
			ilpsched_node_attr_t *na   = get_ilpsched_node_attr(node);

			/* if node has no consumer within current block, it cannot die here */
			/* we also ignore nodes assigned to dummy unit */
			if (ARR_LEN(na->block_consumer) < 1 || na->is_dummy_node)
				continue;

			/* node can only die here if t at least asap(n) */
			if (t >= na->asap - 1) {
				int node_tp_idx;

				/* for all unit types */
				for (node_tp_idx = na->n_unit_types - 1; node_tp_idx >= 0; --node_tp_idx) {
					int tp_idx, i, cst;
					int *tmp_var_idx = NEW_ARR_F(int, 0);

					snprintf(buf, sizeof(buf), "dying_node_cst_%u_n%u", t, get_irn_idx(irn));
					cst = lpp_add_cst_uniq(lpp, buf, lpp_less, (double)(na->n_consumer - 1));
					DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
					num_cst++;

					/* number of consumer scheduled till t */
					for (i = ARR_LEN(na->block_consumer) - 1; i >= 0; --i) {
						be_ilpsched_irn_t    *cons = get_ilpsched_irn(env, na->block_consumer[i]);
						ilpsched_node_attr_t *ca   = get_ilpsched_node_attr(cons);

						for (tp_idx = ca->n_unit_types - 1; tp_idx >= 0; --tp_idx) {
							unsigned tm;

							for (tm = ca->asap - 1; tm <= t && tm <= ca->alap - 1; ++tm) {
								int idx = ILPVAR_IDX(ca, tp_idx, tm);
								ARR_APP1(int, tmp_var_idx, ca->ilp_vars.x[idx]);
							}
						}
					}

					/* could be that no consumer can be scheduled at this point */
					if (ARR_LEN(tmp_var_idx)) {
						int      idx;
						unsigned tn;

						/* subtract possible prior kill points */
						for (tn = na->asap - 1; tn < t; ++tn) {
							idx = ILPVAR_IDX_DEAD(ba, na, node_tp_idx, tn);
							lpp_set_factor_fast(lpp, cst, na->ilp_vars.d[idx], -1.0);
						}

						idx = ILPVAR_IDX_DEAD(ba, na, node_tp_idx, t);
						lpp_set_factor_fast(lpp, cst, na->ilp_vars.d[idx], 0.0 - (double)(na->n_consumer));
						lpp_set_factor_fast_bulk(lpp, cst, tmp_var_idx, ARR_LEN(tmp_var_idx), 1.0);
					}

					DEL_ARR_F(tmp_var_idx);
				}
			}

		}
	}
	ilp_timer_pop();
	DBG((env->dbg, LEVEL_1, "\t%u dying nodes constraints (%g sec)\n",
		num_cst, ilp_timer_elapsed_usec(t_cst) / 1000000.0));
}

/**
* Create ILP alive nodes constraints:
* - set variable a_{nt}^k to 1 if nodes n is alive at step t on unit k
*/
static void create_alive_nodes_constraint(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node) {
	char                  buf[1024];
	ir_node               *irn;
	unsigned              num_cst = 0;
	ilpsched_block_attr_t *ba     = get_ilpsched_block_attr(block_node);
#ifdef WITH_LIBCORE
	lc_timer_t            *t_cst  = lc_timer_register("beilpsched_cst_alive_nodes", "create alive nodes constraints");
#endif /* WITH_LIBCORE */

	ilp_timer_push(t_cst);
	/* for each node */
	foreach_linked_irns(ba->head_ilp_nodes, irn) {
		be_ilpsched_irn_t    *node = get_ilpsched_irn(env, irn);
		ilpsched_node_attr_t *na   = get_ilpsched_node_attr(node);
		unsigned             t;

		/* we ignore nodes assigned to dummy unit here */
		if (na->is_dummy_node)
			continue;

		/* check check all time steps: asap(n) <= t <= U */
		for (t = na->asap - 1; t < ba->max_steps; ++t) {
			int node_tp_idx;

			/* for all unit types available for this node */
			for (node_tp_idx = na->n_unit_types - 1; node_tp_idx >= 0; --node_tp_idx) {
				unsigned tn, tn_max, idx;
				int      cst, i;
				int      *tmp_var_idx_n = NEW_ARR_F(int, 0);
				int      *tmp_var_idx_m = NEW_ARR_F(int, 0);

				snprintf(buf, sizeof(buf), "alive_node_cst_%u_n%u_%s",
					t, get_irn_idx(irn), na->type_info[node_tp_idx].tp->name);
				cst = lpp_add_cst_uniq(lpp, buf, lpp_less, 0.0);
				DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
				num_cst++;

				tn_max = MIN(na->alap - 1, t);
				/* check if the node has been scheduled so far */
				for (tn = na->asap - 1; tn <= tn_max; ++tn) {
					int idx = ILPVAR_IDX(na, node_tp_idx, tn);
					ARR_APP1(int, tmp_var_idx_n, na->ilp_vars.x[idx]);
				}

				if (ARR_LEN(tmp_var_idx_n) > 0)
					lpp_set_factor_fast_bulk(lpp, cst, tmp_var_idx_n, ARR_LEN(tmp_var_idx_n), (double)(na->n_consumer));
				DEL_ARR_F(tmp_var_idx_n);

				/* subtract the number of consumer scheduled so far */
				for (i = ARR_LEN(na->block_consumer) - 1; i >= 0; --i) {
					be_ilpsched_irn_t    *cons = get_ilpsched_irn(env, na->block_consumer[i]);
					ilpsched_node_attr_t *ca   = get_ilpsched_node_attr(cons);
					int                  tp_idx;
					unsigned             tm, tm_max;

					tm_max = MIN(ca->alap - 1, t);
					for (tp_idx = ca->n_unit_types - 1; tp_idx >= 0; --tp_idx) {
						for (tm = ca->asap - 1; tm <= tm_max; ++tm) {
							int idx = ILPVAR_IDX(ca, tp_idx, tm);
							ARR_APP1(int, tmp_var_idx_m, ca->ilp_vars.x[idx]);
						}
					}
				}

				if (ARR_LEN(tmp_var_idx_m) > 0)
					lpp_set_factor_fast_bulk(lpp, cst, tmp_var_idx_m, ARR_LEN(tmp_var_idx_m), -1.0);
				DEL_ARR_F(tmp_var_idx_m);

				/* -c * a_{nt}^k */
				idx = ILPVAR_IDX_DEAD(ba, na, node_tp_idx, t);
				lpp_set_factor_fast(lpp, cst, na->ilp_vars.a[idx], 0.0 - (double)(na->n_consumer));

			}
		}
	}
	ilp_timer_pop();
	DBG((env->dbg, LEVEL_1, "\t%u alive nodes constraints (%g sec)\n",
		num_cst, ilp_timer_elapsed_usec(t_cst) / 1000000.0));
}

/**
 * Create ILP pressure constraints, based on dead nodes:
 * - add additional costs to objective function if a node is scheduled
 *   on a unit although all units of this type are currently occupied
 */
static void create_pressure_dead_constraint(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node) {
	char                  buf[1024];
	ir_node               *cur_irn;
	unsigned              num_cst = 0;
	ilpsched_block_attr_t *ba     = get_ilpsched_block_attr(block_node);
#ifdef WITH_LIBCORE
	lc_timer_t            *t_cst  = lc_timer_register("beilpsched_cst_pressure", "create pressure constraints");
#endif /* WITH_LIBCORE */

	ilp_timer_push(t_cst);
	/* y_{nt}^k is set for each node and timestep and unit type */
	foreach_linked_irns(ba->head_ilp_nodes, cur_irn) {
		unsigned             cur_idx   = get_irn_idx(cur_irn);
		be_ilpsched_irn_t    *cur_node = get_ilpsched_irn(env, cur_irn);
		ilpsched_node_attr_t *cur_na   = get_ilpsched_node_attr(cur_node);
		int                  glob_type_idx;

		/* we ignore nodes assigned to DUMMY unit here */
		if (cur_na->is_dummy_node)
			continue;

		/* for all types */
		for (glob_type_idx = env->cpu->n_unit_types - 1; glob_type_idx >= 0; --glob_type_idx) {
			be_execution_unit_type_t *cur_tp   = &env->cpu->unit_types[glob_type_idx];
			int                      cur_tp_idx;
			unsigned                 t;

			/* BEWARE: the DUMMY unit types is not in CPU, so it's skipped automatically */

			/* check if node can be executed on this unit type */
			cur_tp_idx = is_valid_unit_type_for_node(cur_tp, cur_node);
			if (cur_tp_idx < 0)
				continue;

			/* check all time_steps */
			for (t = cur_na->asap - 1; t <= cur_na->alap - 1; ++t) {
				int     cst, y_idx;
				ir_node *irn;
				int     *tmp_idx_1  = NEW_ARR_F(int, 0);
				int     *tmp_idx_m1 = NEW_ARR_F(int, 0);

				snprintf(buf, sizeof(buf), "pressure_cst_n%u_%u_%s", cur_idx, t, cur_tp->name);
				cst = lpp_add_cst_uniq(lpp, buf, lpp_less, (double)(cur_tp->n_units - 1));
				DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
				num_cst++;

				/*
					- accumulate all nodes scheduled on unit type k till t
					- subtract all nodes died on unit type k till t
				*/
				foreach_linked_irns(ba->head_ilp_nodes, irn) {
					be_ilpsched_irn_t    *node = get_ilpsched_irn(env, irn);
					ilpsched_node_attr_t *na   = get_ilpsched_node_attr(node);
					unsigned             tn, tmax;
					int                  tp_idx;

					tmax   = MIN(t, na->alap - 1);
					tp_idx = is_valid_unit_type_for_node(cur_tp, node);

					/* current unit type is not suitable for current node */
					if (tp_idx < 0)
						continue;

					for (tn = na->asap - 1; tn <= tmax; ++tn) {
						int idx;

						/* node scheduled */
						idx = ILPVAR_IDX(na, tp_idx, tn);
						ARR_APP1(int, tmp_idx_1, na->ilp_vars.x[idx]);

						/* node dead */
						idx = ILPVAR_IDX_DEAD(ba, na, tp_idx, tn);
						ARR_APP1(int, tmp_idx_m1, na->ilp_vars.d[idx]);
					}
				}

				if (ARR_LEN(tmp_idx_1) > 0)
					lpp_set_factor_fast_bulk(lpp, cst, tmp_idx_1, ARR_LEN(tmp_idx_1), 1.0);

				if (ARR_LEN(tmp_idx_m1) > 0)
					lpp_set_factor_fast_bulk(lpp, cst, tmp_idx_m1, ARR_LEN(tmp_idx_m1), -1.0);

				/* BEWARE: t is unsigned, so (double)(-t) won't work */
				y_idx = ILPVAR_IDX(cur_na, cur_tp_idx, t);
				lpp_set_factor_fast(lpp, cst, cur_na->ilp_vars.y[y_idx], 0.0 - (double)(t));

				DEL_ARR_F(tmp_idx_1);
				DEL_ARR_F(tmp_idx_m1);
			}
		}
	}
	ilp_timer_pop();
	DBG((env->dbg, LEVEL_1, "\t%u pressure constraints (%g sec)\n",
		num_cst, ilp_timer_elapsed_usec(t_cst) / 1000000.0));
}

/**
 * Create ILP pressure constraints, based on alive nodes:
 * - add additional costs to objective function if a node is scheduled
 *   on a unit although all units of this type are currently occupied
 */
static void create_pressure_alive_constraint(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node) {
	char                  buf[1024];
	ir_node               *cur_irn;
	unsigned              num_cst = 0;
	ilpsched_block_attr_t *ba     = get_ilpsched_block_attr(block_node);
#ifdef WITH_LIBCORE
	lc_timer_t            *t_cst  = lc_timer_register("beilpsched_cst_pressure", "create pressure constraints");
#endif /* WITH_LIBCORE */

	ilp_timer_push(t_cst);
	/* y_{nt}^k is set for each node and timestep and unit type */
	foreach_linked_irns(ba->head_ilp_nodes, cur_irn) {
		unsigned             cur_idx   = get_irn_idx(cur_irn);
		be_ilpsched_irn_t    *cur_node = get_ilpsched_irn(env, cur_irn);
		ilpsched_node_attr_t *cur_na   = get_ilpsched_node_attr(cur_node);
		int                  glob_type_idx;

		/* we ignore nodes assigned to DUMMY unit here */
		if (cur_na->is_dummy_node)
			continue;

		/* for all types */
		for (glob_type_idx = env->cpu->n_unit_types - 1; glob_type_idx >= 0; --glob_type_idx) {
			be_execution_unit_type_t *cur_tp   = &env->cpu->unit_types[glob_type_idx];
			int                      cur_tp_idx;
			unsigned                 t;

			/* BEWARE: the DUMMY unit types is not in CPU, so it's skipped automatically */

			/* check if node can be executed on this unit type */
			cur_tp_idx = is_valid_unit_type_for_node(cur_tp, cur_node);
			if (cur_tp_idx < 0)
				continue;

			/* check all time_steps at which the current node can be scheduled */
			for (t = cur_na->asap - 1; t <= cur_na->alap - 1; ++t) {
				int     cst, y_idx;
				ir_node *irn;
				int     *tmp_var_idx = NEW_ARR_F(int, 0);

				snprintf(buf, sizeof(buf), "pressure_cst_n%u_%u_%s", cur_idx, t, cur_tp->name);
				cst = lpp_add_cst_uniq(lpp, buf, lpp_less, (double)(cur_tp->n_units - 1));
				DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
				num_cst++;

				/* - accumulate all nodes alive at point t on unit type k */
				foreach_linked_irns(ba->head_ilp_nodes, irn) {
					be_ilpsched_irn_t    *node = get_ilpsched_irn(env, irn);
					ilpsched_node_attr_t *na   = get_ilpsched_node_attr(node);
					int                  a_idx, tp_idx;

					/* check if node can be alive here */
					if (t < na->asap - 1)
						continue;

					tp_idx = is_valid_unit_type_for_node(cur_tp, node);

					/* current type is not suitable */
					if (tp_idx < 0)
						continue;

					a_idx = ILPVAR_IDX_DEAD(ba, na, tp_idx, t);
					ARR_APP1(int, tmp_var_idx, na->ilp_vars.a[a_idx]);
				}

				if (ARR_LEN(tmp_var_idx) > 0)
					lpp_set_factor_fast_bulk(lpp, cst, tmp_var_idx, ARR_LEN(tmp_var_idx), 1.0);
				DEL_ARR_F(tmp_var_idx);

				/* - num_nodes * y_{nt}^k */
				y_idx = ILPVAR_IDX(cur_na, cur_tp_idx, t);
				lpp_set_factor_fast(lpp, cst, cur_na->ilp_vars.y[y_idx], 0.0 - (double)(ba->n_interesting_nodes));
			}
		}
	}
	ilp_timer_pop();
	DBG((env->dbg, LEVEL_1, "\t%u pressure constraints (%g sec)\n",
		num_cst, ilp_timer_elapsed_usec(t_cst) / 1000000.0));
}

#if 0
static void create_proj_keep_constraints(be_ilpsched_env_t *env, lpp_t *lpp, be_ilpsched_irn_t *block_node) {
	char                  buf[1024];
	ir_node               *irn;
	unsigned              num_cst = 0;
	ilpsched_block_attr_t *ba     = get_ilpsched_block_attr(block_node);
#ifdef WITH_LIBCORE
	lc_timer_t            *t_cst  = lc_timer_register("beilpsched_cst_projkeep", "create proj and keep constraints");
#endif /* WITH_LIBCORE */

	ilp_timer_push(t_cst);
	/* check all nodes */
	foreach_linked_irns(ba->head_ilp_nodes, irn) {
		be_ilpsched_irn_t    *node;
		ilpsched_node_attr_t *na;
		unsigned             t;
		ir_node              **pk;

		/* only mode_T nodes can have Projs and Keeps assigned */
		if (get_irn_mode(irn) != mode_T)
			continue;

		node = get_ilpsched_irn(env, irn);
		na   = get_ilpsched_node_attr(node);

		/* check if has some Projs and Keeps assigned */
		if (! na->projkeeps)
			continue;

		/* we can run only once over the queue, so preserve the nodes */
		pk = NEW_ARR_F(ir_node *, 0);
		while (! waitq_empty(na->projkeeps))
			ARR_APP1(ir_node *, pk, waitq_get(na->projkeeps));
		del_waitq(na->projkeeps);
		na->projkeeps = NULL;

		/* for all time steps at which this node can be scheduled */
		for (t = na->asap - 1; t <= na->alap - 1; ++t) {
			int cst, tp_idx, i;
			int *tmp_var_idx_n = NEW_ARR_F(int, 0);

			/* add the constraint, assure, that a node is always scheduled along with it's Projs and Keeps */
			snprintf(buf, sizeof(buf), "projkeep_cst_n%u_%u", get_irn_idx(irn), t);
			cst = lpp_add_cst_uniq(lpp, buf, lpp_equal, 0.0);
			DBG((env->dbg, LEVEL_2, "added constraint %s\n", buf));
			num_cst++;

			/* sum up scheduling variables for this time step */
			for (tp_idx = na->n_unit_types - 1; tp_idx >= 0; --tp_idx) {
				int idx = ILPVAR_IDX(na, tp_idx, t);
				ARR_APP1(int, tmp_var_idx_n, na->ilp_vars.x[idx]);
			}

			if (ARR_LEN(tmp_var_idx_n) > 0)
				lpp_set_factor_fast_bulk(lpp, cst, tmp_var_idx_n, ARR_LEN(tmp_var_idx_n), (double)(ARR_LEN(pk)));
			DEL_ARR_F(tmp_var_idx_n);

			/* subtract all Proj and Keep variables for this step */
			for (i = ARR_LEN(pk) - 1; i >= 0; --i) {
				be_ilpsched_irn_t    *pk_node = get_ilpsched_irn(env, pk[i]);
				ilpsched_node_attr_t *pk_na   = get_ilpsched_node_attr(pk_node);
				int                  pk_tp_idx;

				for (pk_tp_idx = pk_na->n_unit_types - 1; pk_tp_idx >= 0; --pk_tp_idx) {
					int idx = ILPVAR_IDX(pk_na, pk_tp_idx, t);
					lpp_set_factor_fast(lpp, cst, pk_na->ilp_vars.x[idx], -1.0);
				}
			}
		}
	}
	ilp_timer_pop();
	DBG((env->dbg, LEVEL_1, "\t%u Proj and Keep constraints (%g sec)\n",
		num_cst, ilp_timer_elapsed_usec(t_cst) / 1000000.0));
}
#endif

/***************************************************
 *  _____ _      _____                    _
 * |_   _| |    |  __ \                  (_)
 *   | | | |    | |__) |  _ __ ___   __ _ _ _ __
 *   | | | |    |  ___/  | '_ ` _ \ / _` | | '_ \
 *  _| |_| |____| |      | | | | | | (_| | | | | |
 * |_____|______|_|      |_| |_| |_|\__,_|_|_| |_|
 *
 ***************************************************/

/**
 * Create the ilp (add variables, build constraints, solve, build schedule from solution).
 */
static void create_ilp(ir_node *block, void *walk_env) {
	be_ilpsched_env_t     *env           = walk_env;
	be_ilpsched_irn_t     *block_node    = get_ilpsched_irn(env, block);
	ilpsched_block_attr_t *ba            = get_ilpsched_block_attr(block_node);
	FILE                  *logfile       = NULL;
	lpp_t                 *lpp           = NULL;
	struct obstack        var_obst;

	DBG((env->dbg, 255, "\n\n\n=========================================\n"));
	DBG((env->dbg, 255, "  ILP Scheduling for %+F\n", block));
	DBG((env->dbg, 255, "=========================================\n\n"));

	DBG((env->dbg, LEVEL_1, "Creating ILP Variables for nodes in %+F (%u interesting nodes, %u max steps)\n",
		block, ba->n_interesting_nodes, ba->max_steps));

	/* notify backend and get block environment */
	env->block_env = be_ilp_sched_init_block_ilp_schedule(env->sel, block);

	/* if we have less than two interesting nodes, there is no need to create the ILP */
	if (ba->n_interesting_nodes > 1) {
		double fact_var        = ba->n_interesting_nodes > 25 ? 1.1 : 1.2;
		double fact_cst        = ba->n_interesting_nodes > 25 ? 0.7 : 1.5;
		int    base_num        = ba->n_interesting_nodes * ba->n_interesting_nodes;
		int    estimated_n_var = (int)((double)base_num * fact_var);
		int    estimated_n_cst = (int)((double)base_num * fact_cst);

		DBG((env->dbg, LEVEL_1, "Creating LPP with estimed numbers: %d vars, %d cst\n",
			estimated_n_var, estimated_n_cst));

		/* set up the LPP object */
		lpp = new_lpp_userdef(
			"be ilp scheduling",
			lpp_minimize,
			estimated_n_cst + 1,  /* num vars */
			estimated_n_cst + 20, /* num cst */
			1.2);                 /* grow factor */
		obstack_init(&var_obst);

		/* create ILP variables */
		create_variables(env, lpp, block_node, &var_obst);

		/* create ILP constraints */
		DBG((env->dbg, LEVEL_1, "Creating constraints for nodes in %+F:\n", block));
		create_assignment_and_precedence_constraints(env, lpp, block_node);
		create_ressource_constraints(env, lpp, block_node);
		create_bundle_constraints(env, lpp, block_node);
		//create_proj_keep_constraints(env, lpp, block_node);
		if (ba->n_interesting_nodes > env->opts->limit_dead) {
			create_alive_nodes_constraint(env, lpp, block_node);
			create_pressure_alive_constraint(env, lpp, block_node);
		} else {
			create_dying_nodes_constraint(env, lpp, block_node);
			create_pressure_dead_constraint(env, lpp, block_node);
		}

		DBG((env->dbg, LEVEL_1, "ILP to solve: %u variables, %u constraints\n", lpp->var_next, lpp->cst_next));

		/* debug stuff, dump lpp when debugging is on  */
		DEBUG_ONLY(
			if (firm_dbg_get_mask(env->dbg) > 0) {
				char buf[1024];
				FILE *f;

				snprintf(buf, sizeof(buf), "lpp_block_%lu.txt", get_irn_node_nr(block));
				f = fopen(buf, "w");
				lpp_dump_plain(lpp, f);
				fclose(f);
				snprintf(buf, sizeof(buf), "lpp_block_%lu.mps", get_irn_node_nr(block));
				lpp_dump(lpp, buf);
			}
		);

		/* set solve time limit */
		lpp_set_time_limit(lpp, env->opts->time_limit);

		/* set logfile if requested */
		if (strlen(env->opts->log_file) > 0) {
			if (strcasecmp(env->opts->log_file, "stdout") == 0)
				lpp_set_log(lpp, stdout);
			else if (strcasecmp(env->opts->log_file, "stderr") == 0)
				lpp_set_log(lpp, stderr);
			else {
				logfile = fopen(env->opts->log_file, "w");
				if (! logfile)
					fprintf(stderr, "Could not open logfile '%s'! Logging disabled.\n", env->opts->log_file);
				else
					lpp_set_log(lpp, logfile);
			}
		}

		/* solve the ILP */
		lpp_solve_net(lpp, env->main_env->options->ilp_server, env->main_env->options->ilp_solver);

		if (logfile)
			fclose(logfile);

		/* check for valid solution */
		if (! lpp_is_sol_valid(lpp)) {
			char buf[1024];
			FILE *f;

			snprintf(buf, sizeof(buf), "lpp_block_%lu.assert.txt", get_irn_node_nr(block));
			f = fopen(buf, "w");
			lpp_dump_plain(lpp, f);
			fclose(f);
			snprintf(buf, sizeof(buf), "lpp_block_%lu.assert.mps", get_irn_node_nr(block));
			lpp_dump(lpp, buf);
			dump_ir_block_graph(env->irg, "-assert");

			assert(0 && "ILP solution is not feasible!");
		}

		DBG((env->dbg, LEVEL_1, "\nSolution:\n"));
		DBG((env->dbg, LEVEL_1, "\tsend time: %g sec\n", lpp->send_time / 1000000.0));
		DBG((env->dbg, LEVEL_1, "\treceive time: %g sec\n", lpp->recv_time / 1000000.0));
		DBG((env->dbg, LEVEL_1, "\titerations: %d\n", lpp->iterations));
		DBG((env->dbg, LEVEL_1, "\tsolution time: %g\n", lpp->sol_time));
		DBG((env->dbg, LEVEL_1, "\tobjective function: %g\n", LPP_VALUE_IS_0(lpp->objval) ? 0.0 : lpp->objval));
		DBG((env->dbg, LEVEL_1, "\tbest bound: %g\n", LPP_VALUE_IS_0(lpp->best_bound) ? 0.0 : lpp->best_bound));

		DBG((env->dbg, LEVEL_1, "variables used %u bytes\n", obstack_memory_used(&var_obst)));
	}

	/* apply solution */
	apply_solution(env, lpp, block);

	if (lpp)
		free_lpp(lpp);

	/* notify backend */
	be_ilp_sched_finish_block_ilp_schedule(env->sel, block, env->block_env);
}

/**
 * Perform ILP scheduling on the given irg.
 */
void be_ilp_sched(const be_irg_t *birg) {
	be_ilpsched_env_t          env;
	const char                 *name = "be ilp scheduling";
	arch_isa_t                 *isa  = birg->main_env->arch_env->isa;
	const ilp_sched_selector_t *sel  = isa->impl->get_ilp_sched_selector(isa);

	FIRM_DBG_REGISTER(env.dbg, "firm.be.sched.ilp");

	//firm_dbg_set_mask(env.dbg, 31);

	env.irg_env    = be_ilp_sched_init_irg_ilp_schedule(sel, birg->irg);
	env.sel        = sel;
	env.irg        = birg->irg;
	env.height     = heights_new(birg->irg);
	env.main_env   = birg->main_env;
	env.arch_env   = birg->main_env->arch_env;
	env.cpu        = arch_isa_get_machine(birg->main_env->arch_env->isa);
	env.opts       = &ilp_opts;
	phase_init(&env.ph, name, env.irg, PHASE_DEFAULT_GROWTH, init_ilpsched_irn);

	/* assign a unique per block number to all interesting nodes */
	irg_walk_in_or_dep_graph(env.irg, NULL, build_block_idx, &env);

	/*
		The block indices are completely build after the walk,
		now we can allocate the bitsets (size depends on block indices)
		for all nodes.
	*/
	phase_reinit_irn_data(&env.ph);

	/* Collect all root nodes (having no user in their block) and calculate ASAP. */
	irg_walk_in_or_dep_blkwise_graph(env.irg, collect_alap_root_nodes, calculate_irn_asap, &env);

	/* Calculate ALAP of all irns */
	irg_block_walk_graph(env.irg, NULL, calculate_block_alap, &env);

	/* We refine the {ASAP(n), ALAP(n)} interval and fix the time steps for Projs and Keeps */
	irg_walk_in_or_dep_blkwise_graph(env.irg, NULL, refine_asap_alap_times, &env);

	/* we don't need this information any longer */
	heights_free(env.height);

	/* perform ILP scheduling */
	irg_block_walk_graph(env.irg, clear_unwanted_data, create_ilp, &env);

	DEBUG_ONLY(
		if (firm_dbg_get_mask(env.dbg)) {
			phase_stat_t stat;
			phase_stat_t *stat_ptr = phase_stat(&env.ph, &stat);

			fprintf(stderr, "Phase used: %u bytes\n", stat_ptr->overall_bytes);
		}
	);

	/* free all allocated object */
	phase_free(&env.ph);

	/* notify backend */
	be_ilp_sched_finish_irg_ilp_schedule(sel, birg->irg, env.irg_env);
}

#ifdef WITH_LIBCORE
/**
 * Register ILP scheduler options.
 */
void be_init_ilpsched(void)
{
	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *sched_grp = lc_opt_get_grp(be_grp, "ilpsched");

	lc_opt_add_table(sched_grp, ilpsched_option_table);
}
#endif /* WITH_LIBCORE */

#else /* WITH_ILP */

static INLINE void some_picky_compiler_do_not_allow_empty_files(void)
{}

#endif /* WITH_ILP */
