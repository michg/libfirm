/*
 * Project:     libFIRM
 * File name:   ir/ir/irprog_t.h
 * Purpose:     Entry point to the representation of a whole program 0-- private header.
 * Author:      Goetz Lindenmaier
 * Modified by:
 * Created:     2000
 * CVS-ID:      $Id$
 * Copyright:   (c) 2000-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/**
 * @file irprog_t.h
 */

# ifndef _IRPROG_T_H_
# define _IRPROG_T_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "irprog.h"
#include "irgraph.h"
#include "ircgcons.h"
#include "firm_common_t.h"
#include "typegmod.h"

#include "array.h"

/** ir_prog */
struct ir_prog {
  firm_kind kind;
  ir_graph  *main_irg;            /**< entry point to the compiled program
				     @@@ or a list, in case we compile a library or the like? */
  ir_graph **graphs;              /**< all graphs in the ir */
  type      *glob_type;           /**< global type.  Must be a class as it can
				     have fields and procedures.  */
  type     **types;               /**< all types in the ir */
  ir_graph  *const_code_irg;      /**< This ir graph gives the proper environment
				     to allocate nodes the represent values
				     of constant entities. It is not meant as
				     a procedure.  */

  irg_outs_state outs_state;     /**< Out edges. */
  ir_node **ip_outedges;         /**< Huge Array that contains all out edges
				    in interprocedural view. */
  ip_view_state ip_view;         /**< State of interprocedural view. */
  ident     *name;
  /*struct obstack *obst;	   * @@@ Should we place all types and
                                     entities on an obstack, too? */

#ifdef DEBUG_libfirm
  long max_node_nr;                /**< to generate unique numbers for nodes. */
#endif
};

INLINE void remove_irp_type_from_list (type *typ);

static INLINE type *
__get_glob_type(void) {
  assert(irp);
  return irp->glob_type = skip_tid(irp->glob_type);
}

static INLINE int
__get_irp_n_irgs(void) {
  assert (irp && irp->graphs);
  /* Strangely the first element of the array is NULL.  Why??  */
  return (ARR_LEN((irp)->graphs));
}

static INLINE ir_graph *
__get_irp_irg(int pos){
  assert (irp && irp->graphs);
  /* Strangely the first element of the array is NULL.  Why??  */
  return irp->graphs[pos];
}


static INLINE int
__get_irp_n_types (void) {
  assert (irp && irp->types);
  /* Strangely the first element of the array is NULL.  Why??  */
  return (ARR_LEN((irp)->types));
}

static INLINE type *
__get_irp_type(int pos) {
  assert (irp && irp->types);
  /* Strangely the first element of the array is NULL.  Why??  */
  /* Don't set the skip_tid result so that no double entries are generated. */
  return skip_tid(irp->types[pos]);
}

#ifdef DEBUG_libfirm
/** Returns a new, unique number to number nodes or the like. */
int get_irp_new_node_nr(void);
#endif

static INLINE ir_graph *
__get_const_code_irg(void)
{
  return irp->const_code_irg;
}

#define get_irp_n_irgs()       __get_irp_n_irgs()
#define get_irp_irg(pos)       __get_irp_irg(pos)
#define get_irp_n_types()      __get_irp_n_types()
#define get_irp_type(pos)      __get_irp_type(pos)
#define get_const_code_irg()   __get_const_code_irg()
#define get_glob_type()        __get_glob_type()

#endif /* ifndef _IRPROG_T_H_ */
