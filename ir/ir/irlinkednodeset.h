/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
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
 * @author    Michael Beck
 * @brief     A linked nodeset.
 */
#ifndef _FIRM_IRLINKEDNODESET_H_
#define _FIRM_IRLINKEDNODESET_H_

#include "firm_types.h"
#include "xmalloc.h"
#include "list.h"

typedef struct ir_lnk_nodeset_entry_t {
	ir_node     *node;  /**< the node itself */
	list_head   list;   /**< link field for the list iterator */
} ir_lnk_nodeset_entry_t;

#define HashSet          ir_lnk_nodeset_t
#define HashSetIterator  ir_lnk_nodeset_iterator_t
#define ValueType        ir_lnk_nodeset_entry_t
#define ADDITIONAL_DATA  list_head elem_list; list_head all_iters;
#define DO_REHASH
#define NO_ITERATOR

#include "hashset.h"

#undef NO_ITERATOR
#undef DO_REHASH
#undef ADDITIONAL_DATA
#undef ValueType
#undef HashSetIterator
#undef HashSet

typedef struct ir_lnk_nodeset_t ir_lnk_nodeset_t;
typedef struct ir_lnk_nodeset_iterator_t {
	list_head              *iter;       /**< points to the list head of the last element */
	const ir_lnk_nodeset_t *nodeset;    /**< lithe nodeset of this iterator. */
} ir_lnk_nodeset_iterator_t;

/**
 * Initializes a linked nodeset with default size.
 *
 * @param nodeset      Pointer to allocated space for the nodeset
 */
void ir_lnk_nodeset_init(ir_lnk_nodeset_t *nodeset);

/**
 * Initializes a linked nodeset.
 *
 * @param nodeset             Pointer to allocated space for the nodeset
 * @param expected_elements   Number of elements expected in the nodeset (roughly)
 */
void ir_lnk_nodeset_init_size(ir_lnk_nodeset_t *nodeset, size_t expected_elements);

/**
 * Destroys a nodeset and frees the memory allocated for hashtable. The memory of
 * the nodeset itself is not freed.
 *
 * @param nodeset   Pointer to the nodeset
 */
void ir_lnk_nodeset_destroy(ir_lnk_nodeset_t *nodeset);

/**
 * Allocates memory for a linked nodeset and initializes the set.
 *
 * @param expected_elements   Number of elements expected in the nodeset (roughly)
 * @return The initialized nodeset
 */
static inline ir_lnk_nodeset_t *ir_lnk_nodeset_new(size_t expected_elements) {
	ir_lnk_nodeset_t *res = XMALLOC(ir_lnk_nodeset_t);
	ir_lnk_nodeset_init_size(res, expected_elements);
	return res;
}

/**
 * Destroys a linked nodeset and frees the memory of the nodeset itself.
 */
static inline void ir_lnk_nodeset_del(ir_lnk_nodeset_t *nodeset) {
	ir_lnk_nodeset_destroy(nodeset);
	xfree(nodeset);
}

/**
 * Inserts a node into a linked nodeset.
 *
 * @param nodeset   Pointer to the nodeset
 * @param node      node to insert into the nodeset
 * @returns         1 if the element has been inserted,
 *                  0 if it was already there
 */
int ir_lnk_nodeset_insert(ir_lnk_nodeset_t *nodeset, ir_node *node);


/**
 * Removes a node from a linked nodeset. Does nothing if the nodeset doesn't contain
 * the node.
 *
 * @param nodeset  Pointer to the nodeset
 * @param node     Node to remove from the nodeset
 */
void ir_lnk_nodeset_remove(ir_lnk_nodeset_t *nodeset, const ir_node *node);

/**
 * Tests whether a linked nodeset contains a specific node.
 *
 * @param nodeset   Pointer to the nodeset
 * @param node      The pointer to find
 * @returns         1 if nodeset contains the node, 0 else
 */
int ir_lnk_nodeset_contains(const ir_lnk_nodeset_t *nodeset, const ir_node *node);

/**
 * Returns the number of nodes contained in the linked nodeset.
 *
 * @param nodeset   Pointer to the nodeset
 * @returns         Number of nodes contained in the linked nodeset
 */
size_t ir_lnk_nodeset_size(const ir_lnk_nodeset_t *nodeset);

/**
 * Initializes a nodeset iterator. Sets the iterator before the first element in
 * the linked nodeset.
 *
 * @param iterator   Pointer to already allocated iterator memory
 * @param nodeset       Pointer to the nodeset
 */
void ir_lnk_nodeset_iterator_init(ir_lnk_nodeset_iterator_t *iterator,
                                  const ir_lnk_nodeset_t *nodeset);

/**
 * Advances the iterator and returns the current element or NULL if all elements
 * in the linked nodeset have been processed.
 * @attention It is not allowed to use ir_lnk_nodeset_insert or ir_lnk_nodeset_remove while
 *            iterating over a nodeset.
 *
 * @param iterator  Pointer to the nodeset iterator.
 * @returns         Next element in the nodeset or NULL
 */
ir_node *ir_lnk_nodeset_iterator_next(ir_lnk_nodeset_iterator_t *iterator);

/**
 * Removes the element the iterator currently points to.
 *
 * @param nodeset   Pointer to the linked nodeset
 * @param iterator  Pointer to the linked nodeset iterator.
 */
void ir_lnk_nodeset_remove_iterator(ir_lnk_nodeset_t *nodeset,
                                    ir_lnk_nodeset_iterator_t *iterator);

#define foreach_ir_lnk_nodeset(nodeset, irn, iter) \
	for (ir_lnk_nodeset_iterator_init(&iter, nodeset), \
        irn = ir_lnk_nodeset_iterator_next(&iter);    \
		irn != NULL; irn = ir_lnk_nodeset_iterator_next(&iter))

#endif
