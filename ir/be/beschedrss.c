/**
 * Implementation of a register saturating list scheduler
 * as described in: Sid-Ahmed-Ali Touati
 * Register Saturation in Superscalar and VLIW Codes
 *
 * @license This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 * @author  Christian Wuerdig
 * @date    29.08.2006
 * @cvs-id  $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>

#include "obst.h"
#include "debug.h"

#include "irgraph_t.h"
#include "irnode_t.h"
#include "iredges_t.h"
#include "ircons_t.h"
#include "irphase_t.h"
#include "irgwalk.h"
#include "irtools.h"
#include "irbitset.h"
#include "irprintf.h"
#include "bipartite.h"
#include "hungarian.h"
#include "plist.h"

#include "height.h"

#include "beabi.h"
#include "benode_t.h"
#include "besched_t.h"
#include "beschedmris.h"

#define DEBUG_NODEINFO  1 << 0
#define DEBUG_PKILL     1 << 1
#define DEBUG_BIPARTITE 1 << 2
#define DEBUG_SKS       1 << 3
#define DEBUG_DVG       1 << 4
#define DEBUG_SER_HEUR  1 << 5
#define DEBUG_MAX_AC    1 << 6

#define HASH_RSS_EDGE(edge) ((get_irn_node_nr((edge)->src) << 16) | (get_irn_node_nr((edge)->tgt) & 0xFFFF))
#define BSEARCH_IRN_ARR(val, arr) \
	bsearch(&(val), (arr), ARR_LEN((arr)), sizeof((arr)[0]), cmp_irn_idx)

#define BLOCK_IDX_MAP(rss, irn) bsearch_for_index(get_irn_idx((irn)), (rss)->idx_map, ARR_LEN((rss)->idx_map), 1)

/* Represents a child with associated costs */
typedef struct _child {
	ir_node *irn;
	float   cost;
} child_t;

/* We need edges for several purposes. */
typedef struct _rss_edge {
	ir_node *src;
	ir_node *tgt;
	void    *next;
} rss_edge_t;

/* Represents a connected bipartite component. */
typedef struct _cbc {
	nodeset *parents;       /**< = S  a set of value producers */
	nodeset *children;      /**< = T  a set of value consumers */
	pset    *kill_edges;    /**< = E  a set of edges (t in T, s in S) such as each s in S gets killed by at least one t in T */
	int     nr;             /**< a deterministic index for set insertion (used as hash) */
} cbc_t;

/* Represents a serialization edge with associated costs. */
typedef struct _serialization {
	rss_edge_t *edge;
	int        omega1;
	int        omega2;
} serialization_t;

/* Represents a disjoint value DAG. */
typedef struct _dvg {
	nodeset *nodes;
	pset    *edges;
} dvg_t;

/* Represents a chain of nodes. */
typedef struct _chain {
	plist_t *elements;   /**< List of chain elements */
	int     nr;          /**< a deterministic index for set insertion (used as hash) */
} chain_t;

typedef struct _rss_irn {
	plist_t  *consumer_list;    /**< List of consumers */
	ir_node **consumer;         /**< Sorted consumer array (needed for faster access) */

	plist_t  *parent_list;      /**< List of parents */
	ir_node **parents;          /**< Sorted parent array (needed for faster access) */

	plist_t  *descendant_list;  /**< List of descendants */
	ir_node **descendants;      /**< Sorted descendant array (needed for faster access) */

	plist_t  *pkiller_list;     /**< List of potential killers */
	ir_node **pkillers;         /**< Sorted pkiller array (needed for faster access) */

	plist_t  *dvg_desc_list;    /**< List of all descendants in the DVG */
	ir_node **dvg_desc;         /**< Sorted dvg descendant array (needed for faster access) */

	plist_t  *dvg_pkiller_list; /**< List of potential killers in the DVG */
	ir_node **dvg_pkiller;      /**< Sorted dvg pkiller array (needed for faster access) */

	plist_t  *kill_value_list;  /**< List of values getting potentially killed by this node */
	plist_t  *dvg_user_list;    /**< List of users in the disjoint value DAG DVG */

	ir_node  *killer;           /**< The selected unique killer */
	ir_node  *irn;              /**< The corresponding firm node to this rss_irn */

	chain_t  *chain;            /**< The chain, this node is associated to */

	unsigned live_out : 1;      /**< irn has consumers outside of it's block */
	unsigned visited  : 1;      /**< visited flag for bipartite decomposition */
	unsigned handled  : 1;      /**< flag indicating whether or not the list structures have been build */
	unsigned dumped   : 1;      /**< flag indication whether or not this node was dumped */
} rss_irn_t;

typedef struct _rss {
	phase_t          ph;              /**< Phase to hold some data */
	heights_t        *h;              /**< The current height object */
	ir_graph         *irg;            /**< The irg to preprocess */
	plist_t          *nodes;          /**< The list of interesting nodes */
	const arch_env_t *arch_env;       /**< The architecture environment */
	be_abi_irg_t     *abi;            /**< The abi for this irg */
	pset             *cbc_set;        /**< A set of connected bipartite components */
	ir_node          *block;          /**< The current block in progress. */
	int              *idx_map;        /**< Mapping irn indices to per block indices */
	unsigned         max_height;      /**< maximum height in the current block */
	const arch_register_class_t *cls; /**< The current register class */
	DEBUG_ONLY(firm_dbg_module_t *dbg);
} rss_t;

#define get_rss_irn(rss, irn)  (phase_get_or_set_irn_data(&rss->ph, irn))

/**
 * We need some special nodes:
 * a source and a sink for all live-in and live-out values of a block
 */

static enum {
	iro_rss_Source,
	iro_rss_Sink,
	iro_rss_last
};

static ir_node *_source = NULL;
static ir_node *_sink   = NULL;

#define is_Source(irn) ((irn) == _source)
#define is_Sink(irn)   ((irn) == _sink)

/**
 * Acquire opcodes and create source and sink nodes.
 */
static void init_rss_special_nodes(ir_graph *irg) {
	ir_node *block         = get_irg_start_block(irg);
	int     iro_rss_base   = get_next_ir_opcodes(iro_rss_last);
	ir_op   *op_rss_Source = new_ir_op(iro_rss_base + iro_rss_Source, "rss_Source", op_pin_state_pinned, irop_flag_none, oparity_zero, 0, 0, NULL);
	ir_op   *op_rss_Sink   = new_ir_op(iro_rss_base + iro_rss_Sink,   "rss_Sink",   op_pin_state_pinned, irop_flag_none, oparity_zero, 0, 0, NULL);
	_source                = new_ir_node(NULL, irg, block, op_rss_Source, mode_ANY, 0, NULL);
	_sink                  = new_ir_node(NULL, irg, block, op_rss_Sink, mode_ANY, 0, NULL);
}

static int cmp_int(const void *a, const void *b) {
	const int *i1 = a;
	const int *i2 = b;

	return QSORT_CMP(*i1, *i2);
}

static int cmp_child_costs(const void *a, const void *b) {
	const child_t *c1 = a;
	const child_t *c2 = b;

	return QSORT_CMP(c1->cost, c2->cost);
}

static int cmp_irn_idx(const void *a, const void *b) {
	const ir_node *n1 = *(ir_node **)a;
	const ir_node *n2 = *(ir_node **)b;

	return QSORT_CMP(get_irn_idx(n1), get_irn_idx(n2));
}

static int cmp_rss_edges(const void *a, const void *b) {
	const rss_edge_t *e1 = a;
	const rss_edge_t *e2 = b;

	return (e1->src != e2->src) || (e1->tgt != e2->tgt);
}

static int bsearch_for_index(int key, int *arr, size_t len, int force) {
	int left = 0;
	int right = len;

	while (right >= left) {
		int idx = (left + right) / 2;

		if (key < arr[idx])
			right = idx - 1;
		else if (key > arr[idx])
			left = idx + 1;
		else
			return idx;
	}

	if (force)
		assert(0 && "Something is wrong, key not found.");
	return -1;
}

static void dump_nodeset(nodeset *ns, const char *prefix) {
	ir_node *irn;
	foreach_nodeset(ns, irn) {
		ir_printf("%s%+F\n", prefix, irn);
	}
}

static ir_node **build_sorted_array_from_list(plist_t *irn_list, struct obstack *obst) {
	plist_element_t *el;
	int     i     = 0;
	int     len   = plist_count(irn_list);
	ir_node **arr = NEW_ARR_D(ir_node *, obst, len);

	/* copy the list into the array */
	foreach_plist(irn_list, el) {
		arr[i++] = plist_element_get_value(el);
	}

	/* sort the array by node index */
	qsort(arr, len, sizeof(arr[0]), cmp_irn_idx);

	return arr;
}

