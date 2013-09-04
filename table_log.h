/*
 * table_log () -- log changes to another table
 *
 *
 * see README.table_log for details
 *
 *
 * written by Andreas ' ads' Scherbaum (ads@pgug.de)
 * adapted for PostgreSQL 9.1+ by Bernd Helmle (bernd.helmle@credativ.de)
 *
 */
#define MAX_TABLE_LOG_PARTITIONS 2

/*
 * Selected log table identifier, relies
 * on the current selected partition via table_log.active_partition
 */
typedef int TableLogPartitionId;
