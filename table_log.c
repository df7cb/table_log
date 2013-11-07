/*
 * table_log () -- log changes to another table
 *
 *
 * see README.table_log for details
 *
 *
 * written by Andreas ' ads' Scherbaum (ads@pgug.de)
 *
 */

#include "table_log.h"

#include <ctype.h>		/* tolower () */
#include <string.h>		/* strlen() */

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"	/* this is what you need to work with SPI */
#include "catalog/namespace.h"
#include "commands/trigger.h"	/* -"- and triggers */
#include "mb/pg_wchar.h"	/* support for the quoting functions */
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/guc.h"
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/timestamp.h>
#include <utils/syscache.h>
#include "funcapi.h"

/* for PostgreSQL >= 8.2.x */
#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif


#ifndef PG_NARGS
/*
 * Get number of arguments passed to function.
 * this macro isnt defined in 7.2.x
 */
#define PG_NARGS() (fcinfo->nargs)
#endif

/*
 * Current active log table partition. Default is always zero.
 */
TableLogPartitionId tableLogActivePartitionId = 0;

/*
 * table_log restore descriptor.
 *
 * Carries all information to restore data from a table_log table
 */
typedef struct
{
	char *schema;
	char *relname;
} TableLogRelIdent;

/*
 * table_log logging descriptor for log triggers.
 */
typedef struct
{
	/*
	 * Pointer to trigger data
	 */
	TriggerData *trigdata;

	/*
	 * Number of columns of source table (excludes
	 * dropped columns!)
	 */
	int number_columns;

	/*
	 * Number of columns of log table (excludes
	 * dropped columns!)
	 */
	int number_columns_log;

	/*
	 * Name/schema of the log table
	 */
	TableLogRelIdent ident_log;

	/*
	 * Log session user
	 */
	int use_session_user;

} TableLogDescr;

/*
 * table_log restore descriptor structure.
 */
typedef struct
{
	/* Non-qualified relation name
	 * of original table.
	 */
	char *orig_relname;

	/*
	 * OID of original table, saved
	 * for cache lookup.
	 */
	Oid orig_relid;

	/*
	 * List of attnums of original table part
	 * of the primary key or unique constraint. Only
	 * used in case of no explicit specified pk column.
	 * (see relationGetPrimaryKeyColumns() for details).
	 */
	AttrNumber *orig_pk_attnum;

	/*
	 * Number of pk attributes in original tables.
	 */
	int orig_num_pk_attnums;

	/*
	 * List of attribute names. The list index matches
	 * the attribute number stored in the orig_pk_attnum
	 * array.
	 */
	List *orig_pk_attr_names;

	/*
	 * OID of log table.
	 */
	Oid log_relid;

	/*
	 * Possible schema qualified relation name
	 * of log table.
	 */
	bool use_schema_log;
	union
	{
		TableLogRelIdent ident_log;
		char *relname_log;
	};

	/*
	 * Primary key column name of the log table.
	 */
	char *pkey_log;

	/*
	 * OID of restore table.
	 */
	Oid restore_relid;

	/*
	 * Possible schema qualified relation name
	 * of restore table.
	 */
	bool use_schema_restore;
	union
	{
		TableLogRelIdent ident_restore;
		char *relname_restore;
	};
} TableLogRestoreDescr;

#define DESCR_TRIGDATA(a) \
	(a).trigdata

#define DESCR_TRIGDATA_GET_TUPDESC(a) \
	(a).trigdata->tg_relation->rd_att

#define DESCR_TRIGDATA_GET_RELATION(a) \
	(a).trigdata->tg_relation

#define DESCR_TRIGDATA_GETARG(a, index) \
	(a).trigdata->tg_trigger->tgargs[(index)]

#define DESCR_TRIGDATA_NARGS(a) \
	(a).trigdata->tg_trigger->tgnargs

#define DESCR_TRIGDATA_GET_TUPLE(a) \
	(a).trigdata->tg_trigtuple

#define DESCR_TRIGDATA_GET_NEWTUPLE(a) \
	(a).trigdata->tg_newtuple

#define DESCR_TRIGDATA_LOG_SCHEMA(a) \
	(a).trigdata->tg_trigger->tgargs[2]

#define DESCR_TRIGDATA_LOG_SESSION_USER(a) \
	(a).trigdata->tg_trigger->tgargs[1]