static void build_file_name(rss_t *rss, const char *suffix, size_t suf_len, char *buf, size_t len) {
	const char *irg_name;

	memset(buf, 0, len);
	irg_name = get_entity_name(get_irg_entity(rss->irg));
	snprintf(buf, len - suf_len, "%s-%s-block-%d",
		irg_name, arch_register_class_name(rss->cls), get_irn_node_nr(rss->block));
	strcat(buf, suffix);
}

/* Dumps all collected bipartite components of current irg as vcg. */
static void debug_vcg_dump_bipartite(rss_t *rss) {
	cbc_t *cbc;
	FILE  *f;
	char  file_name[256];
	static const char suffix[] = "-RSS-CBC.vcg";

	build_file_name(rss, suffix, sizeof(suffix), file_name, sizeof(file_name));
	f = fopen(file_name, "w");

	ir_fprintf(f, "graph: { title: \"connected bipartite component graph of %+F\"\n", rss->irg);
	fprintf(f, "display_edge_labels: no\n");
	fprintf(f, "layoutalgorithm: mindepth\n");
	fprintf(f, "manhattan_edges: yes\n\n");

	foreach_pset(rss->cbc_set, cbc) {
		ir_node    *n;
		rss_edge_t *ke;

		fprintf(f, "graph: { titel: \"cbc %d\" label: \"cbc %d\" status:clustered color:yellow\n", cbc->nr, cbc->nr);
		foreach_nodeset(cbc->parents, n) {
			ir_fprintf(f, "node: { title: \"n%d_%d\" label: \"%+F\" }\n", get_irn_node_nr(n), cbc->nr, n);
		}
		foreach_nodeset(cbc->children, n) {
			ir_fprintf(f, "node: { title: \"n%d_%d\" label: \"%+F\" }\n", get_irn_node_nr(n), cbc->nr, n);
		}
		foreach_pset(cbc->kill_edges, ke) {
			ir_fprintf(f, "edge: { sourcename: \"n%d_%d\" targetname: \"n%d_%d\" }\n",
				get_irn_node_nr(ke->src), cbc->nr, get_irn_node_nr(ke->tgt), cbc->nr);
		}
		fprintf(f, "}\n\n");
	}
	fprintf(f, "}\n");
	fclose(f);
}

/* Dump the computed killing function as vcg. */
static void debug_vcg_dump_kill(rss_t *rss) {
	FILE  *f;
	char  file_name[256];
	static const char suffix[] = "-RSS-KILL.vcg";
	plist_element_t *el;

	build_file_name(rss, suffix, sizeof(suffix), file_name, sizeof(file_name));
	f = fopen(file_name, "w");

	ir_fprintf(f, "graph: { title: \"computed kill graph of %+F, block %d\"\n", rss->irg, get_irn_node_nr(rss->block));
	fprintf(f, "display_edge_labels: no\n");
	fprintf(f, "layoutalgorithm: mindepth\n");
	fprintf(f, "manhattan_edges: yes\n\n");

	/* first: reset dumped flag of all nodes */
	foreach_plist(rss->nodes, el) {
		ir_node   *irn  = plist_element_get_value(el);
		rss_irn_t *rirn = get_rss_irn(rss, irn);
		rirn->dumped = 0;
	}

	/* dump all nodes and their killers */
	foreach_plist(rss->nodes, el) {
		ir_node   *irn     = plist_element_get_value(el);
		rss_irn_t *rirn    = get_rss_irn(rss, irn);
		rss_irn_t *pk_rirn = get_rss_irn(rss, rirn->killer);

		if (! rirn->dumped) {
			ir_fprintf(f, "node: { title: \"n%d\" label: \"%+F\" }\n", get_irn_node_nr(irn), irn);
			rirn->dumped = 1;
		}

		if (! pk_rirn->dumped) {
			ir_fprintf(f, "node: { title: \"n%d\" label: \"%+F\" }\n", get_irn_node_nr(rirn->killer), rirn->killer);
			pk_rirn->dumped = 1;
		}

		ir_fprintf(f, "edge: { sourcename: \"n%d\" targetname: \"n%d\" }\n",
			get_irn_node_nr(rirn->killer), get_irn_node_nr(irn));
	}

	fprintf(f, "}\n");
	fclose(f);
}

/* Dumps the potential killing DAG (PKG) as vcg. */
static void debug_vcg_dump_pkg(rss_t *rss) {
	FILE    *f;
	char    file_name[256];
	static const char suffix[] = "-RSS-PKG.vcg";
	plist_element_t *el;

	build_file_name(rss, suffix, sizeof(suffix), file_name, sizeof(file_name));
	f = fopen(file_name, "w");

	ir_fprintf(f, "graph: { title: \"potential killing DAG of %+F, block %d\"\n", rss->irg, get_irn_node_nr(rss->block));
	fprintf(f, "display_edge_labels: no\n");
	fprintf(f, "layoutalgorithm: mindepth\n");
	fprintf(f, "manhattan_edges: yes\n\n");

	foreach_plist(rss->nodes, el) {
		ir_node   *irn  = plist_element_get_value(el);
		rss_irn_t *rirn = get_rss_irn(rss, irn);
		plist_element_t *k_el;

		ir_fprintf(f, "node: { title: \"n%d\" label: \"%+F\" }\n", get_irn_node_nr(irn), irn);
		rirn->dumped = 1;

		foreach_plist(rirn->pkiller_list, k_el) {
			ir_node   *pkiller = plist_element_get_value(k_el);
			rss_irn_t *pk_rirn = get_rss_irn(rss, pkiller);

			if (! pk_rirn->dumped) {
				ir_fprintf(f, "node: { title: \"n%d\" label: \"%+F\" }\n", get_irn_node_nr(pkiller), pkiller);
				pk_rirn->dumped = 1;
			}
			ir_fprintf(f, "edge: { sourcename: \"n%d\" targetname: \"n%d\" }\n",
				get_irn_node_nr(pkiller), get_irn_node_nr(irn));
		}
	}
	fprintf(f, "}\n");
	fclose(f);
}

/* Dumps the disjoint value DAG (DVG) as vcg. */
static void debug_vcg_dump_dvg(rss_t *rss, dvg_t *dvg) {
	static const char suffix[] = "-RSS-DVG.vcg";
	FILE       *f;
	char       file_name[256];
	ir_node    *irn;
	rss_edge_t *edge;

	build_file_name(rss, suffix, sizeof(suffix), file_name, sizeof(file_name));
	f = fopen(file_name, "w");

	ir_fprintf(f, "graph: { title: \"disjoint value DAG of %+F, block %d\"\n", rss->irg, get_irn_node_nr(rss->block));
	fprintf(f, "display_edge_labels: no\n");
	fprintf(f, "layoutalgorithm: mindepth\n");
	fprintf(f, "manhattan_edges: yes\n\n");

	/* dump all nodes */
	foreach_nodeset(dvg->nodes, irn) {
		ir_fprintf(f, "node: { title: \"n%d\" label: \"%+F\" }\n", get_irn_node_nr(irn), irn);
	}

	/* dump all edges */
	foreach_pset(dvg->edges, edge) {
		rss_irn_t *src = get_rss_irn(rss, edge->src);
		rss_irn_t *tgt = get_rss_irn(rss, edge->tgt);

		ir_fprintf(f, "edge: { sourcename: \"n%d\" targetname: \"n%d\" }\n",
			get_irn_node_nr(edge->src), get_irn_node_nr(edge->tgt));
	}

	fprintf(f, "}\n");
	fclose(f);
}

/* Dumps the PKG(DVG). */
static void debug_vcg_dump_dvg_pkiller(rss_t *rss, dvg_t *dvg) {
	static const char suffix[] = "-RSS-DVG-PKG.vcg";
	FILE       *f;
	char       file_name[256];
	ir_node    *irn;

	build_file_name(rss, suffix, sizeof(suffix), file_name, sizeof(file_name));
	f = fopen(file_name, "w");

	ir_fprintf(f, "graph: { title: \"PKG of disjoint value DAG of %+F, block %d\"\n", rss->irg, get_irn_node_nr(rss->block));
	fprintf(f, "display_edge_labels: no\n");
	fprintf(f, "layoutalgorithm: mindepth\n");
	fprintf(f, "manhattan_edges: yes\n\n");

	/* dump all nodes */
	foreach_nodeset(dvg->nodes, irn) {
		ir_fprintf(f, "node: { title: \"n%d\" label: \"%+F\" }\n", get_irn_node_nr(irn), irn);
	}

	/* dump all edges */
	foreach_nodeset(dvg->nodes, irn) {
		rss_irn_t       *node = get_rss_irn(rss, irn);
		plist_element_t *el;

		foreach_plist(node->dvg_pkiller_list, el) {
			ir_fprintf(f, "edge: { sourcename: \"n%d\" targetname: \"n%d\" }\n",
				get_irn_node_nr(plist_element_get_value(el)), get_irn_node_nr(irn));
		}
	}

	fprintf(f, "}\n");
	fclose(f);
}

