#ifndef VECTORIZED_AGGREGATES_H
#define VECTORIZED_AGGREGATES_H

#include "executor/execdesc.h"
#include "nodes/parsenodes.h"


extern void vectorized_ExecutorRun(QueryDesc *queryDesc,
					 ScanDirection direction, long count);

extern TupleTableSlot *ExecProcNodeVectorized(PlanState *node);

extern TupleTableSlot *ExecAggVectorized(AggState *node);

#endif
