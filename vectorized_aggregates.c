#include "postgres.h"
#include "cstore_fdw.h"
#include "vectorized_aggregates.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "utils/tuplesort.h"
#include "utils/datum.h"
#include "utils/typcache.h"


/*
 * AggStatePerAggData - per-aggregate working state for the Agg scan
 */
typedef struct AggStatePerAggData
{
	/*
	 * These values are set up during ExecInitAgg() and do not change
	 * thereafter:
	 */

	/* Links to Aggref expr and state nodes this working state is for */
	AggrefExprState *aggrefstate;
	Aggref	   *aggref;

	/* number of input arguments for aggregate function proper */
	int			numArguments;

	/* number of inputs including ORDER BY expressions */
	int			numInputs;

	/* Oids of transfer functions */
	Oid			transfn_oid;
	Oid			finalfn_oid;	/* may be InvalidOid */

	/*
	 * fmgr lookup data for transfer functions --- only valid when
	 * corresponding oid is not InvalidOid.  Note in particular that fn_strict
	 * flags are kept here.
	 */
	FmgrInfo	transfn;
	FmgrInfo	finalfn;

	/* Input collation derived for aggregate */
	Oid			aggCollation;

	/* number of sorting columns */
	int			numSortCols;

	/* number of sorting columns to consider in DISTINCT comparisons */
	/* (this is either zero or the same as numSortCols) */
	int			numDistinctCols;

	/* deconstructed sorting information (arrays of length numSortCols) */
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *sortCollations;
	bool	   *sortNullsFirst;

	/*
	 * fmgr lookup data for input columns' equality operators --- only
	 * set/used when aggregate has DISTINCT flag.  Note that these are in
	 * order of sort column index, not parameter index.
	 */
	FmgrInfo   *equalfns;		/* array of length numDistinctCols */

	/*
	 * initial value from pg_aggregate entry
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * We need the len and byval info for the agg's input, result, and
	 * transition data types in order to know how to copy/delete values.
	 *
	 * Note that the info for the input type is used only when handling
	 * DISTINCT aggs with just one argument, so there is only one input type.
	 */
	int16		inputtypeLen,
				resulttypeLen,
				transtypeLen;
	bool		inputtypeByVal,
				resulttypeByVal,
				transtypeByVal;

	/*
	 * Stuff for evaluation of inputs.  We used to just use ExecEvalExpr, but
	 * with the addition of ORDER BY we now need at least a slot for passing
	 * data to the sort object, which requires a tupledesc, so we might as
	 * well go whole hog and use ExecProject too.
	 */
	TupleDesc	evaldesc;		/* descriptor of input tuples */
	ProjectionInfo *evalproj;	/* projection machinery */

	/*
	 * Slots for holding the evaluated input arguments.  These are set up
	 * during ExecInitAgg() and then used for each input row.
	 */
	TupleTableSlot *evalslot;	/* current input tuple */
	TupleTableSlot *uniqslot;	/* used for multi-column DISTINCT */

	/*
	 * These values are working state that is initialized at the start of an
	 * input tuple group and updated for each input tuple.
	 *
	 * For a simple (non DISTINCT/ORDER BY) aggregate, we just feed the input
	 * values straight to the transition function.  If it's DISTINCT or
	 * requires ORDER BY, we pass the input values into a Tuplesort object;
	 * then at completion of the input tuple group, we scan the sorted values,
	 * eliminate duplicates if needed, and run the transition function on the
	 * rest.
	 */

	Tuplesortstate *sortstate;	/* sort object, if DISTINCT or ORDER BY */
}	AggStatePerAggData;


/*
 * AggStatePerGroupData - per-aggregate-per-group working state
 *
 * These values are working state that is initialized at the start of
 * an input tuple group and updated for each input tuple.
 *
 * In AGG_PLAIN and AGG_SORTED modes, we have a single array of these
 * structs (pointed to by aggstate->pergroup); we re-use the array for
 * each input group, if it's AGG_SORTED mode.  In AGG_HASHED mode, the
 * hash table contains an array of these structs for each tuple group.
 *
 * Logically, the sortstate field belongs in this struct, but we do not
 * keep it here for space reasons: we don't support DISTINCT aggregates
 * in AGG_HASHED mode, so there's no reason to use up a pointer field
 * in every entry of the hashtable.
 */
typedef struct AggStatePerGroupData
{
	Datum		transValue;		/* current transition value */
	bool		transValueIsNull;

	bool		noTransValue;	/* true if transValue not set yet */

	/*
	 * Note: noTransValue initially has the same value as transValueIsNull,
	 * and if true both are cleared to false at the same time.  They are not
	 * the same though: if transfn later returns a NULL, we want to keep that
	 * NULL and not auto-replace it with a later input value. Only the first
	 * non-NULL input will be auto-substituted.
	 */
} AggStatePerGroupData;


/*
 * To implement hashed aggregation, we need a hashtable that stores a
 * representative tuple and an array of AggStatePerGroup structs for each
 * distinct set of GROUP BY column values.  We compute the hash key from
 * the GROUP BY columns.
 */
typedef struct AggHashEntryData *AggHashEntry;

typedef struct AggHashEntryData
{
	TupleHashEntryData shared;	/* common header for hash table entries */
	/* per-aggregate transition status array - must be last! */
	AggStatePerGroupData pergroup[1];	/* VARIABLE LENGTH ARRAY */
}	AggHashEntryData;	/* VARIABLE LENGTH STRUCT */


typedef struct AggregationHashEntry
{
	Datum key;
	Datum value;
} AggregationHashEntry;


