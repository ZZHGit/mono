/*
 * seq-points.c: Sequence Points functions
 *
 * Authors:
 *   Marcos Henrich (marcos.henrich@xamarin.com)
 *
 * Copyright 2014 Xamarin, Inc (http://www.xamarin.com)
 */

#include "mini.h"
#include "seq-points.h"

static void
insert_pred_seq_point (MonoBasicBlock *in_bb, MonoInst *ins, GSList **next)
{
	GSList *l;
	int src_index = in_bb->last_seq_point->backend.size;
	int dst_index = ins->backend.size;

	/* bb->in_bb might contain duplicates */
	for (l = next [src_index]; l; l = l->next)
		if (GPOINTER_TO_UINT (l->data) == dst_index)
			break;
	if (!l)
		next [src_index] = g_slist_append (next [src_index], GUINT_TO_POINTER (dst_index));
}

static void
collect_pred_seq_points (MonoBasicBlock *bb, MonoInst *ins, GSList **next, GHashTable *memoize)
{
	const gpointer MONO_SEQ_SEEN_LOOP = GINT_TO_POINTER(-1);

	for (int i = 0; i < bb->in_count; ++i) {
		MonoBasicBlock *in_bb = bb->in_bb [i];
		gpointer result = g_hash_table_lookup (memoize, in_bb);

		if (result == MONO_SEQ_SEEN_LOOP) {
			// We've looped or handled this before, exit early.
			// No last sequence points to find.
			continue;
		} else if (in_bb->last_seq_point) {
			// if last seq point, insert into next
			insert_pred_seq_point (in_bb, ins, next);
		} else {
			// Compute predecessors of in_bb

			// Insert/remove sentinel into the memoize table to detect loops containing in_bb
			// This works to ensure that we only have a basic block on the stack once
			// at any given time
			g_hash_table_insert (memoize, in_bb, MONO_SEQ_SEEN_LOOP);
			collect_pred_seq_points (in_bb, ins, next, memoize);
			g_hash_table_remove (memoize, in_bb);
		}
	}
}

