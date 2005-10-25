/*
 * Project:     libFIRM
 * File name:   ir/ana/analyze_irg_agrs.c
 * Purpose:     read/write analyze of graph argument, which have mode reference.
 * Author:      Beyhan Veliev
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1998-2005 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/**
 * @file analyze_irg_agrs.c
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#include <stdlib.h>

#include "irouts.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "array.h"
#include "irprog.h"
#include "entity_t.h"

#include "analyze_irg_args.h"

static char v;
static void *VISITED = &v;

/**
 * Walk recursive the successors of a graph argument
 * with mode reference and mark in the "irg_args" if it will be read,
 * written or stored.
 *
 * @param arg   The graph argument with mode reference,
 *             that must be checked.
 */
static unsigned analyze_arg(ir_node *arg, unsigned bits)
{
  int i, p;
  ir_node *succ;

  /* We must visit a node once to avoid endless recursion.*/
  set_irn_link(arg, VISITED);

  for (i = get_irn_n_outs(arg) - 1; i >= 0; --i) {
    succ = get_irn_out(arg, i);

    /* We was here.*/
    if (get_irn_link(succ) == VISITED)
      continue;

    /* We should not walk over the memory edge.*/
    if (get_irn_mode(succ) == mode_M)
      continue;

    /* If we reach with the recursion a Call node and our reference
       isn't the address of this Call we accept that the reference will
       be read and written if the graph of the method represented by
       "Call" isn't computed else we analyze that graph. If our
       reference is the address of this
       Call node that mean the reference will be read.*/
    if (get_irn_op(succ) == op_Call) {
      ir_node *Call_ptr  = get_Call_ptr(succ);

      if (Call_ptr == arg) {
        /* Hmm: not sure what this is, most likely a read */
        bits |= ptr_access_read;
      }
      else if (op_SymConst == get_irn_op(Call_ptr) &&
	      get_SymConst_kind(Call_ptr) == symconst_addr_ent) {
        entity *meth_ent = get_SymConst_entity(Call_ptr);

	      for (p = get_Call_n_params(succ) - 1; p >= 0; --p){
	        if (get_Call_param(succ, p) == arg) {
            /* an arg can be used more than once ! */
            bits |= get_method_param_access(meth_ent, p);
	        }
	      }
      } else /* can do anything */
        bits |= ptr_access_all;

      /* search stops here anyway */
      continue;
    }
    else if (get_irn_op(succ) == op_Store) {
      /* We have reached a Store node => the reference is written or stored. */
      if (get_Store_ptr(succ) == arg) {
        /* written to */
        bits |= ptr_access_write;
      }
      else {
        /* stored itself */
        bits |= ptr_access_store;
      }

      /* search stops here anyway */
      continue;
   }
    else if (get_irn_op(succ) == op_Load) {
      /* We have reached a Load node => the reference is read. */
      bits |= ptr_access_read;

      /* search stops here anyway */
      continue;
    }
    else if (get_irn_op(succ) == op_Conv) {
      /* our address is casted into something unknown. Break our search. */
      return ptr_access_all;
    }

    /* If we know that, the argument will be read, write and stored, we
       can break the recursion.*/
    if (bits == ptr_access_all)
      return ptr_access_all;

    /*
     * A calculation that do not lead to a reference mode ends our search.
     * This is dangerous: It would allow to cast into integer and that cast back ...
     * so, when we detect a Conv we go mad, see the Conv case above.
     */
    if (!mode_is_reference(get_irn_mode(succ)))
      continue;

    /* follow further the address calculation */
    bits = analyze_arg(succ, bits);
  }
  set_irn_link(arg, NULL);
  return bits;
}


/**
 * Check if a argument of the ir graph with mode
 * reference is read, write or both.
 *
 * @param irg   The ir graph to analyze.
 */
static void analyze_ent_args(entity *ent)
{
  ir_graph *irg;
  ir_node *irg_args, *arg;
  ir_mode *arg_mode;
  int nparams, i;
  long proj_nr;
  type *mtp;
  ptr_access_kind *rw_info;

  mtp     = get_entity_type(ent);
  nparams = get_method_n_params(mtp);

  ent->param_access = NEW_ARR_F(ptr_access_kind, nparams);

  /* If the method haven't parameters we have
   * nothing to do.
   */
  if (nparams <= 0)
    return;

  irg = get_entity_irg(ent);

  /* we have not yet analysed the graph, set ALL access for pointer args */
  for (i = nparams - 1; i >= 0; --i)
    ent->param_access[i] =
      is_Pointer_type(get_method_param_type(mtp, i)) ? ptr_access_all : ptr_access_none;

  if (! irg) {
    /* no graph, no better info */
    return;
  }

  /* Call algorithm that computes the out edges */
  if (get_irg_outs_state(irg) != outs_consistent)
    compute_outs(irg);

  irg_args = get_irg_args(irg);

  /* A array to save the information for each argument with
     mode reference.*/
  NEW_ARR_A(ptr_access_kind, rw_info, nparams);

  /* We initialize the element with none state. */
  for (i = nparams - 1; i >= 0; --i)
    rw_info[i] = ptr_access_none;

  /* search for arguments with mode reference
     to analyze them.*/
  for (i = get_irn_n_outs(irg_args) - 1; i >= 0; --i) {
    arg      = get_irn_out(irg_args, i);
    arg_mode = get_irn_mode(arg);
    proj_nr  = get_Proj_proj(arg);

    if (mode_is_reference(arg_mode))
      rw_info[proj_nr] |= analyze_arg(arg, rw_info[proj_nr]);
  }

  /* copy the temporary info */
  memcpy(ent->param_access, rw_info, nparams * sizeof(ent->param_access[0]));

  printf("\n%s:\n", get_entity_name(ent));
  for (i = 0; i < nparams; ++i) {
    if (is_Pointer_type(get_method_param_type(mtp, i)))
      if (ent->param_access[i] != ptr_access_none) {
        printf("  Pointer Arg %d access: ", i);
        if (ent->param_access[i] & ptr_access_read)
          printf("READ ");
        if (ent->param_access[i] & ptr_access_write)
          printf("WRITE ");
        if (ent->param_access[i] & ptr_access_store)
          printf("STORE ");
        printf("\n");
      }
  }
}

/*
 * Compute for a method with pointer parameter(s)
 * if they will be read or written.
 */
ptr_access_kind get_method_param_access(entity *ent, int pos)
{
  type *mtp = get_entity_type(ent);
  int  is_variadic = get_method_variadicity(mtp) == variadicity_variadic;

  assert(0 <= pos && (is_variadic || pos < get_method_n_params(mtp)));

  if (ent->param_access) {
    if (pos < ARR_LEN(ent->param_access))
      return ent->param_access[pos];
    else
      return ptr_access_all;
  }

  analyze_ent_args(ent);

  if (pos < ARR_LEN(ent->param_access))
    return ent->param_access[pos];
  else
    return ptr_access_all;
}

/**
 * Analyze how pointer arguments of a given
 * ir graph are accessed.
 *
 * @param irg   The ir graph to analyze.
 */
void analyze_irg_args(ir_graph *irg)
{
  entity *ent;

  if (irg == get_const_code_irg())
    return;

  ent = get_irg_entity(irg);
  if (! ent)
    return;

  if (! ent->param_access)
    analyze_ent_args(ent);
}