/**
 * In case there is no rss information for irn, initialize it.
 */
static void *init_rss_irn(phase_t *ph, ir_node *irn, void *old) {
	rss_irn_t *res = phase_alloc(ph, sizeof(res[0]));

	res->descendant_list  = plist_obstack_new(phase_obst(ph));
	res->descendants      = NULL;

	res->consumer_list    = plist_obstack_new(phase_obst(ph));
	res->consumer         = NULL;

	res->pkiller_list     = plist_obstack_new(phase_obst(ph));
	res->pkillers         = NULL;

	res->parent_list      = plist_obstack_new(phase_obst(ph));
	res->parents          = NULL;

	res->dvg_desc_list    = plist_obstack_new(phase_obst(ph));
	res->dvg_desc         = NULL;

	res->kill_value_list  = plist_obstack_new(phase_obst(ph));
	res->dvg_user_list    = plist_obstack_new(phase_obst(ph));
	res->dvg_pkiller_list = plist_obstack_new(phase_obst(ph));

	res->killer           = NULL;
	res->irn              = irn;
	res->chain            = NULL;

	res->live_out         = 0;
	res->visited          = 0;
	res->handled          = 0;
	res->dumped           = 0;

	return res;
}

/**
 * Collect all nodes data dependent on current node.
 */
static void collect_descendants(rss_t *rss, rss_irn_t *rirn, ir_node *irn, int *got_sink) {
	const ir_edge_t *edge;
	ir_node         *block = rss->block;

	foreach_out_edge(irn, edge) {
		ir_node *user = get_edge_src_irn(edge);

		/* skip ignore nodes as they do not really contribute to register presssure */
		if (arch_irn_is(rss->arch_env, user, ignore))
			continue;

		/* check if user lives in block and is not a control flow node */
		if (get_nodes_block(user) == block && get_irn_mode(user) != mode_X) {
			/* skip mode_T nodes */
			if (get_irn_mode(user) != mode_T && ! plist_has_value(rirn->descendant_list, user)) {
				plist_insert_back(rirn->descendant_list, user);
				DBG((rss->dbg, DEBUG_NODEINFO, "\t\tdescendant %+F\n", user));
			}
			collect_descendants(rss, rirn, user, got_sink);
		}
		else if (! *got_sink) {
			/* user lives out of block: add sink as descendant if not already done */
			plist_insert_back(rirn->descendant_list, _sink);
			*got_sink = 1;
			DBG((rss->dbg, DEBUG_NODEINFO, "\t\tdescendant %+F\n", _sink));
		}
	}
}

/**
 * Handles a single consumer.
 */
static int collect_single_consumer(rss_t *rss, rss_irn_t *rss_irn, ir_node *consumer, int got_sink) {
	ir_node *block = rss->block;

	if (get_nodes_block(consumer) == block) {
		/* the consumer of a mode_T node are it's projs */
		if (get_irn_mode(consumer) == mode_T) {
			const ir_edge_t *cons_edge;

			DBG((rss->dbg, DEBUG_NODEINFO, "\t\tmode_T consumer %+F skipped\n", consumer));
			foreach_out_edge(consumer, cons_edge) {
				ir_node *cons_proj = get_edge_src_irn(cons_edge);

				assert(get_nodes_block(cons_proj) == block && "Proj in wrong block!");

				/* skip ignore nodes, as they do not really contribute to register pressure */
				if (arch_irn_is(rss->arch_env, cons_proj, ignore))
					continue;

				plist_insert_back(rss_irn->consumer_list, cons_proj);
				DBG((rss->dbg, DEBUG_NODEINFO, "\t\t\treal consumer %+F\n", cons_proj));
			}
		}
		else if (! arch_irn_is(rss->arch_env, consumer, ignore)) {
			plist_insert_back(rss_irn->consumer_list, consumer);
			DBG((rss->dbg, DEBUG_NODEINFO, "\t\tconsumer %+F\n", consumer));
		}
	}
	else {
		rss_irn->live_out = 1;
		DBG((rss->dbg, DEBUG_NODEINFO, "\t\tlive out %+F", consumer));
		if (! got_sink) {
			plist_insert_back(rss_irn->consumer_list, _sink);
			got_sink = 1;
			DB((rss->dbg, DEBUG_NODEINFO, ", %+F added instead", _sink));
		}
		DB((rss->dbg, DEBUG_NODEINFO, "\n"));
	}

	return got_sink;
}

/**
 * Collect all nodes consuming the value(s) produced by current node.
 */
static void collect_consumer(rss_t *rss, rss_irn_t *rss_irn, ir_node *irn) {
	const ir_edge_t *edge;
	int got_sink = 0;

	foreach_out_edge(irn, edge) {
		ir_node *consumer = get_edge_src_irn(edge);
		got_sink = collect_single_consumer(rss, rss_irn, consumer, got_sink);
	}
}

#if 0
/**
 * We need to build the consumer and descendant list for _source.
 */
static void collect_node_info_source(rss_t *rss) {
	const ir_edge_t *edge;
	rss_irn_t       *rirn  = get_rss_irn(rss, _source);
	ir_node         *block = rss->block;

	if (rirn->handled)
		return;

	foreach_out_edge(block, edge) {
		ir_node *irn = get_edge_src_irn(edge);
		int     i;

		for (i = get_irn_arity(n) - 1; i >= 0; --i) {

		}
	}
}

static void reset_node_info(rss_irn_t *rss_irn) {
	/* Beware: array data resides on phase obstack, so it gets removed automatically */

	plist_clear(rss_irn->consumer_list);
	rss_irn->consumer = NULL;

	plist_clear(rss_irn->parent_list);
	rss_irn->parents = NULL;

	plist_clear(rss_irn->descendant_list);
	rss_irn->descendants = NULL;

	plist_clear(rss_irn->pkiller_list);
	rss_irn->pkillers = NULL;

	plist_clear(rss_irn->kill_value_list);

	rss_irn->killer   = NULL;
	rss_irn->live_out = 0;
	rss_irn->visited  = 0;
	rss_irn->handled  = 0;
}
#endif

/**
 * Collects all consumer and descendant of a irn.
 */
static void collect_node_info(rss_t *rss, ir_node *irn) {
	rss_irn_t *rss_irn = get_rss_irn(rss, irn);
	int       got_sink;

	assert(get_irn_mode(irn) != mode_T && "Cannot handle mode_T nodes.");

	/* check if node info is already available */
	if (rss_irn->handled)
		return;

	DBG((rss->dbg, DEBUG_NODEINFO, "\tcomputing consumers of %+F:\n", irn));

	/* collect all consumer */
	got_sink = 0;
	collect_consumer(rss, rss_irn, irn);

	/* build sorted consumer array */
	rss_irn->consumer = build_sorted_array_from_list(rss_irn->consumer_list, phase_obst(&rss->ph));

	DBG((rss->dbg, DEBUG_NODEINFO, "\tcompute descendants of %+F:\n", irn));

	/* collect descendants */
	got_sink = 0;
	collect_descendants(rss, rss_irn, irn, &got_sink);

	/* build sorted descendant array */
	rss_irn->descendants = build_sorted_array_from_list(rss_irn->descendant_list, phase_obst(&rss->ph));

	rss_irn->handled = 1;
}

/**
 * Checks if v is a potential killer of u.
 * v is in pkill(u) iff descendants(v) cut consumer(u) is v
 *
 * @param rss   The rss object
 * @param v      The node to check for killer
 * @param u      The potentially killed value
 * @return 1 if v is in pkill(u), 0 otherwise
 */
static int is_potential_killer(rss_t *rss, rss_irn_t *v, rss_irn_t *u) {
	plist_t *list;
	ir_node **arr;
	plist_element_t *el;

	assert(is_Sink(v->irn) || ((plist_count(v->descendant_list) > 0 && v->descendants) || 1));
	assert(is_Sink(u->irn) || ((plist_count(u->consumer_list)   > 0 && u->consumer)    || 1));

	/* as we loop over the list: loop over the shorter one */
	if (plist_count(v->descendant_list) > plist_count(u->consumer_list)) {
		list = u->consumer_list;
		arr  = v->descendants;
	}
	else {
		list = v->descendant_list;
		arr  = u->consumer;
	}

	/* for each list element: try to find element in array */
	foreach_plist(list, el) {
		ir_node *irn   = plist_element_get_value(el);
		ir_node *match = BSEARCH_IRN_ARR(irn, arr);

		if (match && match != irn)
			return 0;
	}

	return 1;
}