void
mono_save_seq_point_info (MonoCompile *cfg)
{
	MonoBasicBlock *bb;
	GSList *bb_seq_points, *l;
	MonoInst *last;
	MonoDomain *domain = cfg->domain;
	int i, seq_info_size;
	GSList **next = NULL;
	SeqPoint* seq_points;
	GByteArray* array;
	gboolean has_debug_data = cfg->gen_sdb_seq_points;

	if (!cfg->seq_points)
		return;

	seq_points = g_new0 (SeqPoint, cfg->seq_points->len);

	for (i = 0; i < cfg->seq_points->len; ++i) {
		SeqPoint *sp = &seq_points [i];
		MonoInst *ins = (MonoInst *)g_ptr_array_index (cfg->seq_points, i);

		sp->il_offset = ins->inst_imm;
		sp->native_offset = ins->inst_offset;
		if (ins->flags & MONO_INST_NONEMPTY_STACK)
			sp->flags |= MONO_SEQ_POINT_FLAG_NONEMPTY_STACK;

		/* Used below */
		ins->backend.size = i;
	}

	if (has_debug_data) {
		/*
		 * For each sequence point, compute the list of sequence points immediately
		 * following it, this is needed to implement 'step over' in the debugger agent.
		 */
		next = g_new0 (GSList*, cfg->seq_points->len);
		GHashTable *memoize = g_hash_table_new (NULL, NULL);
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			bb_seq_points = g_slist_reverse (bb->seq_points);
			last = NULL;
			for (l = bb_seq_points; l; l = l->next) {
				MonoInst *ins = (MonoInst *)l->data;

				if (ins->inst_imm == METHOD_ENTRY_IL_OFFSET || ins->inst_imm == METHOD_EXIT_IL_OFFSET)
				/* Used to implement method entry/exit events */
					continue;
				if (ins->inst_offset == SEQ_POINT_NATIVE_OFFSET_DEAD_CODE)
					continue;

				if (last != NULL) {
					/* Link with the previous seq point in the same bb */
					next [last->backend.size] = g_slist_append (next [last->backend.size], GUINT_TO_POINTER (ins->backend.size));
				} else {
					/* Link with the last bb in the previous bblocks */
					collect_pred_seq_points (bb, ins, next, memoize);
				}

				last = ins;
			}

			/* The second case handles endfinally opcodes which are in a separate bb by themselves */
			if ((bb->last_ins && bb->last_ins->opcode == OP_ENDFINALLY && bb->seq_points) || (bb->out_count == 1 && bb->out_bb [0]->code && bb->out_bb [0]->code->opcode == OP_ENDFINALLY)) {
				MonoBasicBlock *bb2;
				MonoInst *endfinally_seq_point = NULL;

				/*
				 * The ENDFINALLY branches are not represented in the cfg, so link it with all seq points starting bbs.
				 */
				l = g_slist_last (bb->seq_points);
				if (l) {
					endfinally_seq_point = (MonoInst *)l->data;

					for (bb2 = cfg->bb_entry; bb2; bb2 = bb2->next_bb) {
						GSList *l = g_slist_last (bb2->seq_points);

						if (l) {
							MonoInst *ins = (MonoInst *)l->data;

							if (!(ins->inst_imm == METHOD_ENTRY_IL_OFFSET || ins->inst_imm == METHOD_EXIT_IL_OFFSET) && ins != endfinally_seq_point)
								next [endfinally_seq_point->backend.size] = g_slist_append (next [endfinally_seq_point->backend.size], GUINT_TO_POINTER (ins->backend.size));
						}
					}
				}
			}
		}
		g_hash_table_destroy (memoize);

		if (cfg->verbose_level > 2) {
			printf ("\nSEQ POINT MAP: \n");

			for (i = 0; i < cfg->seq_points->len; ++i) {
				SeqPoint *sp = &seq_points [i];
				GSList *l;

				if (!next [i])
					continue;

				printf ("\tIL0x%x[0x%0x] ->", sp->il_offset, sp->native_offset);
				for (l = next [i]; l; l = l->next) {
					int next_index = GPOINTER_TO_UINT (l->data);
					printf (" IL0x%x", seq_points [next_index].il_offset);
				}
				printf ("\n");
			}
		}
	}

	array = g_byte_array_new ();

	{ /* Add sequence points to seq_point_info */
		SeqPoint zero_seq_point = {0};
		SeqPoint* last_seq_point = &zero_seq_point;

		for (i = 0; i < cfg->seq_points->len; ++i) {
			SeqPoint *sp = &seq_points [i];
			GSList* next_list = NULL;

			if (has_debug_data)
				next_list = next[i];

			if (mono_seq_point_info_add_seq_point (array, sp, last_seq_point, next_list, has_debug_data))
				last_seq_point = sp;

			if (has_debug_data)
				g_slist_free (next [i]);
		}
	}

	if (has_debug_data)
		g_free (next);

	cfg->seq_point_info = mono_seq_point_info_new (array->len, TRUE, array->data, has_debug_data, &seq_info_size);
	mono_jit_stats.allocated_seq_points_size += seq_info_size;

	g_byte_array_free (array, TRUE);

	// FIXME: dynamic methods
	if (!cfg->compile_aot) {
		mono_domain_lock (domain);
		// FIXME: How can the lookup succeed ?
		if (!g_hash_table_lookup (domain_jit_info (domain)->seq_points, cfg->method_to_register))
			g_hash_table_insert (domain_jit_info (domain)->seq_points, cfg->method_to_register, cfg->seq_point_info);
		mono_domain_unlock (domain);
	}

	g_ptr_array_free (cfg->seq_points, TRUE);
	cfg->seq_points = NULL;
}