typedef enum VectorizedAggType 
{ 
	VAT_GROUP_BY_COUNT,
	VAT_GROUP_BY_SUM
} VectorizedAggType;


static void initialize_aggregates(AggState *aggstate,
								  AggStatePerAgg peragg,
								  AggStatePerGroup pergroup);
static void process_ordered_aggregate_single(AggState *aggstate,
											 AggStatePerAgg peraggstate,
											 AggStatePerGroup pergroupstate);
static void process_ordered_aggregate_multi(AggState *aggstate,
											AggStatePerAgg peraggstate,
											AggStatePerGroup pergroupstate);
static void advance_transition_function(AggState *aggstate,
										AggStatePerAgg peraggstate,
										AggStatePerGroup pergroupstate,
										FunctionCallInfoData *fcinfo);
static void finalize_aggregate(AggState *aggstate,
							   AggStatePerAgg peraggstate,
							   AggStatePerGroup pergroupstate,
							   Datum *resultVal, 
							   bool *resultIsNull);
static void ExecutePlanVectorized(EState *estate, 
								  PlanState *planstate,
								  CmdType operation,
								  bool sendTuples,
								  long numberTuples,
								  ScanDirection direction,
								  DestReceiver *dest);
static void advance_aggregates_vectorized(AggState *aggstate, 
										  AggStatePerGroup pergroup);
static void advance_transition_function_vectorized(AggState *aggstate,
												   AggStatePerAgg peraggstate,
												   AggStatePerGroup pergroupstate,
												   FunctionCallInfoData *fcinfo);
static TupleTableSlot *agg_retrieve_hash_vectorized(AggState *aggstate);
static TupleTableSlot *agg_retrieve_direct_vectorized(AggState *aggstate);
int CompareHashKeyStrings(const void *key1, const void *key2, Size keysize);


/* is it a group by query */
static Var *CurrentKeyVar = NULL;
static Var *CurrentValueVar = NULL;
static HTAB *CurrentAggregationHash = NULL;
static HASH_SEQ_STATUS CurrentHashSeqStatus;
static VectorizedAggType CurrentVectorizedAggType = VAT_GROUP_BY_COUNT;

static FmgrInfo *CurrentHashFunction = NULL;
static FmgrInfo *CurrentEqualityFunction = NULL;

static uint32 VectorizedHashTableHash(const void *key, Size keysize);
static int VectorizedHashTableMatch(const void *key1, const void *key2, Size keySize);


/*
 * Initialize all aggregates for a new group of input values.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
initialize_aggregates(AggState *aggstate,
					  AggStatePerAgg peragg,
					  AggStatePerGroup pergroup)
{
	int			aggno;

	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &peragg[aggno];
		AggStatePerGroup pergroupstate = &pergroup[aggno];

		/*
		 * Start a fresh sort operation for each DISTINCT/ORDER BY aggregate.
		 */
		if (peraggstate->numSortCols > 0)
		{
			/*
			 * In case of rescan, maybe there could be an uncompleted sort
			 * operation?  Clean it up if so.
			 */
			if (peraggstate->sortstate)
				tuplesort_end(peraggstate->sortstate);

			/*
			 * We use a plain Datum sorter when there's a single input column;
			 * otherwise sort the full tuple.  (See comments for
			 * process_ordered_aggregate_single.)
			 */
			peraggstate->sortstate =
				(peraggstate->numInputs == 1) ?
				tuplesort_begin_datum(peraggstate->evaldesc->attrs[0]->atttypid,
									  peraggstate->sortOperators[0],
									  peraggstate->sortCollations[0],
									  peraggstate->sortNullsFirst[0],
									  work_mem, false) :
				tuplesort_begin_heap(peraggstate->evaldesc,
									 peraggstate->numSortCols,
									 peraggstate->sortColIdx,
									 peraggstate->sortOperators,
									 peraggstate->sortCollations,
									 peraggstate->sortNullsFirst,
									 work_mem, false);
		}

		/*
		 * (Re)set transValue to the initial value.
		 *
		 * Note that when the initial value is pass-by-ref, we must copy it
		 * (into the aggcontext) since we will pfree the transValue later.
		 */
		if (peraggstate->initValueIsNull)
			pergroupstate->transValue = peraggstate->initValue;
		else
		{
			MemoryContext oldContext;

			oldContext = MemoryContextSwitchTo(aggstate->aggcontext);
			pergroupstate->transValue = datumCopy(peraggstate->initValue,
												  peraggstate->transtypeByVal,
												  peraggstate->transtypeLen);
			MemoryContextSwitchTo(oldContext);
		}
		pergroupstate->transValueIsNull = peraggstate->initValueIsNull;

		/*
		 * If the initial value for the transition state doesn't exist in the
		 * pg_aggregate table then we will let the first non-NULL value
		 * returned from the outer procNode become the initial value. (This is
		 * useful for aggregates like max() and min().) The noTransValue flag
		 * signals that we still need to do this.
		 */
		pergroupstate->noTransValue = peraggstate->initValueIsNull;
	}
}


