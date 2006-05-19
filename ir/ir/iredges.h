/*
 * Project:     libFIRM
 * File name:   ir/ir/iredges.h
 * Purpose:     Public header for the automatically updating outs.
 * Author:      Sebastian Hack
 * Created:     3.2.2005
 * CVS-ID:      $Id$
 * Copyright:   (c) 1998-2005 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/**
 * Public header for the automatically updating outs.
 * @author Sebastian Hack
 * @date 3.2.2005
 */

#ifndef _FIRM_IR_EDGES_H
#define _FIRM_IR_EDGES_H

#include "firm_types.h"

#ifndef _IR_EDGE_TYPEDEF_
#define _IR_EDGE_TYPEDEF_
typedef struct _ir_edge_t ir_edge_t;
#endif

#ifndef _IR_BLOCK_EDGE_TYPEDEF_
#define _IR_BLOCK_EDGE_TYPEDEF_
typedef struct _ir_block_edge_t ir_block_edge_t;
#endif

/**
 * Get the first edge pointing to some node.
 * @note There is no order on out edges. First in this context only
 * means, that you get some starting point into the list of edges.
 * @param irn The node.
 * @return The first out edge that points to this node.
 */
const ir_edge_t *get_irn_out_edge_first(const ir_node *irn);

/**
 * Get the next edge in the out list of some node.
 * @param irn The node.
 * @param last The last out edge you have seen.
 * @return The next out edge in @p irn 's out list after @p last.
 */
const ir_edge_t *get_irn_out_edge_next(const ir_node *irn,
		const ir_edge_t *last);

/**
 * A convenience iteration macro over all out edges of a node.
 * @param irn  The node.
 * @param edge An ir_edge_t pointer which shall be set to the current
 * edge.
 */
#define foreach_out_edge(irn,edge) \
	for(edge = get_irn_out_edge_first(irn); edge; edge = get_irn_out_edge_next(irn, edge))

/**
 * A convenience iteration macro over all out edges of a node, which is safe
 * against alteration of the current edge.
 *
 * @param irn  The node.
 * @param edge An ir_edge_t pointer which shall be set to the current
 * edge.
 * @param ne   The next edge, enables alteration safe edge processing.
 */
#define foreach_out_edge_safe(irn,edge,ne) \
	for( \
		(edge) = (get_irn_out_edge_first(irn)), \
			(ne) = ((edge) ? (get_irn_out_edge_next(irn, edge)) : NULL); \
		edge; \
		(edge) = (ne), (ne) = ((edge) ? (get_irn_out_edge_next(irn, edge)) : NULL) \
	)


/**
 * A convenience iteration macro for all control flow edges
 * leaving a block, and thus are cf successor edges.
 * @param bl The block.
 * @param edge An @c ir_edge_t pointer which is set to the current edge.
 */
#define foreach_block_succ(bl,edge) \
	for(edge = get_block_succ_first(bl); edge; edge = get_block_succ_next(bl, edge))

/*
 * Get the source node of an edge.
 * @param edge The edge.
 * @return The source node of that edge.
 */
ir_node *get_edge_src_irn(const ir_edge_t *edge);

/**
 * Get the number of edges pointing to a node.
 * @param irn The node.
 * @return The number of edges pointing to this node.
 */
int get_irn_n_edges(const ir_node *irn);

/**
 * Get the position of an edge.
 * @param edge The edge.
 * @return The position in the in array of that edges source.
 */
extern int get_edge_src_pos(const ir_edge_t *edge);

/**
 * Get the edge object of an outgoing edge at a node.
 * @param   irg The graph, the node is in.
 * @param   irn The node at which the edge originates.
 * @param   pos The position of the edge.
 * @return      The corresponding edge object or NULL,
 *              if no such edge exists.
 */
const ir_edge_t *get_irn_edge(ir_graph *irg, const ir_node *irn, int pos);

/**
 * Check, if the out edges are activated.
 * @param irg The graph.
 * @return 1, if the edges are present for the given irg, 0 if not.
 */
extern int edges_activated(const ir_graph *irg);

/**
 * Activate the edges for an irg.
 * @param irg The graph to activate the edges for.
 **/
extern void edges_activate(ir_graph *irg);

/**
 * Deactivate the edges for an irg.
 * @param irg The graph.
 */
extern void edges_deactivate(ir_graph *irg);

#endif /* _FIRM_IR_EDGES_H */