#define RESTORE_TABLE_IDENT(a, type) \
	((a.use_schema_##type )								 \
	 ? quote_qualified_identifier(a.ident_##type.schema, \
								  a.ident_##type.relname)\
	 : quote_identifier(a.relname_##type))

void _PG_init(void);
Datum table_log(PG_FUNCTION_ARGS);
Datum table_log_basic(PG_FUNCTION_ARGS);
Datum table_log_restore_table(PG_FUNCTION_ARGS);
static char *do_quote_ident(char *iptr);
static char *do_quote_literal(char *iptr);
static void __table_log (TableLogDescr *descr,
						 char          *changed_mode,
						 char          *changed_tuple,
						 HeapTuple      tuple);
static void table_log_prepare(TableLogDescr *descr);
static void table_log_finalize(void);
static void __table_log_restore_table_insert(SPITupleTable *spi_tuptable,
											 char *table_restore,
											 char *table_orig_pkey,
											 char *col_query_start,
											 int col_pkey,
											 int number_columns,
											 int i);
static void __table_log_restore_table_update(SPITupleTable *spi_tuptable,
											 char *table_restore,
											 char *table_orig_pkey,
											 char *col_query_start,
											 int col_pkey,
											 int number_columns,
											 int i,
											 char *old_key_string);
static void __table_log_restore_table_delete(SPITupleTable *spi_tuptable,
											 char *table_restore,
											 char *table_orig_pkey,
											 char *col_query_start,
											 int col_pkey,
											 int number_columns,
											 int i);
static char *__table_log_varcharout(VarChar *s);
static int count_columns (TupleDesc tupleDesc);
static void mapPrimaryKeyColumnNames(TableLogRestoreDescr *restore_descr);
static void setTableLogRestoreDescr(TableLogRestoreDescr *restore_descr,
									char *table_orig,
									char *table_orig_pkey,
									char *table_log,
									char *table_log_pkey,
									char *table_restore);
static void getRelationPrimaryKeyColumns(TableLogRestoreDescr *restore_descr);

/* this is a V1 (new) function */
/* the trigger function */
PG_FUNCTION_INFO_V1(table_log);
PG_FUNCTION_INFO_V1(table_log_forward);
/* build only, if the 'Table Function API' is available */
#ifdef FUNCAPI_H_not_implemented
/* restore one single column */
PG_FUNCTION_INFO_V1(table_log_show_column);
#endif /* FUNCAPI_H */
/* restore a full table */
PG_FUNCTION_INFO_V1(table_log_restore_table);

/*
 * Initialize table_log module and various internal
 * settings like customer variables.
 */
void _PG_init(void)
{
	DefineCustomIntVariable("table_log.active_partition",
							"Sets the current active partition identifier.",
							NULL,
							&tableLogActivePartitionId,
							0,
							0,
							MAX_TABLE_LOG_PARTITIONS - 1,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);
}

/*
 * Returns a fully formatted log table relation name
 * of the current active log table partition.
 *
 * The table_name argument is adjusted to match either a single
 * table or a partitioned log table with _n appended, where n matches
 * the current selected active partition id
 * (see tableLogActivePartitionId).
 */
static inline char *getActiveLogTable(TriggerData *tg_data)
{
	bool       use_partitions = false;
	StringInfo buf            = makeStringInfo();

	/*
	 * If we use several partitions for the log table, append
	 * the partition id.
	 */
	if (tg_data->tg_trigger->tgnargs == 4)
	{
		/*
		 * Examine trigger argument list. We expect the
		 * partition mode to be the 4th argument to the table_log()
		 * trigger. In case no argument was specified, we know that
		 * we are operating on an old version, so assume
		 * a non-partitioned installation automatically.
		 */
		if (strcmp(tg_data->tg_trigger->tgargs[3], "PARTITION") == 0)
		{
			/* Partition support enabled */
			use_partitions = true;
		}
	}

	if (tg_data->tg_trigger->tgnargs > 0)
	{
		appendStringInfoString(buf, tg_data->tg_trigger->tgargs[0]);
	}
	else
	{
		/*
		 * We must deal with no arguments given to the trigger. In this
		 * case the log table name is the same like the table we are
		 * called on, plus the _log appended...
		 */
		appendStringInfo(buf, "%s_log", SPI_getrelname(tg_data->tg_relation));
	}

	if (use_partitions)
	{
		/*
		 * Append the current active partition id, if partitioning
		 * support is used.
		 */
		appendStringInfo(buf, "_%u", tableLogActivePartitionId);
	}

	/* ...and we're done */
	return buf->data;
}

/*
 * count_columns (TupleDesc tupleDesc)
 * Will count and return the number of columns in the table described by
 * tupleDesc. It needs to ignore dropped columns.
 */
static int count_columns (TupleDesc tupleDesc)
{
	int count = 0;
	int i;

	for (i = 0; i < tupleDesc->natts; ++i)
	{
		if (!tupleDesc->attrs[i]->attisdropped)
		{
			++count;
		}
	}

	return count;
}

/*
 * Initialize a TableLogDescr descriptor structure.
 */
static void initTableLogDescr(TableLogDescr *descr,
							  TriggerData   *trigdata)
{
	Assert(descr != NULL);

	descr->trigdata = trigdata;

	descr->number_columns = -1;
	descr->number_columns_log = -1;
	descr->ident_log.schema   = NULL;
	descr->ident_log.relname  = NULL;
	descr->use_session_user   = 0;
}

/*
 * table_log_internal()
 *
 * Internal function to initialize all required stuff
 * for table_log() or table_log_basic().
 *
 * Requires a TableLogDescr structure previously
 * initialized via initTableLogDescr().
 */
static void table_log_prepare(TableLogDescr *descr)
{
	int         ret;
	StringInfo  query;

	/* must only be called for ROW trigger */
	if (TRIGGER_FIRED_FOR_STATEMENT(descr->trigdata->tg_event))
	{
		elog(ERROR, "table_log: can't process STATEMENT events");
	}

	/* must only be called AFTER */
	if (TRIGGER_FIRED_BEFORE(descr->trigdata->tg_event))
	{
		elog(ERROR, "table_log: must be fired after event");
	}

	/* now connect to SPI manager */
	ret = SPI_connect();

	if (ret != SPI_OK_CONNECT)
	{
		elog(ERROR, "table_log: SPI_connect returned %d", ret);
	}

	elog(DEBUG2, "prechecks done, now getting original table attributes");

	descr->number_columns = count_columns(DESCR_TRIGDATA_GET_TUPDESC((*descr)));
	if (descr->number_columns < 1)
	{
		elog(ERROR, "table_log: number of columns in table is < 1, can this happen?");
	}

	elog(DEBUG2, "number columns in orig table: %i", descr->number_columns);

	if (DESCR_TRIGDATA_NARGS((*descr)) > 4)
	{
		elog(ERROR, "table_log: too many arguments to trigger");
	}

	/* name of the log schema */
	if (DESCR_TRIGDATA_NARGS((*descr)) <= 2)
	{
		/* if no explicit schema specified, use source table schema  */
		descr->ident_log.schema = get_namespace_name(RelationGetNamespace(DESCR_TRIGDATA_GET_RELATION((*descr))));
	}
	else
	{
		descr->ident_log.schema  = DESCR_TRIGDATA_LOG_SCHEMA((*descr));
	}

	/* name of the log table */
	descr->ident_log.relname = getActiveLogTable(DESCR_TRIGDATA((*descr)));

	/* should we write the current user? */
	if (DESCR_TRIGDATA_NARGS((*descr)) > 1)
	{
		/*
		 * check if a second argument is given
		 * if yes, use it, if it is true
		 */
		if (atoi(DESCR_TRIGDATA_LOG_SESSION_USER((*descr))) == 1)
		{
			descr->use_session_user = 1;
			elog(DEBUG2, "will write session user to 'trigger_user'");
		}
	}

	elog(DEBUG2, "log table: %s.%s",
		 quote_identifier(descr->ident_log.schema),
		 quote_identifier(descr->ident_log.relname));

	/* get the number columns in the table */
	query = makeStringInfo();
	appendStringInfo(query, "%s.%s",
					 do_quote_ident(descr->ident_log.schema),
					 do_quote_ident(descr->ident_log.relname));
	descr->number_columns_log = count_columns(RelationNameGetTupleDesc(query->data));

	if (descr->number_columns_log < 1)
	{
		elog(ERROR, "could not get number columns in relation %s.%s",
			 quote_identifier(descr->ident_log.schema),
			 quote_identifier(descr->ident_log.relname));
	}

    elog(DEBUG2, "number columns in log table: %i",
		 descr->number_columns_log);

	/*
	 * check if the logtable has 3 (or now 4) columns more than our table
	 * +1 if we should write the session user
	 */

	if (descr->use_session_user == 0)
	{
		/* without session user */
		if ((descr->number_columns_log != descr->number_columns + 3)
			&& (descr->number_columns_log != descr->number_columns + 4))
		{
			elog(ERROR, "number colums in relation %s(%d) does not match columns in %s.%s(%d)",
				 SPI_getrelname(DESCR_TRIGDATA_GET_RELATION((*descr))),
				 descr->number_columns,
				 quote_identifier(descr->ident_log.schema),
				 quote_identifier(descr->ident_log.relname),
				 descr->number_columns_log);
		}
	}
	else
	{
		/* with session user */
		if ((descr->number_columns_log != descr->number_columns + 3 + 1)
			&& (descr->number_columns_log != descr->number_columns + 4 + 1))
		{
			elog(ERROR, "number colums in relation %s does not match columns in %s.%s",
				 SPI_getrelname(DESCR_TRIGDATA_GET_RELATION((*descr))),
				 quote_identifier(descr->ident_log.schema),
				 quote_identifier(descr->ident_log.relname));
		}
	}

	elog(DEBUG2, "log table OK");
	/* For each column in key ... */
	elog(DEBUG2, "copy data ...");
}

static void table_log_finalize()
{
	/* ...for now only SPI needs to be cleaned up. */
	SPI_finish();
}

/*
 * table_log_forward
 *
 * Trigger function with the same core functionality
 * than table_log(), but without the possibility to do
 * backward log replay. This means that NEW tuples for UPDATE
 * actions aren't logged, which makes the log table much smaller
 * in case someone have a heavy updated source table.
 */
Datum table_log_basic(PG_FUNCTION_ARGS)
{
	TableLogDescr  log_descr;

	/*
	 * Some checks first...
	 */

	elog(DEBUG2, "start table_log()");

	/* called by trigger manager? */
	if (!CALLED_AS_TRIGGER(fcinfo))
	{
		elog(ERROR, "table_log: not fired by trigger manager");
	}

	/*
	 * Assign trigger data structure to table log descriptor.
	 */
	initTableLogDescr(&log_descr,
					  (TriggerData *) fcinfo->context);

	/*
	 * Do all the preparing leg work...
	 */
	table_log_prepare(&log_descr);

	if (TRIGGER_FIRED_BY_INSERT(DESCR_TRIGDATA(log_descr)->tg_event))
	{
		/* trigger called from INSERT */
		elog(DEBUG2, "mode: INSERT -> new");

		__table_log(&log_descr,
					"INSERT",
					"new",
					DESCR_TRIGDATA_GET_TUPLE(log_descr));
	}
	else if (TRIGGER_FIRED_BY_UPDATE(DESCR_TRIGDATA(log_descr)->tg_event))
	{
		elog(DEBUG2, "mode: UPDATE -> old");

		__table_log(&log_descr,
					"UPDATE",
					"old",
					DESCR_TRIGDATA_GET_TUPLE(log_descr));
	}
	else if (TRIGGER_FIRED_BY_DELETE(DESCR_TRIGDATA(log_descr)->tg_event))
	{
		/* trigger called from DELETE */
		elog(DEBUG2, "mode: DELETE -> old");

		__table_log(&log_descr,
					"DELETE",
					"old",
					DESCR_TRIGDATA_GET_TUPLE(log_descr));
	}
	else
	{
		elog(ERROR, "trigger fired by unknown event");
	}

	elog(DEBUG2, "cleanup, trigger done");

	table_log_finalize();

	/* return trigger data */
	return PointerGetDatum(DESCR_TRIGDATA_GET_TUPLE(log_descr));
}

/*
table_log()

trigger function for logging table changes

parameter:
  - log table name (optional)
return:
  - trigger data (for Pg)
*/
Datum table_log(PG_FUNCTION_ARGS)
{
	TableLogDescr  log_descr;

	/*
	 * Some checks first...
	 */

	elog(DEBUG2, "start table_log()");

	/* called by trigger manager? */
	if (!CALLED_AS_TRIGGER(fcinfo))
	{
		elog(ERROR, "table_log: not fired by trigger manager");
	}

	/*
	 * Assign trigger data structure to table log descriptor.
	 */
	initTableLogDescr(&log_descr,
					  (TriggerData *) fcinfo->context);

	/*
	 * Do all the preparing leg work...
	 */
	table_log_prepare(&log_descr);


	if (TRIGGER_FIRED_BY_INSERT(DESCR_TRIGDATA(log_descr)->tg_event))
	{
		/* trigger called from INSERT */
		elog(DEBUG2, "mode: INSERT -> new");

		__table_log(&log_descr,
					"INSERT",
					"new",
					DESCR_TRIGDATA_GET_TUPLE(log_descr));
	}
	else if (TRIGGER_FIRED_BY_UPDATE(DESCR_TRIGDATA(log_descr)->tg_event))
	{
		/* trigger called from UPDATE */
		elog(DEBUG2, "mode: UPDATE -> old");

		__table_log(&log_descr,
					"UPDATE",
					"old",
					DESCR_TRIGDATA_GET_TUPLE(log_descr));

		elog(DEBUG2, "mode: UPDATE -> new");

		__table_log(&log_descr,
					"UPDATE",
					"new",
					DESCR_TRIGDATA_GET_NEWTUPLE(log_descr));
	}
	else if (TRIGGER_FIRED_BY_DELETE(DESCR_TRIGDATA(log_descr)->tg_event))
	{
		/* trigger called from DELETE */
		elog(DEBUG2, "mode: DELETE -> old");

		__table_log(&log_descr,
					"DELETE",
					"old",
					DESCR_TRIGDATA_GET_TUPLE(log_descr));
	}
	else
	{
		elog(ERROR, "trigger fired by unknown event");
	}

	elog(DEBUG2, "cleanup, trigger done");

	table_log_finalize();

	/* return trigger data */
	return PointerGetDatum(DESCR_TRIGDATA_GET_TUPLE(log_descr));
}

/*
__table_log()

helper function for table_log()

parameter:
  - trigger data
  - change mode (INSERT, UPDATE, DELETE)
  - tuple to log (old, new)
  - pointer to tuple
  - number columns in table
  - logging table
  - flag for writing session user
return:
  none
*/
static void __table_log (TableLogDescr *descr,
						 char          *changed_mode,
						 char          *changed_tuple,
						 HeapTuple      tuple)
{
	StringInfo query;
	char      *before_char;
	int        i;
	int        col_nr;
	int        found_col;
	int        ret;

	elog(DEBUG2, "build query");

	/* allocate memory */
	query = makeStringInfo();

	/* build query */
	appendStringInfo(query, "INSERT INTO %s.%s (",
					 do_quote_ident(descr->ident_log.schema),
					 do_quote_ident(descr->ident_log.relname));

	/* add colum names */
	col_nr = 0;

	for (i = 1; i <= descr->number_columns; i++)
	{
		col_nr++;
		found_col = 0;

		do
		{
			if (DESCR_TRIGDATA_GET_TUPDESC((*descr))->attrs[col_nr - 1]->attisdropped)
			{
				/* this column is dropped, skip it */
				col_nr++;
				continue;
			}
			else
			{
				found_col++;
			}
		}
		while (found_col == 0);

		appendStringInfo(query,
						 "%s, ",
						 do_quote_ident(SPI_fname(DESCR_TRIGDATA_GET_TUPDESC((*descr)), col_nr)));
	}

	/* add session user */
	if (descr->use_session_user == 1)
		appendStringInfo(query, "trigger_user, ");

	/* add the 3 extra colum names */
	appendStringInfo(query, "trigger_mode, trigger_tuple, trigger_changed) VALUES (");

	/* add values */
	col_nr = 0;
	for (i = 1; i <= descr->number_columns; i++)
	{
		col_nr++;
		found_col = 0;

		do
		{
			if (DESCR_TRIGDATA_GET_TUPDESC((*descr))->attrs[col_nr - 1]->attisdropped)
			{
				/* this column is dropped, skip it */
				col_nr++;
				continue;
			}
			else
			{
				found_col++;
			}
		}
		while (found_col == 0);

		before_char = SPI_getvalue(tuple,
								   DESCR_TRIGDATA_GET_TUPDESC((*descr)),
								   col_nr);

		if (before_char == NULL)
		{
			appendStringInfo(query, "NULL, ");
		}
		else
		{
			appendStringInfo(query, "%s, ",
							 do_quote_literal(before_char));
		}
	}

	/* add session user */
	if (descr->use_session_user == 1)
		appendStringInfo(query, "SESSION_USER, ");

	/* add the 3 extra values */
	appendStringInfo(query, "%s, %s, NOW());",
					 do_quote_literal(changed_mode),
					 do_quote_literal(changed_tuple));

	elog(DEBUG3, "query: %s", query->data);
	elog(DEBUG2, "execute query");

	/* execute insert */
	ret = SPI_exec(query->data, 0);
	if (ret != SPI_OK_INSERT)
	{
		elog(ERROR, "could not insert log information into relation %s.%s (error: %d)",
			 quote_identifier(descr->ident_log.schema),
			 quote_identifier(descr->ident_log.relname),
			 ret);
	}

	/* clean up */
	pfree(query->data);
	pfree(query);

	elog(DEBUG2, "done");
}


#ifdef FUNCAPI_H_not_implemented
/*
table_log_show_column()

show a single column on a date in the past

parameter:
  not yet defined
return:
  not yet defined
*/
Datum table_log_show_column(PG_FUNCTION_ARGS)
{
	TriggerData    *trigdata = (TriggerData *) fcinfo->context;
	int            ret;

	/*
	 * Some checks first...
	 */
	elog(DEBUG2, "start table_log_show_column()");

	/* Connect to SPI manager */
	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
	{
		elog(ERROR, "table_log_show_column: SPI_connect returned %d", ret);
	}

	elog(DEBUG2, "this function isnt available yet");

	/* close SPI connection */
	SPI_finish();
	return PG_RETURN_NULL;
}
#endif /* FUNCAPI_H */

/*
 * Retrieves the columns of the primary key the original
 * table has and stores their attribute numbers in the
 * specified TableLogRestoreDescr descriptor. The caller is responsible
 * to pass a valid descriptor initialized by initTableLogRestoreDescr().
 */
static void getRelationPrimaryKeyColumns(TableLogRestoreDescr *restore_descr)
{
	Relation  origRel;
	List     *indexOidList;
	ListCell *indexOidScan;

	Assert((restore_descr != NULL)
		   && (restore_descr->orig_relname != NULL));

	restore_descr->orig_pk_attnum = NULL;

	/*
	 * Get all indexes for the relation, take care to
	 * request a share lock before.
	 */
	origRel = heap_open(restore_descr->orig_relid, AccessShareLock);

	indexOidList = RelationGetIndexList(origRel);

	foreach(indexOidScan, indexOidList)
	{
		Oid indexOid = lfirst_oid(indexOidScan);
		Form_pg_index indexStruct;
		HeapTuple     indexTuple;
		int           i;

		/*
		 * Lookup the key via syscache, extract the key columns
		 * from this index in case we have found a primary key.
		 */
		indexTuple = SearchSysCache1(INDEXRELID,
									 ObjectIdGetDatum(indexOid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexOid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Next one if this is not a primary key or
		 * unique constraint.
		 */
		if (!indexStruct->indisprimary)
			continue;

		/*
		 * Okay, looks like this is a PK let's
		 * get the attnums from it and store them
		 * in the TableLogRestoreDescr descriptor.
		 */
		restore_descr->orig_num_pk_attnums = indexStruct->indnatts;
		restore_descr->orig_pk_attnum = (AttrNumber *) palloc(indexStruct->indnatts
															  * sizeof(AttrNumber));
		for (i = 0; i < indexStruct->indnatts; i++)
		{
			restore_descr->orig_pk_attnum[i] = indexStruct->indkey.values[i];
		}
	}

	/*
	 * Okay, we're done. Cleanup and exit.
	 */
	heap_close(origRel, AccessShareLock);
}

static void setTableLogRestoreDescr(TableLogRestoreDescr *restore_descr,
									char *table_orig,
									char *table_orig_pkey,
									char *table_log,
									char *table_log_pkey,
									char *table_restore)
{
	List *logIdentList;
	List *restoreIdentList;
	int   i;

	Assert(restore_descr != NULL);

	/*
	 * Setup some stuff...
	 */
	restore_descr->orig_num_pk_attnums = 0;
	restore_descr->orig_pk_attr_names  = NIL;
	restore_descr->orig_pk_attnum      = NULL;
	restore_descr->orig_relname        = pstrdup(table_orig);

	/*
	 * Take care for possible schema qualified relation names
	 * in table_log and table_restore. table_orig is assumed to
	 * be search_path aware!
	 */

	if (!SplitIdentifierString(table_restore, '.', &restoreIdentList))
	{
		elog(ERROR, "invalid syntax for restore table name: \"%s\"",
			 table_restore);
	}

	if (!SplitIdentifierString(table_log, '.', &logIdentList))
	{
		elog(ERROR, "invalid syntax for log table name: \"%s\"",
			 table_restore);
	}

	/*
	 * Since the original table name is assumed
	 * not to be qualified, simply look it up by RelationGetRelid()
	 */
	restore_descr->orig_relid = RelnameGetRelid(restore_descr->orig_relname);

	if (restore_descr->orig_relid == InvalidOid)
	{
		elog(ERROR, "lookup for relation \"%s\" failed",
			 restore_descr->orig_relname);
	}

	/*
	 * Assign relation identifier to restore descriptor.
	 */
	if (list_length(logIdentList) > 1)
	{
		restore_descr->ident_log.schema  = (char *)lfirst(list_head(logIdentList));
		restore_descr->ident_log.relname = (char *)lfirst(list_tail(logIdentList));
		restore_descr->use_schema_log    = true;
	}
	else
	{
		restore_descr->relname_log    = table_log;
		restore_descr->use_schema_log = false;
	}

	if (list_length(restoreIdentList) > 1)
	{
		restore_descr->ident_restore.schema = (char *)lfirst(list_head(restoreIdentList));
		restore_descr->ident_restore.relname = (char *)lfirst(list_tail(restoreIdentList));
		restore_descr->use_schema_restore = true;
	}
	else
	{
		restore_descr->relname_restore = table_restore;
		restore_descr->use_schema_restore = false;
	}

	/*
	 * Lookup OID of log table. LookupExplicitNamespace()
	 * takes care wether we have at least USAGE on the specified
	 * namespace. We don't need to do that in case we have a
	 * non-qualified relation.
	 */
	if (restore_descr->use_schema_log)
	{
		Oid nspOid;

		nspOid = LookupExplicitNamespace(restore_descr->ident_log.schema);
		restore_descr->log_relid = get_relname_relid(restore_descr->ident_log.relname,
													 nspOid);
	}
	else
	{
		restore_descr->log_relid = RelnameGetRelid(restore_descr->relname_log);
	}

	/*
	 * ... the same for the restore table
	 */
	if (restore_descr->use_schema_restore)
	{
		Oid nspOid;

		nspOid = LookupExplicitNamespace(restore_descr->ident_restore.schema);
		restore_descr->restore_relid = get_relname_relid(restore_descr->ident_restore.relname,
														 nspOid);
	}
	else
	{
		restore_descr->restore_relid = RelnameGetRelid(restore_descr->relname_restore);
	}

	/*
	 * Primary key of original table, but only in case the
	 * caller didn't specify an explicit column.
	 *
	 * NOTE: This code is also responsible to support table_log_restore_table()
	 *       when having a composite primary key on a table. The old
	 *       API only allows for a single column to be specified, so to get
	 *       the new functionality the caller simply passes  NULL to
	 *       the table_orig_pkey value and let this code do all the
	 *       necessary legwork.
	 */
	if (table_orig_pkey == NULL)
	{
		getRelationPrimaryKeyColumns(restore_descr);
	}
	else
	{
		/*
		 * This is a single pkey column.
		 */
		restore_descr->orig_num_pk_attnums = 1;
		restore_descr->orig_pk_attnum      = (AttrNumber *) palloc(sizeof(AttrNumber));
		restore_descr->orig_pk_attnum[0]   = get_attnum(restore_descr->orig_relid,
														table_orig_pkey);
	}

	/*
	 * If there is no PK column, error out...
	 */
	if (restore_descr->orig_num_pk_attnums <= 0)
		elog(ERROR, "no primary key on table \"%s\" found",
			 restore_descr->orig_relname);

	/*
	 * Save the pk column name of the log table.
	 */
	restore_descr->pkey_log = pstrdup(table_log_pkey);

	/*
	 * Map the attribute number for the pk to its
	 * column names.
	 */
	mapPrimaryKeyColumnNames(restore_descr);

	/*
	 * The restore table only allows for a single primary key column.
	 * Check that this column isn't part of the original table's
	 * pkey.
	 */
	for (i = 0; i < restore_descr->orig_num_pk_attnums; i++)
	{
		if (strncmp(list_nth(restore_descr->orig_pk_attr_names, i),
					restore_descr->pkey_log,
					NAMEDATALEN) == 0)
		{
			elog(ERROR, "primary key of log table is part of original table");
		}
	}
}
/*
 * Takes a valid fully initialized TableLogRestoreDescr
 * and maps all attribute numbers from the original
 * table primary key to their column names.
 *
 * The caller must have called setTableLogRestoreDescr()
 * before.
 */
static void mapPrimaryKeyColumnNames(TableLogRestoreDescr *restore_descr)
{
	int i;

	/*
	 * Make sure we deal with an empty list.
	 */
	restore_descr->orig_pk_attr_names = NIL;

	for (i = 0; i < restore_descr->orig_num_pk_attnums; i++)
	{
		/*
		 * Lookup the pk attribute name.
		 */
		char *pk_attr_name = pstrdup(get_relid_attribute_name(restore_descr->orig_relid,
															  restore_descr->orig_pk_attnum[i]));

		restore_descr->orig_pk_attr_names = lappend(restore_descr->orig_pk_attr_names,
													pk_attr_name);
	}
}

static inline char *
StringListToString(List *list, int length, StringInfo buf)
{
	ListCell *scan;
	int i;

	Assert(list != NIL);
	resetStringInfo(buf);

	foreach(scan, list)
	{
		char *str = (char *)lfirst(scan);
		appendStringInfoString(buf, str);

		if (i < (length - 1))
			appendStringInfoString(buf, ", ");
	}

	return buf->data;
}

static inline char *
AttrNumberArrayToString(int *attrnums, int length, StringInfo buf)
{
	int i;

	Assert(attrnums != NULL);
	resetStringInfo(buf);

	for(i = 0; i < length; i++)
	{
		appendStringInfo(buf, "%d", attrnums[i]);

		if (i < (length - 1))
			appendStringInfoString(buf, ", ");
	}

	return buf->data;
}

/*
  table_log_restore_table()

  restore a complete table based on the logging table

  parameter:
  - original table name
  - name of primary key in original table
  - logging table
  - name of primary key in logging table
  - restore table name
  - timestamp for restoring data
  - primary key to restore (only this key will be restored) (optional)
  - restore mode
    0: restore from blank table (default)
       needs a complete logging table
    1: restore from actual table backwards
  - dont create table temporarly
    0: create restore table temporarly (default)
    1: create restore table not temporarly
  return:
    not yet defined
*/
Datum table_log_restore_table(PG_FUNCTION_ARGS)
{
	TableLogRestoreDescr restore_descr;

	/* the primary key in the original table */
	char  *table_orig_pkey;

	/* number columns in log table */
	int  table_log_columns = 0;

	/* the timestamp in past */
	Datum      timestamp = PG_GETARG_DATUM(5);

	/* the single pkey, can be null (then all keys will be restored) */
	char  *search_pkey = "";

	/* the restore method
	   - 0: restore from blank table (default)
	   needs a complete log table!
	   - 1: restore from actual table backwards
	*/
	int            method = 0;
	/* dont create restore table temporarly
	   - 0: create restore table temporarly (default)
	   - 1: dont create restore table temporarly
	*/
	int            not_temporarly = 0;
	int            ret, results, i, number_columns;

    /*
	 * for getting table infos
	 */
	StringInfo     query;

	int            need_search_pkey = 0;          /* does we have a single key to restore? */
	char           *tmp, *timestamp_string, *old_pkey_string = "";
	char           *trigger_mode;
	char           *trigger_tuple;
	char           *trigger_changed;
	SPITupleTable  *spi_tuptable = NULL;          /* for saving query results */
	VarChar        *return_name;

	/* memory for dynamic query */
	StringInfo      d_query;

	/* memory for column names */
	StringInfo      col_query;

	int      col_pkey = 0;

	/*
	 * Some checks first...
	 */
	elog(DEBUG2, "start table_log_restore_table()");

  /* does we have all arguments? */
	if (PG_ARGISNULL(0))
	{
		elog(ERROR, "table_log_restore_table: missing original table name");
	}
	if (PG_ARGISNULL(1))
	{
		table_orig_pkey = NULL;
	}
	if (PG_ARGISNULL(2))
	{
		elog(ERROR, "table_log_restore_table: missing log table name");
	}
	if (PG_ARGISNULL(3))
	{
		elog(ERROR, "table_log_restore_table: missing primary key name for log table");
	}
	if (PG_ARGISNULL(4))
	{
		elog(ERROR, "table_log_restore_table: missing copy table name");
	}
	if (PG_ARGISNULL(5))
	{
		elog(ERROR, "table_log_restore_table: missing timestamp");
	}

	/* first check number arguments to avoid an segfault */
	if (PG_NARGS() >= 7)
	{
		/* if argument is given, check if not null */
		if (!PG_ARGISNULL(6))
		{
			/* yes, fetch it */
			search_pkey = __table_log_varcharout((VarChar *)PG_GETARG_VARCHAR_P(6));

			/* and check, if we have an argument */
			if (strlen(search_pkey) > 0)
			{
				need_search_pkey = 1;
				elog(DEBUG2, "table_log_restore_table: will restore a single key");
			}
		}
	} /* nargs >= 7 */

	/* same procedere here */
	if (PG_NARGS() >= 8)
	{
		if (!PG_ARGISNULL(7))
		{
			method = PG_GETARG_INT32(7);

			if (method > 0)
			{
				method = 1;
			}
			else
			{
				method = 0;
			}
		}
	} /* nargs >= 8 */

	if (method == 1)
		elog(DEBUG2, "table_log_restore_table: will restore from actual state backwards");
	else
		elog(DEBUG2, "table_log_restore_table: will restore from begin forward");

	if (PG_NARGS() >= 9)
	{
		if (!PG_ARGISNULL(8))
		{
			not_temporarly = PG_GETARG_INT32(8);

			if (not_temporarly > 0)
			{
				not_temporarly = 1;
				elog(DEBUG2, "table_log_restore_table: dont create restore table temporarly");
			}
			else
			{
				not_temporarly = 0;
			}
		}
 	} /* nargs >= 9 */

	/* get parameter and set them to the restore descriptor */

	setTableLogRestoreDescr(&restore_descr,
							__table_log_varcharout((VarChar *)PG_GETARG_VARCHAR_P(0)),
							((table_orig_pkey != NULL) ? __table_log_varcharout((VarChar *)PG_GETARG_VARCHAR_P(1)) : NULL),
							__table_log_varcharout((VarChar *)PG_GETARG_VARCHAR_P(2)),
							__table_log_varcharout((VarChar *)PG_GETARG_VARCHAR_P(3)),
							__table_log_varcharout((VarChar *)PG_GETARG_VARCHAR_P(4)));

	/*
	 * Composite PK not supported atm...
	 *
	 * CAUTION:
	 *
	 * The infrastructure to support composite primary keys is there,
	 * but the following old cold still assumes there's only one column
	 * in the PK to consider.
	 */
	if (restore_descr.orig_num_pk_attnums > 1)
		elog(ERROR, "composite primary key not supported");

	/* Connect to SPI manager */
	ret = SPI_connect();

	if (ret != SPI_OK_CONNECT)
	{
		elog(ERROR, "table_log_restore_table: SPI_connect returned %d", ret);
	}

	/* check original table */
	query = makeStringInfo();
	appendStringInfo(query,
					 "SELECT a.attname FROM pg_class c, pg_attribute a WHERE c.oid = %s::regclass AND a.attnum > 0 AND a.attrelid = c.oid ORDER BY a.attnum",
					 do_quote_literal(do_quote_ident(restore_descr.orig_relname)));

	elog(DEBUG3, "query: %s", query->data);

	ret = SPI_exec(query->data, 0);

	if (ret != SPI_OK_SELECT)
	{
		elog(ERROR, "could not check relation: \"%s\"",
			 restore_descr.orig_relname);
	}

	if (SPI_processed <= 0)
	{
		elog(ERROR, "could not check relation: \"%s\"",
			 restore_descr.orig_relname);
	}

	/* check log table */
	if (restore_descr.log_relid == InvalidOid)
	{
		elog(ERROR, "log table \"%s\" does not exist",
			 RESTORE_TABLE_IDENT(restore_descr, log));
	}

	resetStringInfo(query);
	appendStringInfo(query,
					 "SELECT a.attname \
                      FROM pg_class c, pg_attribute a \
                      WHERE c.oid = %u \
                            AND c.relkind IN ('v', 'r') \
                            AND a.attnum > 0 \
                            AND a.attrelid = c.oid	\
                            ORDER BY a.attnum",
					 restore_descr.log_relid);

	elog(DEBUG3, "query: %s", query->data);

	ret = SPI_exec(query->data, 0);

	if (ret != SPI_OK_SELECT)
	{
		elog(ERROR, "could not check relation [1]: %s",
			 RESTORE_TABLE_IDENT(restore_descr, log));
	}

	if (SPI_processed <= 0)
	{
		elog(ERROR, "could not check relation [2]: %s",
			 RESTORE_TABLE_IDENT(restore_descr, log));
	}

	table_log_columns = SPI_processed;

	/* check pkey in log table */
	resetStringInfo(query);
	appendStringInfo(query,
					 "SELECT a.attname \
                      FROM pg_class c, pg_attribute a \
                      WHERE c.oid=%u AND c.relkind IN ('v', 'r') \
                            AND a.attname=%s \
                            AND a.attnum > 0 \
                            AND a.attrelid = c.oid",
					 restore_descr.log_relid,
					 do_quote_literal(restore_descr.pkey_log));

	elog(DEBUG3, "query: %s", query->data);

	ret = SPI_exec(query->data, 0);

	if (ret != SPI_OK_SELECT)
	{
		elog(ERROR, "could not check relation [3]: %s",
			 RESTORE_TABLE_IDENT(restore_descr, log));
	}

	if (SPI_processed == 0)
	{
		elog(ERROR, "could not check relation [4]: %s",
			 RESTORE_TABLE_IDENT(restore_descr, log));
	}

	elog(DEBUG3, "log table: OK (%i columns)", table_log_columns);

	resetStringInfo(query);
	if (restore_descr.use_schema_restore)
	{
		appendStringInfo(query,
						 "SELECT pg_attribute.attname AS a \
                          FROM pg_class, pg_attribute, pg_namespace \
                          WHERE pg_class.relname=%s					\
                             AND pg_attribute.attnum > 0			   \
                             AND pg_attribute.attrelid=pg_class.oid \
                             AND pg_namespace.nspname = %s \
					         AND pg_namespace.oid = pg_class.relnamespace",
						 do_quote_literal(restore_descr.ident_restore.schema),
						 do_quote_literal(restore_descr.ident_restore.relname));

	}
	else
	{
		appendStringInfo(query,
						 "SELECT pg_attribute.attname AS a \
                          FROM pg_class, pg_attribute \
                          WHERE pg_class.relname=%s					\
                             AND pg_attribute.attnum > 0			   \
                             AND pg_attribute.attrelid=pg_class.oid",
						 do_quote_literal(restore_descr.relname_restore));
	}

	elog(DEBUG3, "query: %s", query->data);

	ret = SPI_exec(query->data, 0);

	if (ret != SPI_OK_SELECT)
	{
		elog(ERROR, "could not check relation: %s",
			 RESTORE_TABLE_IDENT(restore_descr, restore));
	}

	if (SPI_processed > 0)
	{
		elog(ERROR, "restore table already exists: %s",
			 RESTORE_TABLE_IDENT(restore_descr, restore));
	}

	elog(DEBUG2, "restore table: OK (doesn't exists)");

	/* now get all columns from original table */
	resetStringInfo(query);
	appendStringInfo(query,
					 "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnum \
                      FROM pg_class c, pg_attribute a \
                      WHERE c.oid = %s::regclass AND a.attnum > 0 AND a.attrelid = c.oid ORDER BY a.attnum",
					 do_quote_literal(do_quote_ident(restore_descr.orig_relname)));

	elog(DEBUG3, "query: %s", query->data);

	ret = SPI_exec(query->data, 0);

	if (ret != SPI_OK_SELECT)
	{
		elog(ERROR, "could not get columns from relation: \"%s\"",
			 restore_descr.orig_relname);
	}

	if (SPI_processed == 0)
	{
		elog(ERROR, "could not check relation: \"%s\"",
			 restore_descr.orig_relname);
	}

	results = SPI_processed;

	/* store number columns for later */
	number_columns = SPI_processed;

	elog(DEBUG2, "number columns: %i", results);

	for (i = 0; i < results; i++)
	{
		/* the column name */
		tmp = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);

		/* now check, if this is the pkey */
		if (strcmp((const char *)tmp,
				   (const char *)list_nth(restore_descr.orig_pk_attr_names, 0)) == 0)
		{
			/* remember the (real) number */
			col_pkey = i + 1;
		}
	}

	/* check if we have found the pkey */
	if (col_pkey == 0)
	{
		elog(ERROR, "cannot find pkey (%s) in table \"%s\"",
			 (char *)list_nth(restore_descr.orig_pk_attr_names, 0),
			 restore_descr.orig_relname);
	}

	/* allocate memory for string */
	col_query = makeStringInfo();

	for (i = 0; i < results; i++)
	{
		if (i > 0)
			appendStringInfo(col_query, ", ");

		appendStringInfo(col_query, "%s",
						 do_quote_ident(SPI_getvalue(SPI_tuptable->vals[i],
													 SPI_tuptable->tupdesc, 1)));
	}

	/* create restore table */
	elog(DEBUG2, "string for columns: %s", col_query->data);
	elog(DEBUG2, "create restore table: %s",
		 RESTORE_TABLE_IDENT(restore_descr, restore));
	resetStringInfo(query);
	appendStringInfo(query, "SELECT * INTO ");

	/* per default create a temporary table */
	if (not_temporarly == 0)
	{
		appendStringInfo(query, "TEMPORARY ");
	}

	/* from which table? */
	appendStringInfo(query, "TABLE %s FROM %s ",
					 RESTORE_TABLE_IDENT(restore_descr, restore),
					 quote_identifier(restore_descr.orig_relname));

	if (need_search_pkey == 1)
	{
		/* only extract a specific key */
		appendStringInfo(query, "WHERE %s = %s ",
						 do_quote_ident(list_nth(restore_descr.orig_pk_attr_names, 0)),
						 do_quote_literal(search_pkey));
	}

	if (method == 0)
	{
		/* restore from begin (blank table) */
		appendStringInfo(query, "LIMIT 0");
	}

	elog(DEBUG3, "query: %s", query->data);

	ret = SPI_exec(query->data, 0);

	if (ret != SPI_OK_SELINTO)
	{
		elog(ERROR, "could not check relation: %s",
			 RESTORE_TABLE_IDENT(restore_descr, restore));
	}

	if (method == 1)
		elog(DEBUG2, "%i rows copied", SPI_processed);

	/* get timestamp as string */
	timestamp_string = DatumGetCString(DirectFunctionCall1(timestamptz_out, timestamp));

	if (method == 0)
		elog(DEBUG2, "need logs from start to timestamp: %s", timestamp_string);
	else
		elog(DEBUG2, "need logs from end to timestamp: %s", timestamp_string);

	/* now build query for getting logs */
	elog(DEBUG2, "build query for getting logs");

	/* allocate memory for string and build query */
	d_query = makeStringInfo();

	elog(DEBUG2, "using log table %s",
		 RESTORE_TABLE_IDENT(restore_descr, log));

	appendStringInfo(d_query,
					 "SELECT %s, trigger_mode, trigger_tuple, trigger_changed FROM %s WHERE ",
					 col_query->data,
					 RESTORE_TABLE_IDENT(restore_descr, log));

	if (method == 0)
	{
		/* from start to timestamp */
		appendStringInfo(d_query, "trigger_changed <= %s",
						 do_quote_literal(timestamp_string));
	}
	else
	{
		/* from now() backwards to timestamp */
		appendStringInfo(d_query, "trigger_changed >= %s ",
						 do_quote_literal(timestamp_string));
	}

	if (need_search_pkey == 1)
	{
		appendStringInfo(d_query, "AND %s = %s ",
						 do_quote_ident(list_nth(restore_descr.orig_pk_attr_names, 0)),
						 do_quote_literal(search_pkey));
	}

	if (method == 0)
	{
		appendStringInfo(d_query, "ORDER BY %s ASC",
						 do_quote_ident(restore_descr.pkey_log));
	}
	else
	{
		appendStringInfo(d_query, "ORDER BY %s DESC",
						 do_quote_ident(restore_descr.pkey_log));
	}

	elog(DEBUG3, "query: %s", d_query->data);

	ret = SPI_exec(d_query->data, 0);

	if (ret != SPI_OK_SELECT)
	{
		elog(ERROR, "could not get log data from table: %s",
			 RESTORE_TABLE_IDENT(restore_descr, log));
	}

	elog(DEBUG2, "number log entries to restore: %i", SPI_processed);

	results = SPI_processed;
	/* save results */
	spi_tuptable = SPI_tuptable;

	/* go through all results */
	for (i = 0; i < results; i++)
	{

		/* get tuple data */
		trigger_mode = SPI_getvalue(spi_tuptable->vals[i], spi_tuptable->tupdesc, number_columns + 1);
		trigger_tuple = SPI_getvalue(spi_tuptable->vals[i], spi_tuptable->tupdesc, number_columns + 2);
		trigger_changed = SPI_getvalue(spi_tuptable->vals[i], spi_tuptable->tupdesc, number_columns + 3);

		/* check for update tuples we doesnt need */
		if (strcmp((const char *)trigger_mode, (const char *)"UPDATE") == 0)
		{
			if (method == 0 && strcmp((const char *)trigger_tuple, (const char *)"old") == 0)
			{
				/* we need the old value of the pkey for the update */
				old_pkey_string = SPI_getvalue(spi_tuptable->vals[i], spi_tuptable->tupdesc, col_pkey);
				elog(DEBUG2, "tuple old pkey: %s", old_pkey_string);

				/* then skip this tuple */
				continue;
			}

			if (method == 1 && strcmp((const char *)trigger_tuple, (const char *)"new") == 0)
			{
				/* we need the old value of the pkey for the update */
				old_pkey_string = SPI_getvalue(spi_tuptable->vals[i], spi_tuptable->tupdesc, col_pkey);
				elog(DEBUG2, "tuple: old pkey: %s", old_pkey_string);

				/* then skip this tuple */
				continue;
			}
		}

		if (method == 0)
		{
			/* roll forward */
			elog(DEBUG2, "tuple: %s  %s  %s", trigger_mode, trigger_tuple, trigger_changed);

			if (strcmp((const char *)trigger_mode, (const char *)"INSERT") == 0)
			{
				__table_log_restore_table_insert(spi_tuptable,
												 (char *)RESTORE_TABLE_IDENT(restore_descr, restore),
												 list_nth(restore_descr.orig_pk_attr_names, 0),
												 col_query->data,
												 col_pkey,
												 number_columns,
												 i);
			}
			else if (strcmp((const char *)trigger_mode, (const char *)"UPDATE") == 0)
			{
				__table_log_restore_table_update(spi_tuptable,
												 (char *)RESTORE_TABLE_IDENT(restore_descr, restore),
												 list_nth(restore_descr.orig_pk_attr_names, 0),
												 col_query->data,
												 col_pkey,
												 number_columns,
												 i,
												 old_pkey_string);
			}
			else if (strcmp((const char *)trigger_mode, (const char *)"DELETE") == 0)
			{
				__table_log_restore_table_delete(spi_tuptable,
												 (char *)RESTORE_TABLE_IDENT(restore_descr, restore),
												 list_nth(restore_descr.orig_pk_attr_names, 0),
												 col_query->data,
												 col_pkey,
												 number_columns,
												 i);
			}
			else
			{
				elog(ERROR, "unknown trigger_mode: %s", trigger_mode);
			}

		}
		else
		{
			/* roll back */
			char rb_mode[10]; /* reverse the method */

			if (strcmp((const char *)trigger_mode, (const char *)"INSERT") == 0)
			{
				sprintf(rb_mode, "DELETE");
			}
			else if (strcmp((const char *)trigger_mode, (const char *)"UPDATE") == 0)
			{
				sprintf(rb_mode, "UPDATE");
			}
			else if (strcmp((const char *)trigger_mode, (const char *)"DELETE") == 0)
			{
				sprintf(rb_mode, "INSERT");
			}
			else
			{
				elog(ERROR, "unknown trigger_mode: %s", trigger_mode);
			}

			elog(DEBUG2, "tuple: %s  %s  %s", rb_mode, trigger_tuple, trigger_changed);

			if (strcmp((const char *)trigger_mode, (const char *)"INSERT") == 0)
			{
				__table_log_restore_table_delete(spi_tuptable,
												 (char *)RESTORE_TABLE_IDENT(restore_descr, restore),
												 list_nth(restore_descr.orig_pk_attr_names, 0),
												 col_query->data,
												 col_pkey,
												 number_columns,
												 i);
			}
			else if (strcmp((const char *)trigger_mode, (const char *)"UPDATE") == 0)
			{
				__table_log_restore_table_update(spi_tuptable,
												 (char *)RESTORE_TABLE_IDENT(restore_descr, restore),
												 list_nth(restore_descr.orig_pk_attr_names, 0),
												 col_query->data,
												 col_pkey,
												 number_columns,
												 i,
												 old_pkey_string);
			}
			else if (strcmp((const char *)trigger_mode, (const char *)"DELETE") == 0)
			{
				__table_log_restore_table_insert(spi_tuptable,
												 (char *)RESTORE_TABLE_IDENT(restore_descr, restore),
												 list_nth(restore_descr.orig_pk_attr_names, 0),
												 col_query->data,
												 col_pkey,
												 number_columns,
												 i);
			}
		}
	}

	/* close SPI connection */
	SPI_finish();

	elog(DEBUG2, "table_log_restore_table() done, results in: %s",
		 RESTORE_TABLE_IDENT(restore_descr, restore));

	/* convert string to VarChar for result */
	return_name = DatumGetVarCharP(DirectFunctionCall2(varcharin,
													   CStringGetDatum(RESTORE_TABLE_IDENT(restore_descr,
																						   restore)),
													   Int32GetDatum(strlen(RESTORE_TABLE_IDENT(restore_descr,
																								restore))
																	 + VARHDRSZ)));

	/* and return the name of the restore table */
	PG_RETURN_VARCHAR_P(return_name);
}

static void __table_log_restore_table_insert(SPITupleTable *spi_tuptable,
											 char *table_restore,
											 char *table_orig_pkey,
											 char *col_query_start,
											 int col_pkey,
											 int number_columns,
											 int i) {
	int            j;
	int            ret;
	char          *tmp;

	/* memory for dynamic query */
	StringInfo     d_query;

	d_query = makeStringInfo();

	/* build query */
	appendStringInfo(d_query, "INSERT INTO %s (%s) VALUES (",
					 table_restore,
					 col_query_start);

	for (j = 1; j <= number_columns; j++)
	{
		if (j > 1)
		{
			appendStringInfoString(d_query, ", ");
		}

		tmp = SPI_getvalue(spi_tuptable->vals[i], spi_tuptable->tupdesc, j);

		if (tmp == NULL)
		{
			appendStringInfoString(d_query, "NULL");
		}
		else
		{
			appendStringInfoString(d_query, do_quote_literal(tmp));
		}
	}

	appendStringInfoString(d_query, ")");
	elog(DEBUG3, "query: %s", d_query->data);

	ret = SPI_exec(d_query->data, 0);

	if (ret != SPI_OK_INSERT) {
		elog(ERROR, "could not insert data into: %s", table_restore);
	}

	/* done */
}

static void __table_log_restore_table_update(SPITupleTable *spi_tuptable,
											 char *table_restore,
											 char *table_orig_pkey,
											 char *col_query_start,
											 int col_pkey,
											 int number_columns,
											 int i,
											 char *old_pkey_string) {
	int   j;
	int   ret;
	char *tmp;
	char *tmp2;

	/* memory for dynamic query */
	StringInfo d_query;

	d_query = makeStringInfo();

	/* build query */
	appendStringInfo(d_query, "UPDATE %s SET ",
					 table_restore);

	for (j = 1; j <= number_columns; j++)
	{
		if (j > 1)
		{
			appendStringInfoString(d_query, ", ");
		}

		tmp = SPI_getvalue(spi_tuptable->vals[i], spi_tuptable->tupdesc, j);
		tmp2 = SPI_fname(spi_tuptable->tupdesc, j);

		if (tmp == NULL)
		{
			appendStringInfo(d_query, "%s=NULL", do_quote_ident(tmp2));
		}
		else
		{
			appendStringInfo(d_query, "%s=%s",
							 do_quote_ident(tmp2), do_quote_literal(tmp));
		}
	}

	appendStringInfo(d_query,
			 " WHERE %s=%s",
			 do_quote_ident(table_orig_pkey),
			 do_quote_literal(old_pkey_string));

	elog(DEBUG3, "query: %s", d_query->data);

	ret = SPI_exec(d_query->data, 0);

  if (ret != SPI_OK_UPDATE)
  {
	  elog(ERROR, "could not update data in: %s", table_restore);
  }

  /* done */
}

static void __table_log_restore_table_delete(SPITupleTable *spi_tuptable,
											 char *table_restore,
											 char *table_orig_pkey,
											 char *col_query_start,
											 int col_pkey,
											 int number_columns,
											 int i) {
	int   ret;
	char *tmp;

	/* memory for dynamic query */
	StringInfo d_query;

	/* get the size of value */
	tmp = SPI_getvalue(spi_tuptable->vals[i], spi_tuptable->tupdesc, col_pkey);

	if (tmp == NULL)
	{
		elog(ERROR, "pkey cannot be NULL");
	}

	/* initalize StringInfo structure */
	d_query = makeStringInfo();

	/* build query */
	appendStringInfo(d_query,
					 "DELETE FROM %s WHERE %s=%s",
					 table_restore,
					 do_quote_ident(table_orig_pkey),
					 do_quote_literal(tmp));

	elog(DEBUG3, "query: %s", d_query->data);

	ret = SPI_exec(d_query->data, 0);

	if (ret != SPI_OK_DELETE)
	{
		elog(ERROR, "could not delete data from: %s", table_restore);
	}

  /* done */
}

/*
 * MULTIBYTE dependant internal functions follow
 *
 */
/* from src/backend/utils/adt/quote.c and slightly modified */

#ifndef MULTIBYTE

/* Return a properly quoted identifier */
static char * do_quote_ident(char *iptr)
{
	char    *result;
	char    *result_return;
	int     len;

	len           = strlen(iptr);
	result        = (char *) palloc(len * 2 + 3);
	result_return = result;
	*result++     = '"';

	while (len-- > 0)
	{
		if (*iptr == '"')
		{
			*result++ = '"';
		}

		if (*iptr == '\\')
		{
			/* just add a backslash, the ' will be follow */
			*result++ = '\\';
		}
		*result++ = *iptr++;
	}

	*result++ = '"';
	*result++ = '\0';

	return result_return;
}

/* Return a properly quoted literal value */
static char * do_quote_literal(char *lptr)
{
	char    *result;
	char    *result_return;
	int     len;

	len           = strlen(lptr);
	result        = (char *) palloc(len * 2 + 3);
	result_return = result;
	*result++     = '\'';

	while (len-- > 0)
	{
		if (*lptr == '\'')
		{
			*result++ = '\\';
		}

		if (*lptr == '\\')
		{
			/* just add a backslash, the ' will be follow */
			*result++ = '\\';
		}
		*result++ = *lptr++;
	}

	*result++ = '\'';
	*result++ = '\0';

	return result_return;
}

#else

/* Return a properly quoted identifier (MULTIBYTE version) */
static char * do_quote_ident(char *iptr)
{
	char    *result;
	char    *result_return;
	int     len;
	int     wl;

	len           = strlen(iptr);
	result        = (char *) palloc(len * 2 + 3);
	result_return = result;
	*result++     = '"';

	while (len > 0)
	{
		if ((wl = pg_mblen(iptr)) != 1)
		{
			len -= wl;

			while (wl-- > 0)
			{
				*result++ = *iptr++;
			}
			continue;
		}

		if (*iptr == '"')
		{
			*result++ = '"';
		}

		if (*iptr == '\\')
		{
			/* just add a backslash, the ' will be follow */
			*result++ = '\\';
		}
		*result++ = *iptr++;

		len--;
	}

	*result++ = '"';
	*result++ = '\0';

	return result_return;
}

/* Return a properly quoted literal value (MULTIBYTE version) */
static char * do_quote_literal(char *lptr)
{
	char    *result;
	char    *result_return;
	int     len;
	int     wl;

	len           = strlen(lptr);
	result        = (char *) palloc(len * 2 + 3);
	result_return = result;
	*result++     = '\'';

	while (len > 0)
	{
		if ((wl = pg_mblen(lptr)) != 1)
		{
			len -= wl;

			while (wl-- > 0)
			{
				*result++ = *lptr++;
			}
			continue;
		}

		if (*lptr == '\'')
		{
			*result++ = '\\';
		}

		if (*lptr == '\\')
		{
			/* just add a backslash, the ' will be follow */
			*result++ = '\\';
		}
		*result++ = *lptr++;

		len--;
	}
	*result++ = '\'';
	*result++ = '\0';

	return result_return;
}

#endif /* MULTIBYTE */

static char * __table_log_varcharout(VarChar *s)
{
	char *result;
	int32 len;

	/* copy and add null term */
	len    = VARSIZE(s) - VARHDRSZ;
	result = palloc(len + 1);
	memcpy(result, VARDATA(s), len);
	result[len] = '\0';

#ifdef CYR_RECODE
	convertstr(result, len, 1);
#endif

	return result;
}