/*
 * Run the transition function for a DISTINCT or ORDER BY aggregate
 * with only one input.  This is called after we have completed
 * entering all the input values into the sort object.  We complete the
 * sort, read out the values in sorted order, and run the transition
 * function on each value (applying DISTINCT if appropriate).
 *
 * Note that the strictness of the transition function was checked when
 * entering the values into the sort, so we don't check it again here;
 * we just apply standard SQL DISTINCT logic.
 *
 * The one-input case is handled separately from the multi-input case
 * for performance reasons: for single by-value inputs, such as the
 * common case of count(distinct id), the tuplesort_getdatum code path
 * is around 300% faster.  (The speedup for by-reference types is less
 * but still noticeable.)
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
process_ordered_aggregate_single(AggState *aggstate,
								 AggStatePerAgg peraggstate,
								 AggStatePerGroup pergroupstate)
{
	Datum		oldVal = (Datum) 0;
	bool		oldIsNull = true;
	bool		haveOldVal = false;
	MemoryContext workcontext = aggstate->tmpcontext->ecxt_per_tuple_memory;
	MemoryContext oldContext;
	bool		isDistinct = (peraggstate->numDistinctCols > 0);
	Datum	   *newVal;
	bool	   *isNull;
	FunctionCallInfoData fcinfo;

	Assert(peraggstate->numDistinctCols < 2);

	tuplesort_performsort(peraggstate->sortstate);

	/* Load the column into argument 1 (arg 0 will be transition value) */
	newVal = fcinfo.arg + 1;
	isNull = fcinfo.argnull + 1;

	/*
	 * Note: if input type is pass-by-ref, the datums returned by the sort are
	 * freshly palloc'd in the per-query context, so we must be careful to
	 * pfree them when they are no longer needed.
	 */

	while (tuplesort_getdatum(peraggstate->sortstate, true,
							  newVal, isNull))
	{
		/*
		 * Clear and select the working context for evaluation of the equality
		 * function and transition function.
		 */
		MemoryContextReset(workcontext);
		oldContext = MemoryContextSwitchTo(workcontext);

		/*
		 * If DISTINCT mode, and not distinct from prior, skip it.
		 *
		 * Note: we assume equality functions don't care about collation.
		 */
		if (isDistinct &&
			haveOldVal &&
			((oldIsNull && *isNull) ||
			 (!oldIsNull && !*isNull &&
			  DatumGetBool(FunctionCall2(&peraggstate->equalfns[0],
										 oldVal, *newVal)))))
		{
			/* equal to prior, so forget this one */
			if (!peraggstate->inputtypeByVal && !*isNull)
				pfree(DatumGetPointer(*newVal));
		}
		else
		{
			advance_transition_function(aggstate, peraggstate, pergroupstate,
										&fcinfo);
			/* forget the old value, if any */
			if (!oldIsNull && !peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(oldVal));
			/* and remember the new one for subsequent equality checks */
			oldVal = *newVal;
			oldIsNull = *isNull;
			haveOldVal = true;
		}

		MemoryContextSwitchTo(oldContext);
	}

	if (!oldIsNull && !peraggstate->inputtypeByVal)
		pfree(DatumGetPointer(oldVal));

	tuplesort_end(peraggstate->sortstate);
	peraggstate->sortstate = NULL;
}


/*
 * Run the transition function for a DISTINCT or ORDER BY aggregate
 * with more than one input.  This is called after we have completed
 * entering all the input values into the sort object.  We complete the
 * sort, read out the values in sorted order, and run the transition
 * function on each value (applying DISTINCT if appropriate).
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
process_ordered_aggregate_multi(AggState *aggstate,
								AggStatePerAgg peraggstate,
								AggStatePerGroup pergroupstate)
{
	MemoryContext workcontext = aggstate->tmpcontext->ecxt_per_tuple_memory;
	FunctionCallInfoData fcinfo;
	TupleTableSlot *slot1 = peraggstate->evalslot;
	TupleTableSlot *slot2 = peraggstate->uniqslot;
	int			numArguments = peraggstate->numArguments;
	int			numDistinctCols = peraggstate->numDistinctCols;
	bool		haveOldValue = false;
	int			i;

	tuplesort_performsort(peraggstate->sortstate);

	ExecClearTuple(slot1);
	if (slot2)
		ExecClearTuple(slot2);

	while (tuplesort_gettupleslot(peraggstate->sortstate, true, slot1))
	{
		/*
		 * Extract the first numArguments as datums to pass to the transfn.
		 * (This will help execTuplesMatch too, so do it immediately.)
		 */
		slot_getsomeattrs(slot1, numArguments);

		if (numDistinctCols == 0 ||
			!haveOldValue ||
			!execTuplesMatch(slot1, slot2,
							 numDistinctCols,
							 peraggstate->sortColIdx,
							 peraggstate->equalfns,
							 workcontext))
		{
			/* Load values into fcinfo */
			/* Start from 1, since the 0th arg will be the transition value */
			for (i = 0; i < numArguments; i++)
			{
				fcinfo.arg[i + 1] = slot1->tts_values[i];
				fcinfo.argnull[i + 1] = slot1->tts_isnull[i];
			}

			advance_transition_function(aggstate, peraggstate, pergroupstate,
										&fcinfo);

			if (numDistinctCols > 0)
			{
				/* swap the slot pointers to retain the current tuple */
				TupleTableSlot *tmpslot = slot2;

				slot2 = slot1;
				slot1 = tmpslot;
				haveOldValue = true;
			}
		}

		/* Reset context each time, unless execTuplesMatch did it for us */
		if (numDistinctCols == 0)
			MemoryContextReset(workcontext);

		ExecClearTuple(slot1);
	}

	if (slot2)
		ExecClearTuple(slot2);

	tuplesort_end(peraggstate->sortstate);
	peraggstate->sortstate = NULL;
}


/*
 * Given new input value(s), advance the transition function of an aggregate.
 *
 * The new values (and null flags) have been preloaded into argument positions
 * 1 and up in fcinfo, so that we needn't copy them again to pass to the
 * transition function.  No other fields of fcinfo are assumed valid.
 *
 * It doesn't matter which memory context this is called in.
 */
