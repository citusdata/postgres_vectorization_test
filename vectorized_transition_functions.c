#include "postgres.h"
#include "cstore_fdw.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>

#include "access/hash.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/int8.h"
#include "utils/numeric.h"

Datum int4_sum_vec(PG_FUNCTION_ARGS);
Datum int8_sum_vec(PG_FUNCTION_ARGS);
Datum int4_avg_accum_vec(PG_FUNCTION_ARGS);
Datum int8_avg_accum_vec(PG_FUNCTION_ARGS);
Datum int8inc_vec(PG_FUNCTION_ARGS);
Datum int8inc_any_vec(PG_FUNCTION_ARGS);
Datum float4pl_vec(PG_FUNCTION_ARGS);
Datum float8pl_vec(PG_FUNCTION_ARGS);
Datum float4_accum_vec(PG_FUNCTION_ARGS);
Datum float8_accum_vec(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(int4_sum_vec);
PG_FUNCTION_INFO_V1(int8_sum_vec);
PG_FUNCTION_INFO_V1(int4_avg_accum_vec);
PG_FUNCTION_INFO_V1(int8_avg_accum_vec);
PG_FUNCTION_INFO_V1(int8inc_vec);
PG_FUNCTION_INFO_V1(int8inc_any_vec);
PG_FUNCTION_INFO_V1(float4pl_vec);
PG_FUNCTION_INFO_V1(float8pl_vec);
PG_FUNCTION_INFO_V1(float4_accum_vec);
PG_FUNCTION_INFO_V1(float8_accum_vec);


/*
 * Routines for avg(int2) and avg(int4).  The transition datatype
 * is a two-element int8 array, holding count and sum.
 */
typedef struct Int8TransTypeData
{
	int64		count;
	int64		sum;
} Int8TransTypeData;


static ArrayType *
do_numeric_avg_accum(ArrayType *transarray, Numeric newval)
{
	Datum	   *transdatums;
	int			ndatums;
	Datum		N,
				sumX;
	ArrayType  *result;

	/* We assume the input is array of numeric */
	deconstruct_array(transarray,
					  NUMERICOID, -1, false, 'i',
					  &transdatums, NULL, &ndatums);
	if (ndatums != 2)
	{
		elog(ERROR, "expected 2-element numeric array");
	}

	N = transdatums[0];
	sumX = transdatums[1];

	N = DirectFunctionCall1(numeric_inc, N);
	sumX = DirectFunctionCall2(numeric_add, sumX,
							   NumericGetDatum(newval));

	transdatums[0] = N;
	transdatums[1] = sumX;

	result = construct_array(transdatums, 2,
							 NUMERICOID, -1, false, 'i');

	return result;
}


static float8 *
check_float8_array(ArrayType *transarray, const char *caller, int n)
{
	/*
	 * We expect the input to be an N-element float array; verify that. We
	 * don't need to use deconstruct_array() since the array data is just
	 * going to look like a C array of N float8 values.
	 */
	if (ARR_NDIM(transarray) != 1 ||
		ARR_DIMS(transarray)[0] != n ||
		ARR_HASNULL(transarray) ||
		ARR_ELEMTYPE(transarray) != FLOAT8OID)
	{
		elog(ERROR, "%s: expected %d-element float8 array", caller, n);
	}

	return (float8 *) ARR_DATA_PTR(transarray);
}


Datum
int4_sum_vec(PG_FUNCTION_ARGS)
{
	ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));
	int64 newValue = 0;
	uint32 i = 0;

	if (PG_ARGISNULL(0))
	{
		newValue = 0;
	}
	else
	{
		newValue = PG_GETARG_INT64(0);
	}

	for (i = 0; i < rowCount; i++)
	{
		int blockIndex = i / blockRowCount;
		int rowIndex = i % blockRowCount;

		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		Datum value = blockData->valueArray[rowIndex];
		bool exists = blockData->existsArray[rowIndex];

		if (exists)
		{
			newValue = newValue + (int64) DatumGetInt32(value);
		}
	}

	PG_RETURN_INT64(newValue);
}


Datum
int8_sum_vec(PG_FUNCTION_ARGS)
{
	ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));
	Datum newValue;
	uint32 i = 0;

	if (PG_ARGISNULL(0))
	{
		newValue = DirectFunctionCall1(int8_numeric, Int64GetDatum(0));
	}
	else
	{
		newValue = PG_GETARG_DATUM(0);
	}

	for (i = 0; i < rowCount; i++)
	{
		int blockIndex = i / blockRowCount;
		int rowIndex = i % blockRowCount;

		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		Datum value = blockData->valueArray[rowIndex];
		bool exists = blockData->existsArray[rowIndex];

		if (exists)
		{
			newValue = DirectFunctionCall2(numeric_add,
										   newValue,
										   DirectFunctionCall1(int8_numeric, value));
		}
	}

	PG_RETURN_DATUM(newValue);
}


Datum
int4_avg_accum_vec(PG_FUNCTION_ARGS)
{
	ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
	ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));

	int64 newValue = 0;
	uint32 i = 0;
	uint32 realCount = 0;
	Int8TransTypeData *transdata = NULL;

	if (ARR_HASNULL(transarray) || 
		ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
	{
		elog(ERROR, "expected 2-element int8 array");
	}

	for (i = 0; i < rowCount; i++)
	{
		int blockIndex = i / blockRowCount;
		int rowIndex = i % blockRowCount;
		
		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		Datum value = blockData->valueArray[rowIndex];
		bool exists = blockData->existsArray[rowIndex];
		
		if (exists)
		{
			newValue = newValue + (int64) DatumGetInt32(value);
			realCount++;
		}
	}

	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);
	transdata->count = transdata->count + realCount;
	transdata->sum = transdata->sum + newValue;
	PG_RETURN_ARRAYTYPE_P(transarray);
}