/**
 * Compute the potential killing set PK.
 */
static void compute_pkill_set(rss_t *rss) {
	plist_element_t *u_el, *v_el;

	foreach_plist(rss->nodes, u_el) {
		ir_node   *u_irn = plist_element_get_value(u_el);
		rss_irn_t *u     = get_rss_irn(rss, u_irn);

		DBG((rss->dbg, DEBUG_PKILL, "\tcomputing potential killers of %+F:\n", u_irn));

		/* check each consumer if it is a potential killer  */
		foreach_plist(u->consumer_list, v_el) {
			ir_node   *v_irn = plist_element_get_value(v_el);
			rss_irn_t *v     = get_rss_irn(rss, v_irn);

			/* check, if v is a potential killer of u */
			if (is_potential_killer(rss, v, u)) {
				if (! plist_has_value(u->pkiller_list, v_irn))
					plist_insert_back(u->pkiller_list, v_irn);
				if (! plist_has_value(v->kill_value_list, u_irn))
					plist_insert_back(v->kill_value_list, u_irn);
				DBG((rss->dbg, DEBUG_PKILL, "\t\tpotential killer %+F\n", v_irn));
			}
		}

		u->killer = _sink;
	}

	DEBUG_ONLY(
		if (firm_dbg_get_mask(rss->dbg) & DEBUG_PKILL)
			debug_vcg_dump_pkg(rss);
	)
}

/**
 * Build set of killing edges (from values to their potential killers)
 */
static void build_kill_edges(rss_t *rss, pset *epk) {
	plist_element_t *el, *k_el;

	foreach_plist(rss->nodes, el) {
		ir_node    *irn  = plist_element_get_value(el);
		rss_irn_t *rirn = get_rss_irn(rss, irn);

		foreach_plist(rirn->pkiller_list, k_el) {
			ir_node    *pkiller = plist_element_get_value(k_el);
			rss_edge_t *ke      = obstack_alloc(phase_obst(&rss->ph), sizeof(*ke));

			ke->src  = irn;
			ke->tgt  = pkiller;
			ke->next = NULL;

			pset_insert(epk, ke, HASH_RSS_EDGE(ke));
		}
	}
}

/* print the given cbc for debugging purpose */
static void debug_print_cbc(firm_dbg_module_t *mod, cbc_t *cbc) {
	ir_node    *n;
	rss_edge_t *ke;

	DBG((mod, DEBUG_BIPARTITE, "\t\tS = set of parents:\n"));
	foreach_nodeset(cbc->parents, n) {
		DBG((mod, DEBUG_BIPARTITE, "\t\t\t%+F\n", n));
	}
	DBG((mod, DEBUG_BIPARTITE, "\t\tT = set of children:\n"));
	foreach_nodeset(cbc->children, n) {
		DBG((mod, DEBUG_BIPARTITE, "\t\t\t%+F\n", n));
	}
	DBG((mod, DEBUG_BIPARTITE, "\t\tE = Edges from producers to consumers\n"));
	foreach_pset(cbc->kill_edges, ke) {
		DBG((mod, DEBUG_BIPARTITE, "\t\t\t%+F -> %+F\n", ke->src, ke->tgt));
	}
}

/**
 * Construct the bipartite decomposition.
 * Sid-Ahmed-Ali Touati, Phd Thesis
 * Register Pressure in Instruction Level Parallelism, p. 71
 */
static void compute_bipartite_decomposition(rss_t *rss) {
	pset *epk    = new_pset(cmp_rss_edges, 10);
	int  cur_num = 0;

	plist_element_t *el;

	DBG((rss->dbg, DEBUG_BIPARTITE, "\tcomputing bipartite decomposition:\n"));

	build_kill_edges(rss, epk);

	foreach_plist(rss->nodes, el) {
		ir_node   *u_irn   = plist_element_get_value(el);
		rss_irn_t *u       = get_rss_irn(rss, u_irn);
		int       p_change = 1;
		int       c_change = 1;

		cbc_t           *cbc;
		plist_element_t *el2;
		rss_edge_t      *k_edge;
		rss_edge_t      *kedge_root = NULL;
		ir_node         *t_irn, *s_irn;

		if (u->visited || u_irn == _sink)
			continue;

		DBG((rss->dbg, DEBUG_BIPARTITE, "\t\t%+F choosen:\n", u_irn));

		cbc     = obstack_alloc(phase_obst(&rss->ph), sizeof(*cbc));
		cbc->nr = cur_num++;

		/* initialize S_cb */
		cbc->parents = new_nodeset(5);
		nodeset_insert(cbc->parents, u_irn);
		DBG((rss->dbg, DEBUG_BIPARTITE, "\t\t\t%+F added to parents (init)\n", u_irn));

		/* E_cb = empty */
		cbc->kill_edges = new_pset(cmp_rss_edges, 5);

		/* each parent gets killed by at least one value from children */

		/* T_cb = PK_successors(u) */
		cbc->children = new_nodeset(5);
		foreach_plist(u->pkiller_list, el2) {
			nodeset_insert(cbc->children, plist_element_get_value(el2));
			DBG((rss->dbg, DEBUG_BIPARTITE, "\t\t\t%+F added to children (init)\n", plist_element_get_value(el2)));
		}

		/*
			Now: insert the parents of all children into the parent set
			and insert the children of all parents into the children set
			until the sets don't change any more
		*/
		while (p_change || c_change) {
			p_change = c_change = 0;

			/* accumulate parents */
			foreach_nodeset(cbc->children, t_irn) {
				rss_irn_t *t = get_rss_irn(rss, t_irn);
				plist_element_t *kvl_el;

				foreach_plist(t->kill_value_list, kvl_el) {
					ir_node *val = plist_element_get_value(kvl_el);

					if (! nodeset_find(cbc->parents, val)) {
						nodeset_insert(cbc->parents, val);
						p_change = 1;
						DBG((rss->dbg, DEBUG_BIPARTITE, "\t\t\t%+F added to parents\n", val));
					}
				}
			}

			/* accumulate children */
			foreach_nodeset(cbc->parents, s_irn) {
				rss_irn_t *s = get_rss_irn(rss, s_irn);
				plist_element_t *pkl_el;

				foreach_plist(s->pkiller_list, pkl_el) {
					ir_node *val = plist_element_get_value(pkl_el);

					if (! nodeset_find(cbc->children, val)) {
						nodeset_insert(cbc->children, val);
						c_change = 1;
						DBG((rss->dbg, DEBUG_BIPARTITE, "\t\t\t%+F added to children\n", val));
					}
				}
			}
		}

		/* mark all parent values as visited */
		foreach_nodeset(cbc->parents, s_irn) {
			rss_irn_t *s = get_rss_irn(rss, s_irn);
			s->visited = 1;
			/* assure bipartite property */
			if (nodeset_find(cbc->children, s_irn)) {
				nodeset_remove(cbc->children, s_irn);
				DBG((rss->dbg, DEBUG_BIPARTITE, "\t\t\t%+F removed from to children\n", s_irn));
			}
		}

		/* update edges */
		foreach_pset(epk, k_edge) {
			if (nodeset_find(cbc->parents, k_edge->src) && nodeset_find(cbc->children, k_edge->tgt)) {
				pset_insert(cbc->kill_edges, k_edge, HASH_RSS_EDGE(k_edge));
				/*
					Link all k_edges which are about to be removed together.
					Beware: pset_remove kills the iterator.
				*/
				k_edge->next = kedge_root;
				kedge_root   = k_edge;
			}
		}

		/* remove all linked k_edges */
		for (; kedge_root; kedge_root = kedge_root->next) {
			pset_remove(epk, kedge_root, HASH_RSS_EDGE(kedge_root));
		}

		/* add the connected bipartite component */
		pset_insert(rss->cbc_set, cbc, (unsigned)cbc->nr);
		DBG((rss->dbg, DEBUG_BIPARTITE, "\tbipartite component %d inserted:\n", cbc->nr));
		DEBUG_ONLY(debug_print_cbc(rss->dbg, cbc));
	}

	if (firm_dbg_get_mask(rss->dbg) & DEBUG_BIPARTITE)
		debug_vcg_dump_bipartite(rss);

	del_pset(epk);
}

/**
 * Select the child with the maximum cost.
 */