static void
advance_transition_function(AggState *aggstate,
							AggStatePerAgg peraggstate,
							AggStatePerGroup pergroupstate,
							FunctionCallInfoData *fcinfo)
{
	int			numArguments = peraggstate->numArguments;
	MemoryContext oldContext;
	Datum		newVal;
	int			i;

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->argnull[i])
				return;
		}
		if (pergroupstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into aggcontext if it is pass-by-ref. We
			 * do not need to pfree the old transValue, since it's NULL.
			 */
			oldContext = MemoryContextSwitchTo(aggstate->aggcontext);
			pergroupstate->transValue = datumCopy(fcinfo->arg[1],
												  peraggstate->transtypeByVal,
												  peraggstate->transtypeLen);
			pergroupstate->transValueIsNull = false;
			pergroupstate->noTransValue = false;
			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (pergroupstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			return;
		}
	}

	/* We run the transition functions in per-input-tuple memory context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	/*
	 * OK to call the transition function
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn),
							 numArguments + 1,
							 peraggstate->aggCollation,
							 (void *) aggstate, NULL);
	fcinfo->arg[0] = pergroupstate->transValue;
	fcinfo->argnull[0] = pergroupstate->transValueIsNull;

	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.  But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(aggstate->aggcontext);
			newVal = datumCopy(newVal,
							   peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		if (!pergroupstate->transValueIsNull)
			pfree(DatumGetPointer(pergroupstate->transValue));
	}

	pergroupstate->transValue = newVal;
	pergroupstate->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}


/*
 * Compute the final value of one aggregate.
 *
 * The finalfunction will be run, and the result delivered, in the
 * output-tuple context; caller's CurrentMemoryContext does not matter.
 */
static void
finalize_aggregate(AggState *aggstate,
				   AggStatePerAgg peraggstate,
				   AggStatePerGroup pergroupstate,
				   Datum *resultVal, bool *resultIsNull)
{
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(aggstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		FunctionCallInfoData fcinfo;

		InitFunctionCallInfoData(fcinfo, &(peraggstate->finalfn), 1,
								 peraggstate->aggCollation,
								 (void *) aggstate, NULL);
		fcinfo.arg[0] = pergroupstate->transValue;
		fcinfo.argnull[0] = pergroupstate->transValueIsNull;
		if (fcinfo.flinfo->fn_strict && pergroupstate->transValueIsNull)
		{
			/* don't call a strict function with NULL inputs */
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			*resultVal = FunctionCallInvoke(&fcinfo);
			*resultIsNull = fcinfo.isnull;
		}
	}
	else
	{
		*resultVal = pergroupstate->transValue;
		*resultIsNull = pergroupstate->transValueIsNull;
	}

	/*
	 * If result is pass-by-ref, make sure it is in the right context.
	 */
	if (!peraggstate->resulttypeByVal && !*resultIsNull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*resultVal)))
		*resultVal = datumCopy(*resultVal,
							   peraggstate->resulttypeByVal,
							   peraggstate->resulttypeLen);

	MemoryContextSwitchTo(oldContext);
}


/*
 * If the query is a supported one, use the vectorized execution,
 * if not use the standard one.
 */
void
vectorized_ExecutorRun(QueryDesc *queryDesc,
					   ScanDirection direction, 
					   long count)
{
	PlannedStmt *plannedstmt = queryDesc->plannedstmt;
	struct Plan *planTree = plannedstmt->planTree;
	bool groupByAggregate = false;
	bool vectorizedExecution = false;

	if ((planTree != NULL) && (planTree->type == T_Agg) && 
	(	planTree->lefttree != NULL) && (planTree->lefttree->type == T_ForeignScan))
	{
		vectorizedExecution = true;
	}

	if (vectorizedExecution && (((Agg *) planTree)->aggstrategy == AGG_HASHED))
	{
		groupByAggregate = true;
	}

	if (groupByAggregate)
	{
		TargetEntry *targetEntry1 = (TargetEntry *) linitial(planTree->targetlist);
		TargetEntry *targetEntry2 = (TargetEntry *) lsecond(planTree->targetlist);
		Expr *expr1 = targetEntry1->expr;
		Expr *expr2 = targetEntry2->expr;
		if (expr1->type == T_Var && expr2->type == T_Aggref)
		{
			Aggref *aggref = (Aggref *) expr2;
			Oid aggregateFunctionId = aggref->aggfnoid;
			List *aggregateArgumentList = aggref->args;
			int32 aggregateArgumentCount = list_length(aggregateArgumentList);
			TargetEntry *targetEntry = NULL;

			/* look up the function name */
			char *aggregateFunctionName = get_func_name(aggregateFunctionId);
			if (aggregateFunctionName == NULL)
			{
				ereport(ERROR, (errmsg("cache lookup failed for function %u",
									   aggregateFunctionId)));
			}

			if (strncmp(aggregateFunctionName, "count", NAMEDATALEN) == 0)
			{
				CurrentVectorizedAggType = VAT_GROUP_BY_COUNT;
			}
			else if (strncmp(aggregateFunctionName, "sum", NAMEDATALEN) == 0)
			{
				CurrentVectorizedAggType = VAT_GROUP_BY_SUM;
			}
			else
			{
				ereport(ERROR, (errmsg("vectorization unsupported for \"%s\" "
									   "group by", aggregateFunctionName)));
			}

			/*
			 * We also check for the argument count here. count(column) works,
			 * but count(*) returns 0 arguments. To make 0 arguments work, we
			 * need to rework the if / else nestings in vectorized group bys.
			 * For now, we only support count(column) group bys, and error out
			 * on count(*) group bys.
			 */
			if (aggregateArgumentCount != 1)
			{
				ereport(ERROR, (errmsg("vectorized group by aggregate received "
									   "%d arguments, but supports only 1",
									   aggregateArgumentCount)));
			}

			/* set global key and value columns */
			CurrentKeyVar = (Var *) expr1;

			targetEntry = (TargetEntry *) linitial(aggregateArgumentList);
			CurrentValueVar = (Var *) targetEntry->expr;
		}
	}

	if (vectorizedExecution)
	{
		EState *estate = NULL;
		CmdType	operation = CMD_UNKNOWN;
		DestReceiver *dest = NULL;
		bool sendTuples = false;
		MemoryContext oldcontext = NULL;

		/* sanity checks */
		Assert(queryDesc != NULL);

		estate = queryDesc->estate;

		Assert(estate != NULL);
		Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

		/*
		 * Switch into per-query memory context
		 */
		oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

		/* Allow instrumentation of Executor overall runtime */
		if (queryDesc->totaltime)
		{
			InstrStartNode(queryDesc->totaltime);
		}

		/*
		 * extract information from the query descriptor and the query feature.
		 */
		operation = queryDesc->operation;
		dest = queryDesc->dest;

		/*
		 * startup tuple receiver, if we will be emitting tuples
		 */
		estate->es_processed = 0;
		estate->es_lastoid = InvalidOid;

		if ((operation == CMD_SELECT) || queryDesc->plannedstmt->hasReturning)
		{
			sendTuples = true;
		}

		if (sendTuples)
		{
			(*dest->rStartup) (dest, operation, queryDesc->tupDesc);
		}
		
		/*
		 * run plan
		 */
		if (!ScanDirectionIsNoMovement(direction))
		{
			ExecutePlanVectorized(estate, queryDesc->planstate, operation,
								  sendTuples, count, direction, dest);
		}

		/*
		 * shutdown tuple receiver, if we started it
		 */
		if (sendTuples)
		{
			(*dest->rShutdown) (dest);
		}

		if (queryDesc->totaltime)
		{
			InstrStopNode(queryDesc->totaltime, estate->es_processed);
		}

		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
  		standard_ExecutorRun(queryDesc, direction, count);
	}

}