Datum
int8_avg_accum_vec(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));
	Numeric newValue = NULL;

	uint32 i = 0;
	for (i = 0; i < rowCount; i++)
	{
		int blockIndex = i / blockRowCount;
		int rowIndex = i % blockRowCount;

		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		Datum value = blockData->valueArray[rowIndex];
		bool exists = blockData->existsArray[rowIndex];

		if (exists)
		{
			newValue = DatumGetNumeric(DirectFunctionCall1(int8_numeric, value));
			transarray = do_numeric_avg_accum(transarray, newValue);
		}
	}

	PG_RETURN_ARRAYTYPE_P(transarray);
}


Datum
int8inc_vec(PG_FUNCTION_ARGS)
{
	int64 arg = PG_GETARG_INT64(0);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	int64 result = arg + (int64) rowCount;

	/* Overflow check */
	if (result < arg)
	{
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	}

	PG_RETURN_INT64(result);
}


Datum
int8inc_any_vec(PG_FUNCTION_ARGS)
{
	int64 arg = PG_GETARG_INT64(0);
	ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));
	uint32 i = 0;
	int64 result = arg;

	for (i = 0; i < rowCount; i++)
	{
		uint32 blockIndex = i / blockRowCount;
		uint32 rowIndex = i % blockRowCount;
		
		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		bool exists = blockData->existsArray[rowIndex];
		
		if (exists)
		{
			result++;
			if (result < arg)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("bigint out of range")));
		}
	}
	
	PG_RETURN_INT64(result);
}


Datum
float4pl_vec(PG_FUNCTION_ARGS)
{
    ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));
	float4 newValue = 0.0;
	uint32 i = 0;

	if (PG_ARGISNULL(0))
	{
		newValue = 0.0;
	}
	else
	{
		newValue = PG_GETARG_FLOAT4(0);
	}

	for (i = 0; i < rowCount; i++)
	{
		uint32 blockIndex = i / blockRowCount;
		uint32 rowIndex = i % blockRowCount;

		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		Datum value = blockData->valueArray[rowIndex];
		bool exists = blockData->existsArray[rowIndex];

		if (exists)
		{
			newValue = newValue + DatumGetFloat4(value);
		}
	}

	PG_RETURN_FLOAT4(newValue);
}


Datum
float8pl_vec(PG_FUNCTION_ARGS)
{
    ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));
	float8 newValue = 0.0;
	uint32 i = 0;

	if (PG_ARGISNULL(0))
	{
		newValue = 0.0;
	}
	else
	{
		newValue = PG_GETARG_FLOAT8(0);
	}

	for (i = 0; i < rowCount; i++)
	{
		uint32 blockIndex = i / blockRowCount;
		uint32 rowIndex = i % blockRowCount;

		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		Datum value = blockData->valueArray[rowIndex];
		bool exists = blockData->existsArray[rowIndex];

		if (exists)
		{
			newValue = newValue + DatumGetFloat8(value);
		}
	}

	PG_RETURN_FLOAT8(newValue);
}


Datum
float8_accum_vec(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);

	ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));

	uint32 i = 0;
	float8 *transvalues = NULL;
	float8 N = 0.0;
	float8 sumX = 0.0;

	transvalues = check_float8_array(transarray, "float8_accum_vec", 3);
	N = transvalues[0];
	sumX = transvalues[1];

	for (i = 0; i < rowCount; i++)
	{
		uint32 blockIndex = i / blockRowCount;
		uint32 rowIndex = i % blockRowCount;
		
		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		Datum value = blockData->valueArray[rowIndex];
		bool exists = blockData->existsArray[rowIndex];
		
		if (exists)
		{
			sumX = sumX + DatumGetFloat8(value);
			N++;
		}
	}
	transvalues[0] = N;
	transvalues[1] = sumX;

	PG_RETURN_ARRAYTYPE_P(transarray);
}


Datum
float4_accum_vec(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	ColumnData *columnData = (ColumnData *) PG_GETARG_POINTER(1);
	uint32 rowCount = *((uint32 *) PG_GETARG_POINTER(2));
	uint64 blockRowCount = *((uint64 *) PG_GETARG_POINTER(3));

	uint32 i = 0;
	float8 *transvalues = NULL;
	float8 N = 0.0;
	float8 sumX = 0.0;

	transvalues = check_float8_array(transarray, "float8_accum_vec", 3);
	N = transvalues[0];
	sumX = transvalues[1];

	for (i = 0; i < rowCount; i++)
	{
		uint32 blockIndex = i / blockRowCount;
		uint32 rowIndex = i % blockRowCount;
		
		ColumnBlockData *blockData = columnData->blockDataArray[blockIndex];
		Datum value = blockData->valueArray[rowIndex];
		bool exists = blockData->existsArray[rowIndex];
		
		if (exists)
		{
			sumX = sumX + (float8) DatumGetFloat4(value);
			N++;
		}
	}

	transvalues[0] = N;
	transvalues[1] = sumX;

	PG_RETURN_ARRAYTYPE_P(transarray);
}