static child_t *select_child_max_cost(rss_t *rss, nodeset *x, nodeset *y, child_t *t, cbc_t *cbc) {
	ir_node *child;
	float   max_cost = -1.0f;

	DBG((rss->dbg, DEBUG_SKS, "\t\tcomputing children costs:\n"));

	foreach_nodeset(cbc->children, child) {
		rss_irn_t  *r_child             = get_rss_irn(rss, child);
		int         num_unkilled_parents = 0;
		int         num_descendants;
		rss_edge_t *k_edge;
		float       cost;

		/* get the number of unkilled parents */
		foreach_pset(cbc->kill_edges, k_edge) {
			if (k_edge->tgt == child && nodeset_find(x, k_edge->src))
				++num_unkilled_parents;
		}

		cost = (float)num_unkilled_parents;

		num_descendants = plist_count(r_child->descendant_list) + nodeset_count(y);

		if (num_descendants > 0)
			cost /= (float)num_descendants;

		DBG((rss->dbg, DEBUG_SKS, "\t\t\t%+F, #desc %d, cost %.3f\n", child, num_descendants, cost));

		if (cost > max_cost) {
			t->irn   = child;
			t->cost  = cost;
			max_cost = cost;
		}
	}

	return t;
}

/**
 * Remove all parents from x which are killed by t_irn.
 */
static void remove_covered_parents(rss_t *rss, nodeset *x, ir_node *t_irn, cbc_t *cbc) {
	rss_irn_t  *t = get_rss_irn(rss, t_irn);
	rss_edge_t *k_edge;

	DBG((rss->dbg, DEBUG_SKS, "\t\tremoving parents covered by %+F:\n", t_irn));

	foreach_pset(cbc->kill_edges, k_edge) {
		if (k_edge->tgt == t_irn && nodeset_find(x, k_edge->src)) {
			nodeset_remove(x, k_edge->src);
			plist_insert_back(t->parent_list, k_edge->src);
			DBG((rss->dbg, DEBUG_SKS, "\t\t\t%+F\n", k_edge->src));
		}
	}
}

static void update_cumulated_descendent_values(rss_t *rss, nodeset *y, ir_node *t_irn) {
	rss_irn_t *t = get_rss_irn(rss, t_irn);
	plist_element_t *el;

	DBG((rss->dbg, DEBUG_SKS, "\t\tupdating cumulated descendant value of %+F:\n", t_irn));

	foreach_plist(t->descendant_list, el) {
		nodeset_insert(y, plist_element_get_value(el));
		DBG((rss->dbg, DEBUG_SKS, "\t\t\t%+F\n", plist_element_get_value(el)));
	}
}

/**
 * Greedy-k: a heuristics for the MMA problem
 */
static void compute_killing_function(rss_t *rss) {
	cbc_t *cbc;
	struct obstack obst;

	obstack_init(&obst);

	rss->cbc_set = pset_new_ptr(5);
	compute_bipartite_decomposition(rss);

	/* for all bipartite components do: */
	foreach_pset(rss->cbc_set, cbc) {
		ir_node *p;
		nodeset *x       = new_nodeset(10);
		nodeset *y       = new_nodeset(10);
		child_t **sks    = NEW_ARR_F(child_t *, 20);
		int     cur_len  = 0;
		int     cur_size = 20;
		int     i;

		DBG((rss->dbg, DEBUG_SKS, "\tcomputing SKS for cbc %d:\n", cbc->nr));
		DBG((rss->dbg, DEBUG_SKS, "\t\tinitializing parents X:\n"));

		/* X = S_cb (all parents are initially uncovered) */
		foreach_nodeset(cbc->parents, p) {
			nodeset_insert(x, p);
			DBG((rss->dbg, DEBUG_SKS, "\t\t\t%+F\n", p));
		}

		/* while X not empty */
		while (nodeset_count(x) > 0) {
			child_t *t = obstack_alloc(&obst, sizeof(*t));
			memset(t, 0, sizeof(*t));

			t = select_child_max_cost(rss, x, y, t, cbc);

			if (cur_len >= cur_size) {
				ARR_EXTO(child_t *, sks, cur_size * 2);
				cur_size *= 2;
			}

			DBG((rss->dbg, DEBUG_SKS, "\t\tinsert child %+F (%.3f) into SKS at pos %d\n", t->irn, t->cost, cur_len));

			sks[cur_len++] = t;
			remove_covered_parents(rss, x, t->irn, cbc);
			update_cumulated_descendent_values(rss, y, t->irn);
		}

		ARR_SHRINKLEN(sks, cur_len);

		/* sort SKS in increasing cost order */
		qsort(sks, cur_len, sizeof(sks[0]), cmp_child_costs);

		DBG((rss->dbg, DEBUG_SKS, "\tprocessing SKS for cbc %d:\n", cbc->nr));

		/* build killing function */
		for (i = cur_len - 1; i >= 0; --i) { /* loop over sks in decreasing cost order */
			child_t         *t = sks[i];
			rss_irn_t       *rt = get_rss_irn(rss, t->irn);
			plist_element_t *p_el;

			DBG((rss->dbg, DEBUG_SKS, "\t\t\tkiller %+F (%.3f):\n", t->irn, t->cost));

			/* kill all unkilled parents of t */
			foreach_plist(rt->parent_list, p_el) {
				ir_node    *par = plist_element_get_value(p_el);
				rss_irn_t *rpar = get_rss_irn(rss, par);

				if (is_Sink(rpar->killer)) {
					rpar->killer = t->irn;
					DBG((rss->dbg, DEBUG_SKS, "\t\tkill %+F\n", rpar->irn));
				}
				else {
					DBG((rss->dbg, DEBUG_SKS, "\t\t\tkeeping %+F as killer for %+F\n", rpar->killer, rpar->irn));
				}
			}
		}

		del_nodeset(x);
		del_nodeset(y);
		DEL_ARR_F(sks);
	}

	DEBUG_ONLY(
		if (firm_dbg_get_mask(rss->dbg) & DEBUG_SKS)
			debug_vcg_dump_kill(rss);
	);

	del_pset(rss->cbc_set);
	obstack_free(&obst, NULL);
}

/**
 * Computes the disjoint value DAG (DVG).
 * BEWARE: It is not made explicitly clear in the Touati paper,
 *         but the DVG is meant to be build from the KILLING DAG
 */
static void compute_dvg(rss_t *rss, dvg_t *dvg) {
	plist_element_t *el, *el2;

	DBG((rss->dbg, DEBUG_DVG, "\tcomputing DVG:\n"));

	foreach_plist(rss->nodes, el) {
		ir_node    *u_irn      = plist_element_get_value(el);
		rss_irn_t  *u          = get_rss_irn(rss, u_irn);
		ir_node    *old_killer = NULL;
		ir_node    *cur_killer = u->killer;

		nodeset_insert(dvg->nodes, u_irn);

		/* We add an edge to every killer, from where we could be reached. */
		while (cur_killer != old_killer) { /* sink kills itself */
			rss_edge_t *dvg_edge = obstack_alloc(phase_obst(&rss->ph), sizeof(*dvg_edge));
			rss_irn_t  *c_killer = get_rss_irn(rss, cur_killer);
			rss_edge_t key;

			nodeset_insert(dvg->nodes, cur_killer);

			/* add an edge to our killer */
			dvg_edge->src  = u_irn;
			dvg_edge->tgt  = cur_killer;
			dvg_edge->next = NULL;

			key.src = cur_killer;
			key.tgt = u_irn;
			assert(pset_find(dvg->edges, &key, HASH_RSS_EDGE(&key)) == NULL && "DVG must be acyclic!");

			/* add the edge to the DVG */
			DBG((rss->dbg, DEBUG_DVG, "\t\tadd edge %+F -> %+F\n", u_irn, cur_killer));
			pset_insert(dvg->edges, dvg_edge, HASH_RSS_EDGE(dvg_edge));

			/* descent to the next killer */
			old_killer = cur_killer;
			cur_killer = c_killer->killer;
		}

#if 0

		foreach_plist(rss->nodes, el2) {
			ir_node *v_irn = plist_element_get_value(el2);

			/*
				There is an edge (u, v) in the DVG iff v is a descendant of the killer(u).
			*/
			if (BSEARCH_IRN_ARR(v_irn, u_kill->descendants)) {
				rss_edge_t *dvg_edge = obstack_alloc(phase_obst(&rss->ph), sizeof(*dvg_edge));
				rss_edge_t key;

				/* insert the user into the DVG and append it to the user list of u */
				nodeset_insert(dvg->nodes, v_irn);
				if (! plist_has_value(u->dvg_user_list, v_irn))
					plist_insert_back(u->dvg_user_list, v_irn);

				dvg_edge->src  = u_irn;
				dvg_edge->tgt  = v_irn;
				dvg_edge->next = NULL;

				key.src = v_irn;
				key.tgt = u_irn;

				assert(pset_find(dvg->edges, &key, HASH_RSS_EDGE(&key)) == NULL && "DVG must be acyclic!");

				/* add the edge to the DVG */
				DBG((rss->dbg, DEBUG_DVG, "\t\tadd edge %+F -> %+F\n", u_irn, v_irn));
				pset_insert(dvg->edges, dvg_edge, HASH_RSS_EDGE(dvg_edge));
			}
		}
#endif /* if 0 */
	}

	DEBUG_ONLY(
		if (firm_dbg_get_mask(rss->dbg) & DEBUG_DVG)
			debug_vcg_dump_dvg(rss, dvg);
	);
}