MonoSeqPointInfo*
mono_get_seq_points (MonoDomain *domain, MonoMethod *method)
{
	MonoSeqPointInfo *seq_points;
	MonoMethod *declaring_generic_method = NULL, *shared_method = NULL;

	if (method->is_inflated) {
		declaring_generic_method = mono_method_get_declaring_generic_method (method);
		shared_method = mini_get_shared_method (method);
	}

	mono_loader_lock ();
	seq_points = (MonoSeqPointInfo *)g_hash_table_lookup (domain_jit_info (domain)->seq_points, method);
	if (!seq_points && method->is_inflated) {
		/* generic sharing + aot */
		seq_points = (MonoSeqPointInfo *)g_hash_table_lookup (domain_jit_info (domain)->seq_points, declaring_generic_method);
		if (!seq_points)
			seq_points = (MonoSeqPointInfo *)g_hash_table_lookup (domain_jit_info (domain)->seq_points, shared_method);
	}
	mono_loader_unlock ();

	return seq_points;
}

/*
 * mono_find_next_seq_point_for_native_offset:
 *
 *   Find the first sequence point after NATIVE_OFFSET.
 */
gboolean
mono_find_next_seq_point_for_native_offset (MonoDomain *domain, MonoMethod *method, gint32 native_offset, MonoSeqPointInfo **info, SeqPoint* seq_point)
{
	MonoSeqPointInfo *seq_points;

	seq_points = mono_get_seq_points (domain, method);
	if (!seq_points) {
		if (info)
			*info = NULL;
		return FALSE;
	}
	if (info)
		*info = seq_points;

	return mono_seq_point_find_next_by_native_offset (seq_points, native_offset, seq_point);
}

/*
 * mono_find_prev_seq_point_for_native_offset:
 *
 *   Find the first sequence point before NATIVE_OFFSET.
 */
gboolean
mono_find_prev_seq_point_for_native_offset (MonoDomain *domain, MonoMethod *method, gint32 native_offset, MonoSeqPointInfo **info, SeqPoint* seq_point)
{
	MonoSeqPointInfo *seq_points;

	seq_points = mono_get_seq_points (domain, method);
	if (!seq_points) {
		if (info)
			*info = NULL;
		return FALSE;
	}
	if (info)
		*info = seq_points;

	return mono_seq_point_find_prev_by_native_offset (seq_points, native_offset, seq_point);
}

/*
 * mono_find_seq_point:
 *
 *   Find the sequence point corresponding to the IL offset IL_OFFSET, which
 * should be the location of a sequence point.
 */
gboolean
mono_find_seq_point (MonoDomain *domain, MonoMethod *method, gint32 il_offset, MonoSeqPointInfo **info, SeqPoint *seq_point)
{
	MonoSeqPointInfo *seq_points;

	seq_points = mono_get_seq_points (domain, method);
	if (!seq_points) {
		if (info)
			*info = NULL;
		return FALSE;
	}
	if (info)
		*info = seq_points;

	return mono_seq_point_find_by_il_offset (seq_points, il_offset, seq_point);
}

void
mono_bb_deduplicate_op_il_seq_points (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *n, *prev;

	MONO_BB_FOR_EACH_INS_SAFE (bb, n, ins) {
		if (ins->opcode != OP_IL_SEQ_POINT)
			continue;

		prev = mono_inst_prev (ins, FILTER_NOP);

		if (!prev || ins == prev || prev->opcode != OP_IL_SEQ_POINT)
			continue;

		MONO_REMOVE_INS (bb, prev);
	};
}

void
mono_image_get_aot_seq_point_path (MonoImage *image, char **str)
{
	int size = strlen (image->name) + strlen (SEQ_POINT_AOT_EXT) + 1;
	*str = (char *)g_malloc (size);
	g_sprintf (*str, "%s%s", image->name, SEQ_POINT_AOT_EXT);
}
