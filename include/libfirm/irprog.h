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
 * @brief   Entry point to the representation of a whole program.
 * @author  Goetz Lindenmaier
 * @date    2000
 * @version $Id$
 * @summary
 *  Intermediate Representation (IR) of a program.
 *
 *  This file defines a construct that keeps all information about a
 *  program:
 *   - A reference point to the method to be executed on program start.
 *   - A list of all procedures.
 *   - A list of all types.
 *   - A global type that contains all global variables and procedures that do
 *     not belong to a class.  This type represents the data segment of the
 *     program.  It is not the base class of
 *     all classes in a class hierarchy (as, e.g., "object" in java).
 *   - A degenerated graph that contains constant expressions.
 *   - interprocedural outs state.
 *   - a flag indicating validity of the interprocedural representation.
 *   - the output file name
 */
#ifndef FIRM_IR_IRPROG_H
#define FIRM_IR_IRPROG_H

#include "firm_types.h"
#include "irgraph.h"

typedef enum ir_segment_t {
	IR_SEGMENT_FIRST,
	/** "normal" global data */
	IR_SEGMENT_GLOBAL = IR_SEGMENT_FIRST,
	/** thread local storage segment */
	IR_SEGMENT_THREAD_LOCAL,
	/**
	 * the constructors segment. Contains pointers to functions which are
	 * executed on module initialization (program start or when a library is
	 * dynamically loaded)
	 */
	IR_SEGMENT_CONSTRUCTORS,
	/** like constructors, but functions are executed on module exit */
	IR_SEGMENT_DESTRUCTORS,

	IR_SEGMENT_COUNT
} ir_segment_t;

/**
 * A variable pointing to the current irp (program or module).
 * This variable should be considered constant. Moreover, one should use get_irp()
 * to get access the the irp.
 *
 * @note
 * 	Think of the irp as the "handle" of a program.
 */
extern ir_prog *irp;

#ifndef NDEBUG
void irp_reserve_resources(ir_prog *irp, ir_resources_t resources);
void irp_free_resources(ir_prog *irp, ir_resources_t resources);
ir_resources_t irp_resources_reserved(const ir_prog *irp);
#else
#define irp_reserve_resources(irp, resources)
#define irp_free_resources(irp, resources)
#define irp_resources_reserved(irp)   0
#endif

/**
 * Returns the current irp from where everything in the current module
 * can be accessed.
 *
 * @see irp
 */
ir_prog *get_irp(void);

/**
 * Creates a new ir_prog (a module or compilation unit),
 * returns it and sets irp with it.
 *
 * @param module_name  the name of this irp (module)
 */
ir_prog *new_ir_prog(const char *name);

/** frees all memory used by irp.  Types in type list and irgs in irg
 *  list must be freed by hand before. */
void free_ir_prog(void);

/** Sets the file name / executable name or the like. Initially the
    ident 'no_name_set'. */
void set_irp_prog_name(ident *name);

/** Returns true if the user ever set a program name */
int irp_prog_name_is_set(void);

/** Gets the name of the current irp. */
ident *get_irp_ident(void);

/** Gets the name of the current irp. */
const char *get_irp_name(void);

/** Gets the main routine of the compiled program. */
ir_graph *get_irp_main_irg(void);

/** Sets the main routine of the compiled program. */
void set_irp_main_irg(ir_graph *main_irg);

/** Adds irg to the list of ir graphs in the current irp. */
void add_irp_irg(ir_graph *irg);

/** Removes irg from the list of irgs and
    shrinks the list by one. */
void remove_irp_irg_from_list(ir_graph *irg);
/** Removes irg from the list of irgs, deallocates it and
    shrinks the list by one. */
void remove_irp_irg(ir_graph *irg);

/** returns the biggest not used irg index number */
int get_irp_last_idx(void);

/** Returns the number of ir graphs in the irp. */
int get_irp_n_irgs(void);

/** Returns the ir graph at position pos in the irp. */
ir_graph *get_irp_irg(int pos);

/** Sets the ir graph at position pos. */
void set_irp_irg(int pos, ir_graph *irg);

/** Gets the number of graphs _and_ pseudo graphs. */
int get_irp_n_allirgs(void);

/** Returns the ir graph at position pos of all graphs (including
 pseudo graphs).  Visits first graphs, then pseudo graphs. */
ir_graph *get_irp_allirg(int pos);

/**
 * Returns the type containing the entities for a segment.
 *
 * @param segment  the segment
 */
ir_type *get_segment_type(ir_segment_t segment);

/**
 * Returns the "global" type of the irp.
 * Upon creation this is an empty class type.
 * This is a convenience function for get_segment_type(IR_SEGMENT_GLOBAL)
 */
ir_type *get_glob_type(void);

/**
 * Returns the "thread local storage" type of the irp.
 * Upon creation this is an empty struct type.
 */
ir_type *get_tls_type(void);

/** Adds type to the list of types in irp. */
void add_irp_type(ir_type *typ);

/** Removes type from the list of types, deallocates it and
    shrinks the list by one. */
void remove_irp_type(ir_type *typ);

/** Returns the number of all types in the irp. */
int get_irp_n_types(void);

/** Returns the type at position pos in the irp. */
ir_type *get_irp_type(int pos);

/** Overwrites the type at position pos with another type. */
void set_irp_type(int pos, ir_type *typ);

/** Returns the number of all modes in the irp. */
int get_irp_n_modes(void);

/** Returns the mode at position pos in the irp. */
ir_mode *get_irp_mode(int pos);

/** Adds opcode to the list of opcodes in irp. */
void add_irp_opcode(ir_op *opcode);

/** Removes opcode from the list of opcodes, deallocates it and
    shrinks the list by one. */
void remove_irp_opcode(ir_op *opcode);

/** Returns the number of all opcodes in the irp. */
int get_irp_n_opcodes(void);

/** Returns the opcode at position pos in the irp. */
ir_op *get_irp_opcode(int pos);

/** Sets the generic function pointer of all opcodes to NULL */
void clear_irp_opcodes_generic_func(void);


/**  Return the graph for global constants of the current irp.
 *
 *   Returns an irgraph that only contains constant expressions for
 *   constant entities.  Do not use any access function for this
 *   graph, do not generate code for this graph.  This graph contains
 *   only one block.  The constant expressions may not contain control
 *   flow.
 *   Walking the graph starting from any node will not reach the block
 *   or any controlflow.
 *   See also copy_const_code() in entity.h.
 */
ir_graph *get_const_code_irg(void);


/** The phase state for the program.
 *
 *  The phase state of the whole program is
 *   building:  if at least one graph is state_building
 *              or one type is incomplete.
 *   high:      all graphs are in state high or low, all types are constructed.
 *   low:       all graphs are in state low, all types are in state layout fixed.
 */
irg_phase_state get_irp_phase_state(void);
void            set_irp_phase_state(irg_phase_state s);

irg_outs_state get_irp_ip_outs_state(void);
void           set_irp_ip_outs_inconsistent(void);


irg_callee_info_state get_irp_callee_info_state(void);
void                  set_irp_callee_info_state(irg_callee_info_state s);

/** Returns a new, unique exception region number. */
ir_exc_region_t get_irp_next_region_nr(void);

/** Returns a new, unique label number. */
ir_label_t get_irp_next_label_nr(void);

/** Add a new global asm include. */
void add_irp_asm(ident *asm_string);

/** Return the number of global asm includes. */
int get_irp_n_asms(void);

/** Return the global asm include at position pos. */
ident *get_irp_asm(int pos);

#endif