/**
 * Accumulate all descendants for root into list.
 */
static void accumulate_dvg_descendant_values(rss_t *rss, rss_irn_t *root, plist_t *list) {
	if (plist_count(root->dvg_user_list) > 0) {
		plist_element_t *el;

		foreach_plist(root->dvg_user_list, el) {
			ir_node   *v_irn = plist_element_get_value(el);
			rss_irn_t *v     = get_rss_irn(rss, v_irn);

			/* add v as descendant */
			if (! plist_has_value(list, v_irn)) {
				plist_insert_back(list, v_irn);
				DBG((rss->dbg, DEBUG_DVG, "\t\t\tadd DVG descendant %+F\n", v_irn));
			}

			/* accumulate v's descendants */
			accumulate_dvg_descendant_values(rss, v, list);
		}
	}
}

/**
 * Builds the list of potential killers for each node
 * in the given DVG.
 * Needs the descendant list for all user as sorted array.
 */
static void build_dvg_pkiller_list(rss_t *rss, dvg_t *dvg) {
	ir_node *irn;

	foreach_nodeset(dvg->nodes, irn) {
		rss_irn_t       *node = get_rss_irn(rss, irn);
		plist_element_t *el, *el2;

		DBG((rss->dbg, DEBUG_DVG, "\t\tbuilding pkiller list for %+F\n", irn));

		/* check each user */
		foreach_plist(node->dvg_user_list, el) {
			ir_node *u_irn = plist_element_get_value(el);

			/* is the current user u_irn not a descendant of any other user -> pkiller */
			foreach_plist(node->dvg_user_list, el2) {
				ir_node   *v_irn = plist_element_get_value(el2);
				rss_irn_t *v     = get_rss_irn(rss, v_irn);

				if (el != el2                             &&
					! BSEARCH_IRN_ARR(u_irn, v->dvg_desc) &&
					! plist_has_value(node->dvg_pkiller_list, u_irn))
				{
					plist_insert_back(node->dvg_pkiller_list, u_irn);
					DBG((rss->dbg, DEBUG_DVG, "\t\t\tadd DVG pkiller %+F\n", u_irn));
				}
			}
		}

		node->dvg_pkiller = build_sorted_array_from_list(node->dvg_pkiller_list, phase_obst(&rss->ph));
	}

	DEBUG_ONLY(
		if (firm_dbg_get_mask(rss->dbg) & DEBUG_DVG)
			debug_vcg_dump_dvg_pkiller(rss, dvg);
	);
}

/**
 * Compute the maximal antichain of the current DVG.
 * This is a reimplementation of the MAXIMAL_ANTI_CHAIN function
 * from the DDG library 1.1 (DAG.cpp).
 */
static nodeset *compute_maximal_antichain(rss_t *rss, dvg_t *dvg) {
	int         n               = nodeset_count(dvg->nodes);
	int         *assignment     = alloca(n * sizeof(assignment[0]));
	int         *assignment_rev = alloca(n * sizeof(assignment_rev[0]));
	int         *idx_map        = alloca(n * sizeof(idx_map[0]));
	hungarian_problem_t *bp;
	nodeset     *values, *temp;
	ir_node     *u_irn;
	int         i, j, cost, cur_chain;
	rss_edge_t  *dvg_edge;

#define MAP_IDX(irn) bsearch_for_index(get_irn_idx(irn), idx_map,  n,  1)

	if (pset_count(dvg->edges) == 0)
		return NULL;

	bp = hungarian_new(n, n, 1, HUNGARIAN_MATCH_NORMAL);

	/*
		At first, we build an index map for the nodes in the DVG,
		because we cannot use the irn idx therefore as the resulting
		bipartite data structure would have around 1.2GB.
		So we limit the size to the number of nodes we have in the DVG
		and build a sorted index map for their irn indices.
	*/
	i = 0;
	foreach_nodeset(dvg->nodes, u_irn) {
		idx_map[i++] = get_irn_idx(u_irn);
	}
	qsort(idx_map, n, sizeof(idx_map[0]), cmp_int);

	foreach_pset(dvg->edges, dvg_edge) {
		int idx_u = MAP_IDX(dvg_edge->src);
		int idx_v = MAP_IDX(dvg_edge->tgt);

		/* add the entry to the bipartite data structure */
		hungarian_add(bp, idx_u, idx_v, 1);
		DBG((rss->dbg, DEBUG_MAX_AC, "\t\t\tadd %d (%+F) -> %d (%+F)\n",
			idx_u, dvg_edge->src, idx_v, dvg_edge->tgt));
	}
#if 0
	/*
		Add a bipartite entry for each pair of nodes (u, v), where exists a
		path in the DVG from u to v, ie. connect all descendants(v) to v.
		desc_dvg(v) = accumulated descendants of all z in dvg_user_list(v)
	*/
	foreach_nodeset(dvg->nodes, u_irn) {
		rss_irn_t *u        = get_rss_irn(rss, u_irn);
		int       idx_u_irn = MAP_IDX(u_irn);

		DBG((rss->dbg, DEBUG_DVG, "\t\tcomputing DVG descendants of %+F:\n", u_irn));

		//plist_clear(u->dvg_desc_list);
		//accumulate_dvg_descendant_values(rss, u, u->dvg_desc_list);

		/*
			FIXME: The array is build on the phase obstack and we cannot free the data.
			So the array get as many times allocated as this function is called.
		*/

		/* build the sorted array for faster searches */
		//u->dvg_desc = build_sorted_array_from_list(u->dvg_desc_list, phase_obst(&rss->ph));

		DBG((rss->dbg, DEBUG_MAX_AC, "\t\tadding bipartite entries of %+F:\n", u_irn));

		/* add the bipartite entries for all descendants */
		for (i = ARR_LEN(u->dvg_desc) - 1; i >= 0; --i) {
			rss_irn_t *desc    = get_rss_irn(rss, u->dvg_desc[i]);
			int       idx_desc = MAP_IDX(u->dvg_desc[i]);

			/* add the entry to the bipartite data structure */
			hungarian_add(bp, idx_u_irn, idx_desc, 1);
			DBG((rss->dbg, DEBUG_MAX_AC, "\t\t\tadd %d (%+F) -> %d (%+F)\n",
				idx_u_irn, u_irn, idx_desc, u->dvg_desc[i]));

			need_matching = 1;
		}
	}
#endif

	/* We want maximum cardinality matching */
	hungarian_prepare_cost_matrix(bp, HUNGARIAN_MODE_MAXIMIZE_UTIL);

	DBG((rss->dbg, DEBUG_DVG, "\t\tcomputing DVG pkiller:\n"));
	/* beware: the following function needs the dvg_desc array */
	build_dvg_pkiller_list(rss, dvg);

	DBG((rss->dbg, DEBUG_MAX_AC, "\t\tcomputing bipartite matching\n"));
	/*
		The maximum cardinality bipartite matching gives us the minimal
		chain partition, which corresponds to the maximum anti chains.
	*/
	cost = hungarian_solve(bp, assignment);
	assert(cost >= 0 && "Bipartite matching failed!");

	hungarian_free(bp);
	memset(assignment_rev, -1, n * sizeof(assignment_rev[0]));

	/* build the reverse assignment, ie. foreach i -> j, add j -> i */
	for (i = 0; i < n; ++i) {
		if (assignment[i] >= 0) {
			int j = assignment[i];
			assignment_rev[j] = i;
		}
	}

	DBG((rss->dbg, DEBUG_MAX_AC, "\t\t\tgot assignment with cost %d\n", cost));
	DBG((rss->dbg, DEBUG_MAX_AC, "\t\t\tassignment   ---   reverse assignment\n", cost));
	for (i = 0; i < n; ++i) {
		DBG((rss->dbg, DEBUG_MAX_AC, "\t\t\t%3d -> %3d         %3d -> %3d\n", i, assignment[i], i, assignment_rev[i]));
	}


	values    = new_nodeset(10);
	cur_chain = 0;
	/* Construction of the minimal chain partition */
	for (j = 0; j < n; ++j) {
		/* check nodes, which did not occur as target */
		if (assignment_rev[j] == -1) {
			int       xj      = idx_map[j];
			ir_node   *xj_irn = get_idx_irn(rss->irg, xj);
			rss_irn_t *xj_rss = get_rss_irn(rss, xj_irn);
			chain_t   *c      = obstack_alloc(phase_obst(&rss->ph), sizeof(*c));
			int       source;

			/* there was no source for j -> we have a source of a new chain */
			nodeset_insert(values, xj_irn);

			c->elements = plist_obstack_new(phase_obst(&rss->ph));
			c->nr       = cur_chain++;
			plist_insert_back(c->elements, xj_irn);

			xj_rss->chain = c;

			DBG((rss->dbg, DEBUG_MAX_AC, "\t\tstarting chain %d:\n", c->nr));
			DBG((rss->dbg, DEBUG_MAX_AC, "\t\t\t%+F (%d)", xj_irn, j));

			/* follow chain, having j as source */
			source = j;
			while (assignment[source] >= 0) {
				int       target  = assignment[source];
				int       irn_idx = idx_map[target];
				ir_node   *irn    = get_idx_irn(rss->irg, irn_idx);
				rss_irn_t *node   = get_rss_irn(rss, irn);

				plist_insert_back(c->elements, irn);
				node->chain = c;

				DB((rss->dbg, DEBUG_MAX_AC, " -> %+F (%d)", irn, target));

				/* new source = last target */
				source = target;
			}

			DB((rss->dbg, DEBUG_MAX_AC, "\n"));
		}
	}

	/*
		Computing the maximal antichain: Select an element from each
		chain such, such it is parallel with the others.
	*/
	DBG((rss->dbg, DEBUG_MAX_AC, "\t\tcomputing set of saturation values (MAX AC)\n"));
	DBG((rss->dbg, DEBUG_MAX_AC, "\t\tstarting with:\n"));
	dump_nodeset(values, "\t\t\t");
	temp = NULL;
	do {
		/*
			We need an explicit array for the values as
			we cannot iterate multiple times over the same
			set at the same time. :-(((((
		*/
		int     n         = nodeset_count(values);
		int     i         = 0;
		ir_node **val_arr = NEW_ARR_F(ir_node *, n);

		foreach_nodeset(values, u_irn)
			val_arr[i++] = u_irn;

		if (temp)
			del_nodeset(temp);

		temp = new_nodeset(10);

		/* Select all nodes from current value set, having another node in the set as descendant. */
		for (i = 0; i < n; ++i) {
			rss_irn_t *u = get_rss_irn(rss, val_arr[i]);
			int j;

			for (j = 0; j < n; ++j) {
				if (i != j) {
					rss_edge_t *entry;
					rss_edge_t key;

//					BSEARCH_IRN_ARR(val_arr[j], u->dvg_desc))
					/* v[j] is descendant of u -> remove u and break */
					nodeset_insert(temp, u->irn);
					nodeset_remove(values, u->irn);

					DBG((rss->dbg, DEBUG_MAX_AC, "\t\t\tremoving %+F from values, adding it to temp\n", u->irn));

					break;
				}
			}
		}

		/* Try to insert the chain predecessor of all selected u's */
		foreach_nodeset(temp, u_irn) {
			rss_irn_t       *u  = get_rss_irn(rss, u_irn);
			chain_t         *c  = u->chain;
			plist_element_t *el = plist_find_value(c->elements, u_irn);

			assert(el && "Missing element in chain!");

			/* If u has predecessor in chain: insert the predecessor */
			if (el = plist_element_get_prev(el)) {
				nodeset_insert(values, plist_element_get_value(el));
				DBG((rss->dbg, DEBUG_MAX_AC, "\t\t\tadding %+F to values\n", plist_element_get_value(el)));
			}
		}


		DEL_ARR_F(val_arr);
	} while (nodeset_count(temp) > 0);

	DBG((rss->dbg, DEBUG_MAX_AC, "\t\tfinal set:\n"));
	dump_nodeset(values, "\t\t\t");

	del_nodeset(temp);

	return values;

#undef MAP_IDX
}