/*
 * Same as ExecutePlan. Instead of ExecProcNode we call ExecProcNodeVectorized.
 */
static void
ExecutePlanVectorized(EState *estate, PlanState *planstate, CmdType operation,
					  bool sendTuples, long numberTuples, ScanDirection direction,
					  DestReceiver *dest)
{
	TupleTableSlot *slot = NULL;
	long current_tuple_count = 0;

	/*
	 * initialize local variables
	 */
	current_tuple_count = 0;

	/*
	 * Set the direction.
	 */
	estate->es_direction = direction;

	/*
	 * Loop until we've processed the proper number of tuples from the plan.
	 */
	for (;;)
	{
		/* Reset the per-output-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/*
		 * Execute the plan and obtain a tuple
		 */
		slot = ExecProcNodeVectorized(planstate);

		/*
		 * if the tuple is null, then we assume there is nothing more to
		 * process so we just end the loop...
		 */
		if (TupIsNull(slot))
			break;

		/*
		 * If we have a junk filter, then project a new tuple with the junk
		 * removed.
		 *
		 * Store this new "clean" tuple in the junkfilter's resultSlot.
		 * (Formerly, we stored it back over the "dirty" tuple, which is WRONG
		 * because that tuple slot has the wrong descriptor.)
		 */
		if (estate->es_junkFilter != NULL)
			slot = ExecFilterJunk(estate->es_junkFilter, slot);

		/*
		 * If we are supposed to send the tuple somewhere, do so. (In
		 * practice, this is probably always the case at this point.)
		 */
		if (sendTuples)
			(*dest->receiveSlot) (slot, dest);

		/*
		 * Count tuples processed, if this is a SELECT.  (For other operation
		 * types, the ModifyTable plan node must count the appropriate
		 * events.)
		 */
		if (operation == CMD_SELECT)
			(estate->es_processed)++;

		/*
		 * check our tuple count.. if we've processed the proper number then
		 * quit, else loop again and process more tuples.  Zero numberTuples
		 * means no limit.
		 */
		current_tuple_count++;
		if (numberTuples && numberTuples == current_tuple_count)
			break;
	}
}


/*
 * Similar to ExecProcNode, but supports only T_AggState. Instead of ExecAgg
 * we call ExecAggVectorized.
 */