static serialization_t *compute_best_admissible_serialization(rss_t *rss, nodeset *sat_vals, serialization_t *ser, int num_regs) {
	int        n                    = nodeset_count(sat_vals);
	int        n_idx                = ARR_LEN(rss->idx_map);
	int        i                    = 0;
	ir_node    **val_arr            = alloca(n * sizeof(val_arr[0]));
	bitset_t   *bs_sv               = bitset_alloca(n_idx);
	bitset_t   *bs_vdesc            = bitset_alloca(n_idx);
	bitset_t   *bs_tmp              = bitset_alloca(n_idx);
	bitset_t   *bs_ukilldesc        = bitset_alloca(n_idx);
	unsigned   best_benefit         = UINT_MAX;
	unsigned   best_omega2          = UINT_MAX;
	unsigned   best_benefit_omega20 = UINT_MAX;
	int        has_positive_omega1  = 0;
	int        j, k;
	ir_node    *irn;
	rss_edge_t min_benefit_edge;
	rss_edge_t min_omega20_edge;

	/*
		We need an explicit array for the values as
		we cannot iterate multiple times over the same
		set at the same time. :-(((((
	*/

	foreach_nodeset(sat_vals, irn) {
		val_arr[i++] = irn;
		bitset_set(bs_sv, BLOCK_IDX_MAP(rss, irn));
	}

	/*
		We build all admissible serializations and remember the best found so far.
		For u in sat_vals:
		 For v in sat_val:
		   if v in pkiller(u): add edge to v from all other pkiller(u)
		   else: for all uu in pkiller(u): add edge to v if there exists no path from v to uu

	*/

	/* for all u in sat_vals */
	for (i = 0; i < n; ++i) {
		rss_irn_t       *u       = get_rss_irn(rss, val_arr[i]);
		int             u_height = get_irn_height(rss->h, val_arr[i]);
		plist_element_t *el;

		/* accumulate all descendants of all pkiller(u) */
		bitset_clear_all(bs_ukilldesc);
		foreach_plist(u->dvg_pkiller_list, el) {
			ir_node   *irn  = plist_element_get_value(el);
			rss_irn_t *node = get_rss_irn(rss, irn);

			if (! is_Sink(irn))
				bitset_set(bs_ukilldesc, BLOCK_IDX_MAP(rss, irn));
			else
				continue;

			for (k = ARR_LEN(node->dvg_desc) - 1; k >= 0; --k) {
				if (! is_Sink(node->dvg_desc[k]))
					bitset_set(bs_ukilldesc, BLOCK_IDX_MAP(rss, node->dvg_desc[k]));
			}
		}

		/* for all v in sat_vals */
		for (j = 0; j < n; ++j) {
			ir_node   *v_irn   = val_arr[j];
			rss_irn_t *v       = get_rss_irn(rss, v_irn);
			unsigned  v_height = get_irn_height(rss->h, v_irn);
			unsigned  omega1, omega2, is_pkiller;

			if (i == j)
				continue;

			/* get descendants of v */
			bitset_clear_all(bs_vdesc);
			for (k = ARR_LEN(v->dvg_desc) - 1; k >= 0; --k) {
				if (! is_Sink(v->dvg_desc[k]))
					bitset_set(bs_vdesc, BLOCK_IDX_MAP(rss, v->dvg_desc[k]));
			}

			/* if v is in pkiller(u) */
			is_pkiller = BSEARCH_IRN_ARR(val_arr[j], u->dvg_pkiller) != NULL ? 1 : 0;

			/* for all vv in pkiller(u) */
			for (k = ARR_LEN(u->dvg_pkiller) - 1; k >= 0; --k) {
				ir_node *vv_irn  = u->dvg_pkiller[k];
				int     add_edge;

				if (is_Sink(vv_irn))
					continue;

				add_edge = is_pkiller ? k != j : ! heights_reachable_in_block(rss->h, v_irn, vv_irn);

				/*
					As we add an edge from vv -> v, we have to make sure,
					that there exists no path from v to vv.
				*/

				if (add_edge) {
					unsigned vv_height = get_irn_height(rss->h, vv_irn);
					unsigned mu1, mu2, critical_path_cost;

					/*
						mu1 = | descendants(v) cut sat_vals |
						the number of saturating values which cannot
						be simultaneously alive with u
					*/
					bitset_copy(bs_tmp, bs_vdesc);
					mu1 = bitset_popcnt(bitset_and(bs_tmp, bs_sv));

					/*
						mu2 = | accum_desc_all_pkiller(u) without descendants(v) |
					*/
					if (is_pkiller) {
						bitset_copy(bs_tmp, bs_ukilldesc);
						mu2 = bitset_popcnt(bitset_andnot(bs_tmp, bs_vdesc));
					}
					else {
						mu2 = 0;
					}

					assert(mu1 >= mu2);

					/* omega1 = mu1 - mu2 */
					omega1 = mu1 - mu2;

					if (omega1 > 0)
						has_positive_omega1 = 1;

					/* omega2 = increase of critical path */
					critical_path_cost =
						v_height                        /* longest path from v to sink */
						+ rss->max_height - vv_height   /* longest path from source to vv */
						+ 1;                            /* edge */

					/*
						If critical_path_cost > max_height -> the new edge
						would increase the longest critical path by the difference.
					*/
					omega2 = critical_path_cost > rss->max_height ? critical_path_cost - rss->max_height : 0;

					/* this keeps track of the edge with the best benefit */
					if (num_regs - omega1 < best_benefit) {
						min_benefit_edge.src = vv_irn;
						min_benefit_edge.tgt = v_irn;

						best_benefit = num_regs - omega1;
					}

					/* this keeps track of the edge with the best omega1 costs where omega2 == 0 */
					if (omega2 == 0 && (num_regs - omega1 < best_benefit_omega20)) {
						min_omega20_edge.src = vv_irn;
						min_omega20_edge.tgt = v_irn;

						best_benefit_omega20 = num_regs - omega1;
					}

					best_omega2 = MIN(best_omega2, omega2);
				} /* if add_edge */
			} /* for all vv in pkiller(u) */
		} /* for all v in sat_vals */
	} /* for all u in sat_vals */

	if (! has_positive_omega1)
		return NULL;

	if (best_omega2 == 0) {
		ser->edge->src = min_omega20_edge.src;
		ser->edge->tgt = min_omega20_edge.tgt;
		ser->omega1    = best_benefit_omega20;
		ser->omega2    = best_omega2;
	}
	else {
		ser->edge->src = min_benefit_edge.src;
		ser->edge->tgt = min_benefit_edge.tgt;
		ser->omega1    = best_benefit;
		ser->omega2    = best_omega2;
	}

	return ser;
}

/**
 * Perform the value serialization heuristic and add all
 * computed serialization edges as dependencies to the irg.
 */
static void perform_value_serialization_heuristic(rss_t *rss) {
	bitset_t *arch_nonign_bs = bitset_alloca(arch_register_class_n_regs(rss->cls));
	bitset_t *abi_ign_bs     = bitset_alloca(arch_register_class_n_regs(rss->cls));
	int      available_regs;
	dvg_t    dvg;
	nodeset  *sat_vals;

	/* available_regs = R = |arch_non_ignore_regs cut ~abi_ignore_regs| */
	arch_put_non_ignore_regs(rss->arch_env, rss->cls, arch_nonign_bs);
	be_abi_put_ignore_regs(rss->abi, rss->cls, abi_ign_bs);
	bitset_andnot(arch_nonign_bs, abi_ign_bs);
	available_regs = bitset_popcnt(arch_nonign_bs);

	DBG((rss->dbg, DEBUG_SER_HEUR, "\n\t#available regs: %d\n\n", available_regs));

	/*
		At first we need to compute the disjoint value DAG (DVG = {V, E_dv}).
		V    = set of all nodes we are currently interested in
		E_dv = there is an edge from u to v iff v is a descendant of killer(u), forall u, v in V
	*/
	dvg.nodes = new_nodeset(plist_count(rss->nodes));
	dvg.edges = new_pset(cmp_rss_edges, 20);
	compute_dvg(rss, &dvg);

	/*
		Then we perform the heuristic serialization algorithm
		on the DVG which gives us all necessary serialization
		edges.
	*/
	DBG((rss->dbg, DEBUG_MAX_AC, "\tcomputing maximal antichain:\n"));
	sat_vals = compute_maximal_antichain(rss, &dvg);
	while (sat_vals && (nodeset_count(sat_vals) > available_regs)) {
		serialization_t *ser, best_ser;
		rss_edge_t      edge;
		rss_irn_t       *tgt;

		best_ser.edge = &edge;
		ser = compute_best_admissible_serialization(rss, sat_vals, &best_ser, available_regs);
		tgt = get_rss_irn(rss, ser->edge->tgt);

		DBG((rss->dbg, DEBUG_SER_HEUR, "\tcurrent register saturation %d, target %d\n", nodeset_count(sat_vals), available_regs));

		/* BEWARE: Update dvg_user_list when inserting a serialization edge !!! */
		plist_insert_back(tgt->dvg_user_list, ser->edge->src);
		pset_insert(dvg.edges, ser->edge, HASH_RSS_EDGE(ser->edge));
		del_nodeset(sat_vals);

		/* TODO: Might be better to update the dvg descendants here as well, instead of recalculating them */

		/* Insert the serialization as dependency edge into the irg. */
		DBG((rss->dbg, DEBUG_SER_HEUR, "\tinserting serialization %+F -> %+F with cost %d, %d\n",
			ser->edge->src, ser->edge->tgt, ser->omega1, ser->omega2));
		add_irn_dep(ser->edge->src, ser->edge->tgt);

		/* TODO: try to find a cheaper way for updating height information */
		rss->max_height = heights_recompute_block(rss->h, rss->block);

		/* Recompute the antichain for next serialization */
		DBG((rss->dbg, DEBUG_MAX_AC, "\tre-computing maximal antichain:\n"));
		sat_vals = compute_maximal_antichain(rss, &dvg);
	}

	del_nodeset(dvg.nodes);
	del_pset(dvg.edges);
}

/**
 * Do initial calculations for a block.
 */
static void process_block(ir_node *block, void *env) {
	rss_t *rss = env;
	int   i, n;
	const ir_edge_t *edge;

	phase_init(&rss->ph, "rss block preprocessor", rss->irg, PHASE_DEFAULT_GROWTH, init_rss_irn);

	DBG((rss->dbg, LEVEL_1, "preprocessing block %+F\n", block));
	rss->block = block;

	/* build an index map for all nodes in the current block */
	i            = 0;
	n            = get_irn_n_edges(block);
	NEW_ARR_A(int *, rss->idx_map, n);
	foreach_out_edge(block, edge) {
		ir_node *irn      = get_edge_src_irn(edge);
		rss->idx_map[i++] = get_irn_idx(irn);
	}
	qsort(rss->idx_map, n, sizeof(rss->idx_map[0]), cmp_int);
	rss->max_height = heights_recompute_block(rss->h, block);

	/* loop over all register classes */
	for (i = arch_isa_get_n_reg_class(rss->arch_env->isa) - 1; i >= 0; --i) {
		const arch_register_class_t *cls = arch_isa_get_reg_class(rss->arch_env->isa, i);

		rss->cls = cls;
		DBG((rss->dbg, LEVEL_1, "register class %s\n", arch_register_class_name(cls)));

		/* reset the list of interesting nodes */
		plist_clear(rss->nodes);
		plist_insert_back(rss->nodes, _sink);

		/* collect all nodes having a certain register class */
		foreach_out_edge(block, edge) {
			ir_node *irn = get_edge_src_irn(edge);

			if (get_irn_mode(irn) == mode_T)
				continue;

			if (! arch_irn_is(rss->arch_env, irn, ignore) && arch_get_irn_reg_class(rss->arch_env, irn, -1) == cls) {
				plist_insert_back(rss->nodes, irn);

				/* calculate the descendants and consumer for each node in the block */
				collect_node_info(rss, irn);
			}
		}

		/* compute the potential killing set PK(G) */
		compute_pkill_set(rss);

		/* compute the killing function k* */
		compute_killing_function(rss);

		/*
			Compute the heuristic value serialization and
			add the necessary dependencies to the irg.
		*/
		perform_value_serialization_heuristic(rss);
	}

	phase_free(&rss->ph);
}

/**
 * Preprocess the irg for scheduling.
 */
void rss_schedule_preparation(const be_irg_t *birg) {
	rss_t rss;

	FIRM_DBG_REGISTER(rss.dbg, "firm.be.sched.rss");

	firm_dbg_set_mask(rss.dbg, 255);

	init_rss_special_nodes(birg->irg);

	rss.irg      = birg->irg;
	rss.arch_env = birg->main_env->arch_env;
	rss.abi      = birg->abi;
	rss.h        = heights_new(birg->irg);
	rss.nodes    = plist_new();
	irg_block_walk_graph(birg->irg, NULL, process_block, &rss);
	heights_free(rss.h);
	plist_free(rss.nodes);
}