TupleTableSlot *
ExecProcNodeVectorized(PlanState *node)
{
	TupleTableSlot *result = NULL;

	CHECK_FOR_INTERRUPTS();

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node);		/* let ReScan handle this */

	if (node->instrument)
		InstrStartNode(node->instrument);

	switch (nodeTag(node))
	{
		case T_AggState:
			result = ExecAggVectorized((AggState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;
			break;
	}

	if (node->instrument)
		InstrStopNode(node->instrument, TupIsNull(result) ? 0.0 : 1.0);

	return result;
}


/*
 * Similar to ExecAgg, but supports only plain aggregates. Instead of
 * agg_retrieve_direct, we call agg_retrieve_direct_vectorized.
 */
TupleTableSlot *
ExecAggVectorized(AggState *node)
{
	/*
	 * Check to see if we're still projecting out tuples from a previous agg
	 * tuple (because there is a function-returning-set in the projection
	 * expressions).  If so, try to project another one.
	 */
	if (node->ss.ps.ps_TupFromTlist)
	{
		TupleTableSlot *result;
		ExprDoneCond isDone;

		result = ExecProject(node->ss.ps.ps_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return result;
		/* Done with that source tuple... */
		node->ss.ps.ps_TupFromTlist = false;
	}

	/*
	 * Exit if nothing left to do.	(We must do the ps_TupFromTlist check
	 * first, because in some cases agg_done gets set before we emit the final
	 * aggregate tuple, and we have to finish running SRFs for it.)
	 */
	if (node->agg_done)
		return NULL;

	/* Dispatch based on strategy */
	if (((Agg *) node->ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		return agg_retrieve_hash_vectorized(node);
	}
	else
	{
		return agg_retrieve_direct_vectorized(node);
	}
}


/*
 * agg_retrieve_hash_vectorized accumulates row values using a hash table. This
 * function executes in two phases. First, it reads input and builds the hash
 * table. Second, the function retrieves the aggregated tuples from the hash
 * table and returns them. In that sense, the function merges the logic for
 * agg_fill_hash_table() and agg_retrieve_hash_table() into a single function.
 */
static TupleTableSlot *
agg_retrieve_hash_vectorized(AggState *aggstate)
{
	TupleTableSlot *resultSlot = aggstate->ss.ps.ps_ProjInfo->pi_slot;
	AggregationHashEntry *nextHashEntry = NULL;

	if (!(aggstate->table_filled))
	{
		ExprContext *tmpcontext = NULL;
		TupleTableSlot *outerslot = NULL;

		PlanState  *outerPlan = outerPlanState(aggstate);
		ForeignScanState *foreignNode = (ForeignScanState *) outerPlan;
		TableReadState *readState = (TableReadState *) foreignNode->fdw_state;

		AttrNumber keyVarIndex = CurrentKeyVar->varattno - 1;
		AttrNumber valueVarIndex = CurrentValueVar->varattno - 1;

		HASHCTL info;
		int hashFlags = 0;
		TypeCacheEntry *keyTypeCacheEntry = NULL;
		TypeCacheEntry *valueTypeCacheEntry = NULL;

		keyTypeCacheEntry = lookup_type_cache(CurrentKeyVar->vartype,
											  (TYPECACHE_HASH_PROC_FINFO |
											   TYPECACHE_EQ_OPR_FINFO));

		valueTypeCacheEntry = lookup_type_cache(CurrentValueVar->vartype, 0);

		CurrentHashFunction = &(keyTypeCacheEntry->hash_proc_finfo);
		CurrentEqualityFunction = &(keyTypeCacheEntry->eq_opr_finfo);

		info.keysize = sizeof(Datum);
		info.entrysize = sizeof(AggregationHashEntry);
		info.hash = VectorizedHashTableHash;
		info.match = VectorizedHashTableMatch;

		hashFlags = HASH_ELEM | HASH_FUNCTION | HASH_COMPARE;	

		CurrentAggregationHash = hash_create("Aggregation Hash", 1024,
											 &info, hashFlags);

		/* tmpcontext is the per-input-tuple expression context */
		tmpcontext = aggstate->tmpcontext;

		/*
		 * Process each outer-plan tuple, and then fetch the next one, until we
		 * exhaust the outer plan.
		 */
		for (;;)
		{
			uint32 i = 0;
			uint32 rowCount = 0;
			uint64 blockRowCount = 0;
			StripeData *stripeData = NULL;

			ColumnData *keyColumnData = NULL;
			ColumnData *valueColumnData = NULL;

			readState->stripeData = NULL;
			outerslot =	foreignNode->fdwroutine->IterateForeignScan(foreignNode);

			if (TupIsNull(outerslot))
				break;

			/* set up for advance_aggregates call */
			tmpcontext->ecxt_outertuple = outerslot;

			stripeData = readState->stripeData;
			rowCount = stripeData->rowCount;
			blockRowCount = readState->tableFooter->blockRowCount;
			keyColumnData = stripeData->columnDataArray[keyVarIndex];
			valueColumnData = stripeData->columnDataArray[valueVarIndex];

			for (i = 0; i < rowCount; i++)
			{
				AggregationHashEntry *aggregationHashEntry = NULL;
				bool handleFound = false;

				int blockIndex = i / blockRowCount;
				int rowIndex = i % blockRowCount;

				ColumnBlockData *keyBlockData = keyColumnData->blockDataArray[blockIndex];
				ColumnBlockData *valueBlockData =
					valueColumnData->blockDataArray[blockIndex];

				Datum key = keyBlockData->valueArray[rowIndex];
				Datum value = valueBlockData->valueArray[rowIndex];

				bool exists = valueBlockData->existsArray[rowIndex];
				if (!exists)
				{
					/* Reset per-input-tuple context after each tuple */
					ResetExprContext(tmpcontext);
					continue;
				}

				aggregationHashEntry =
					(AggregationHashEntry *) hash_search(CurrentAggregationHash, &key,
														 HASH_ENTER, &handleFound);

				if (CurrentVectorizedAggType == VAT_GROUP_BY_SUM)
				{
					if (handleFound)
					{
						Oid aggregateColumnType = CurrentValueVar->vartype;
						if (aggregateColumnType == FLOAT8OID)
						{
							aggregationHashEntry->value =
								DirectFunctionCall2(float8pl,
													aggregationHashEntry->value, 
													value);
						}
						else if (aggregateColumnType == INT4OID)
						{
							aggregationHashEntry->value =
								aggregationHashEntry->value + value;
						}
						else
						{
							ereport(ERROR, (errmsg("unsupported column type: %d "
												   "for vectorized sum() group by",
												   aggregateColumnType)));
						}
					}
					else
					{
						aggregationHashEntry->key = 
							datumCopy(key, keyTypeCacheEntry->typbyval,
									  keyTypeCacheEntry->typlen);

						aggregationHashEntry->value = 
							datumCopy(value, valueTypeCacheEntry->typbyval,
									  valueTypeCacheEntry->typlen);
					}
				}
				else if (CurrentVectorizedAggType == VAT_GROUP_BY_COUNT)
				{
					if (handleFound)
					{
						aggregationHashEntry->value = aggregationHashEntry->value + 1;
					}
					else
					{
						aggregationHashEntry->key = 
							datumCopy(key, keyTypeCacheEntry->typbyval,
									  keyTypeCacheEntry->typlen);

						aggregationHashEntry->value = 
							datumCopy(Int64GetDatum(1), valueTypeCacheEntry->typbyval,
									  valueTypeCacheEntry->typlen);
					}
				}

				/* Reset per-input-tuple context after each tuple */
				ResetExprContext(tmpcontext);
			}
		}

		aggstate->table_filled = true;
		hash_seq_init(&CurrentHashSeqStatus, CurrentAggregationHash);
	}

	ExecClearTuple(resultSlot);

	nextHashEntry =
		(AggregationHashEntry *) hash_seq_search(&CurrentHashSeqStatus);

	if (nextHashEntry != NULL)
	{
		TupleDesc tupleDescriptor = resultSlot->tts_tupleDescriptor;
		uint32 columnCount = tupleDescriptor->natts;
		Datum *columnValues = resultSlot->tts_values;
		bool *columnNulls = resultSlot->tts_isnull;

		memset(columnValues, 0, columnCount * sizeof(Datum));
		memset(columnNulls, true, columnCount * sizeof(bool));

		if ((CurrentVectorizedAggType == VAT_GROUP_BY_SUM) ||
			(CurrentVectorizedAggType == VAT_GROUP_BY_COUNT))
		{
			columnValues[0] = nextHashEntry->key;
			columnValues[1] = nextHashEntry->value;
			columnNulls[0] = false;
			columnNulls[1] = false;
		}

		ExecStoreVirtualTuple(resultSlot);
	}
	else
	{
		hash_destroy(CurrentAggregationHash);
		CurrentAggregationHash = NULL;
	}

	return resultSlot;
}


/* 
 * Similar to agg_retrieve_direct. But takes data stripe by stripe instead of
 * row by row. We force cstore to return a new stripe in each iteration by
 * setting previous stripe to NULL. Instead of advance_aggregates, we call
 * advance_aggregates_vectorized.
 */
static TupleTableSlot *
agg_retrieve_direct_vectorized(AggState *aggstate)
{
	PlanState  *outerPlan;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	TupleTableSlot *outerslot;
	TupleTableSlot *firstSlot;
	int			aggno;
	ForeignScanState *foreignNode;
	TableReadState *readState;
	TupleTableSlot *result;
	ExprDoneCond isDone;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(aggstate);

	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->ss.ps.ps_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;

	/* tmpcontext is the per-input-tuple expression context */
	tmpcontext = aggstate->tmpcontext;
	peragg = aggstate->peragg;
	pergroup = aggstate->pergroup;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	foreignNode = (ForeignScanState *) outerPlan;
	readState = (TableReadState *) foreignNode->fdw_state;
	readState->stripeData = NULL;

	outerslot = foreignNode->fdwroutine->IterateForeignScan(foreignNode);
	if (!TupIsNull(outerslot))
	{
		/*
		 * Make a copy of the first input tuple; we will use this for
		 * comparisons (in group mode) and for projection.
		 */
		aggstate->grp_firstTuple = ExecCopySlotTuple(outerslot);
	}
	else
	{
		aggstate->agg_done = true;
	}

	/*
	 * Clear the per-output-tuple context for each group, as well as
	 * aggcontext (which contains any pass-by-ref transvalues of the old
	 * group).	We also clear any child contexts of the aggcontext; some
	 * aggregate functions store working state in such contexts.
	 */
	ResetExprContext(econtext);

	MemoryContextResetAndDeleteChildren(aggstate->aggcontext);

	/*
	 * Initialize working state for a new input tuple group
	 */
	initialize_aggregates(aggstate, peragg, pergroup);

	if (aggstate->grp_firstTuple != NULL)
	{
		/*
		 * Store the copied first input tuple in the tuple table slot
		 * reserved for it.  The tuple will be deleted when it is cleared
		 * from the slot.
		 */
		ExecStoreTuple(aggstate->grp_firstTuple, firstSlot, InvalidBuffer, true);
		aggstate->grp_firstTuple = NULL;	/* don't keep two pointers */

		/* set up for first advance_aggregates call */
		tmpcontext->ecxt_outertuple = firstSlot;

		/*
		 * Process each outer-plan tuple, and then fetch the next one,
		 * until we exhaust the outer plan or cross a group boundary.
		 */
		for (;;)
		{
			advance_aggregates_vectorized(aggstate, pergroup);

			/* Reset per-input-tuple context after each tuple */
			ResetExprContext(tmpcontext);

			readState->stripeData = NULL;
			outerslot = foreignNode->fdwroutine->IterateForeignScan(foreignNode);
			if (TupIsNull(outerslot))
			{
				/* no more outer-plan tuples available */
				aggstate->agg_done = true;
				break;
			}

			/* set up for next advance_aggregates call */
			tmpcontext->ecxt_outertuple = outerslot;
		}
	}

	/*
	 * Done scanning input tuple group. Finalize each aggregate
	 * calculation, and stash results in the per-output-tuple context.
	 */
	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &peragg[aggno];
		AggStatePerGroup pergroupstate = &pergroup[aggno];

		if (peraggstate->numSortCols > 0)
		{
			if (peraggstate->numInputs == 1)
			{
				process_ordered_aggregate_single(aggstate, peraggstate, pergroupstate);
			}
			else
			{
				process_ordered_aggregate_multi(aggstate, peraggstate, pergroupstate);
			}
		}

		finalize_aggregate(aggstate, peraggstate, pergroupstate, 
						   &aggvalues[aggno], &aggnulls[aggno]);
	}

	/*
	 * Use the representative input tuple for any references to
	 * non-aggregated input columns in the qual and tlist.	(If we are not
	 * grouping, and there are no input rows at all, we will come here
	 * with an empty firstSlot ... but if not grouping, there can't be any
	 * references to non-aggregated input columns, so no problem.)
	 */
	econtext->ecxt_outertuple = firstSlot;

	/*
	 * Form and return a projection tuple using the aggregate results
	 * and the representative input tuple.
	 */

	result = ExecProject(aggstate->ss.ps.ps_ProjInfo, &isDone);

	aggstate->ss.ps.ps_TupFromTlist = false;
	if (isDone == ExprMultipleResult)
	{
		aggstate->ss.ps.ps_TupFromTlist = true;
	}

	return result;
}


/*
 * Similar to advance_aggregates. Instead of passing a cell, we pass a stripe to
 * transfunction. (With some meta information: row count, block count in a row)
 * Instead of advance_transition_function, we call
 * advance_transition_function_vectorized.
 */
static void
advance_aggregates_vectorized(AggState *aggstate, AggStatePerGroup pergroup)
{
	PlanState  *outerPlan = outerPlanState(aggstate);
	ForeignScanState *foreignNode = (ForeignScanState *) outerPlan;
	TableReadState *readState = (TableReadState *) foreignNode->fdw_state;
	StripeData *stripeData = readState->stripeData;
	uint32 rowCount = stripeData->rowCount;
	uint64 blockRowCount = readState->tableFooter->blockRowCount;

	int aggno = 0;
	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &aggstate->peragg[aggno];
		AggStatePerGroup pergroupstate = &pergroup[aggno];
		int32 argumentCount = peraggstate->numArguments + 1;
		ColumnData *columnData = NULL;
		char *transitionFuncName = NULL;
		char vectorTransitionFuncName[NAMEDATALEN];
		List *qualVectorTransitionFuncName = NIL;
		FuncCandidateList vectorTransitionFuncList = NULL;
		FunctionCallInfoData fcinfo;

		/* simple check to handle count(*) */
		int simpleColumnCount =  peraggstate->evalproj->pi_numSimpleVars;
		if (simpleColumnCount >= 1)
		{
			int columnIndex = peraggstate->evalproj->pi_varNumbers[0]-1;
			columnData = stripeData->columnDataArray[columnIndex];
		}

		/*
		 * If the user typed sum(), count(), or avg() instead of the vectorized
		 * aggregate names, manually map to the vectorized version here. This is
		 * merely syntactic sugar. Note that we rely on a naming convention here,
		 * where vectorized function names are regular function names with _vec
		 * appended to them.
		 */
		transitionFuncName = get_func_name(peraggstate->transfn_oid);
		snprintf(vectorTransitionFuncName, NAMEDATALEN, "%s_vec", transitionFuncName);

		qualVectorTransitionFuncName =
			stringToQualifiedNameList(vectorTransitionFuncName);
		vectorTransitionFuncList = FuncnameGetCandidates(qualVectorTransitionFuncName,
														 argumentCount, NIL,
														 false, false);

		if (vectorTransitionFuncList != NULL)
		{
			Oid functionOid = vectorTransitionFuncList->oid;
			fmgr_info(functionOid, &peraggstate->transfn);
		}

		fcinfo.arg[1] = PointerGetDatum(columnData);
		fcinfo.arg[2] = PointerGetDatum(&rowCount);
		fcinfo.arg[3] = PointerGetDatum(&blockRowCount);

		/* we can apply the transition function immediately */
		advance_transition_function_vectorized(aggstate, peraggstate, 
											   pergroupstate, &fcinfo);
	}
}


/*
 * Similar to advance_transition_function, but in vectorized version we don't
 * check for nulls. A stripe should be never null. So handling null values is
 * the responsibility of the related trans function.
 */
static void
advance_transition_function_vectorized(AggState *aggstate, AggStatePerAgg peraggstate,
									   AggStatePerGroup pergroupstate, 
									   FunctionCallInfoData *fcinfo)
{
	int	numArguments = peraggstate->numArguments;
	MemoryContext oldContext;
	Datum newVal;

	/* we run the transition functions in per-input-tuple memory context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	/* OK to call the transition function */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn), numArguments + 1,
							 peraggstate->aggCollation, (void *) aggstate, NULL);
	fcinfo->arg[0] = pergroupstate->transValue;
	fcinfo->argnull[0] = pergroupstate->transValueIsNull;
	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.	But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(aggstate->aggcontext);
			newVal = datumCopy(newVal, peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		if (!pergroupstate->transValueIsNull)
		{
			pfree(DatumGetPointer(pergroupstate->transValue));
		}
	}

	pergroupstate->transValue = newVal;
	pergroupstate->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}


static uint32
VectorizedHashTableHash(const void *key, Size keySize)
{
  uint32 hashKey = DatumGetUInt32(FunctionCall1(CurrentHashFunction,
												(*(Datum *) key)));
  return hashKey;
}


static int
VectorizedHashTableMatch(const void *key1, const void *key2,
						 Size keySize)
{
  	bool keysEqual = DatumGetBool(FunctionCall2(CurrentEqualityFunction,
												(*(Datum *) key1),
												(*(Datum *) key2)));
  	if (keysEqual)
   	{
		return 0;
   	}

 	return 1;
}
