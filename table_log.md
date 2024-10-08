table_log for PostgreSQL
========================

Table of contents:

1. Info
2. License
3. Installation
4. Documentation
   4.1. Manual table log and trigger creation
   4.2. Restore table data
5. Hints
   5.1. Security tips
6. Bugs
7. Todo
8. Changes
9. Contact



# 1. Info

table_log is a set of functions to log changes on a table in PostgreSQL
and to restore the state of the table or a specific row on any time
in the past.

For now it contains 2 functions:

* `table_log()` -- log changes to another table
* `table_log_restore_table()` -- restore a table or a specific column

NOTE: you can only restore a table where the original table and the
logging table has a primary key!

This means: you can log everything, but for the restore function you must
have a primary key on the original table (and of course a different pkey
on the log table).

In the beginning (for table_log()) i have used some code from noup.c
(you will find this in the contrib directory), but there should be
no code left from noup, since i rewrote everything during the development.
In fact, it makes no difference since both software is licensed with
the BSD style licence which is used by PostgreSQL.



# 2. License

Copyright (c) 2002-2007  Andreas Scherbaum

Basically it's the same as the PostgreSQL license (BSD license).

Permission to use, copy, modify, and distribute this software and
its documentation for any purpose and without fee is hereby
granted, provided that the above copyright notice appear in all
copies and that both that copyright notice and this permission
notice appear in supporting documentation, and that the name of the
author not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission. The author makes no representations about the
suitability of this software for any purpose. It is provided "as
is" without express or implied warranty.



# 3. Installation

Build table_log:

```
make USE_PGXS=1
(and as root)
make USE_PGXS=1 install
```

If you are using a version >= 9.1 of PostgreSQL, you can use the new
regression tests to verify the working state of the module:

```
USE_PGXS=1 make installcheck
```

Since the regression checks require the extension infrastructure, this won't work
on version below 9.1.

## 3.1 Pre-9.1 installation procedure

The pg_config tool must be in your $PATH for installation and
you must have the PostgreSQL development packages installed.
Of course, the usual development tools like make, gcc ect. should
also be there ;-)

After this you have to create some new functions:
- in every database you want to use this functions
- if you add this functions to template1, they will be copied to every
  new database
- for older pg versions <= 7.2 change "RETURNS trigger" to "RETURNS opaque",
  but versions < 7.4 are no longer supported

```
CREATE FUNCTION "table_log_basic" ()
    RETURNS trigger
    AS '$libdir/table_log', 'table_log' LANGUAGE 'C';
CREATE FUNCTION "table_log" ()
    RETURNS trigger
    AS '$libdir/table_log', 'table_log' LANGUAGE 'C';
CREATE FUNCTION "table_log_restore_table" (VARCHAR, VARCHAR, CHAR, CHAR, CHAR, TIMESTAMPTZ, CHAR, INT, INT)
    RETURNS VARCHAR
    AS '$libdir/table_log', 'table_log_restore_table' LANGUAGE 'C';
CREATE FUNCTION "table_log_restore_table" (VARCHAR, VARCHAR, CHAR, CHAR, CHAR, TIMESTAMPTZ, CHAR, INT)
    RETURNS VARCHAR
    AS '$libdir/table_log', 'table_log_restore_table' LANGUAGE 'C';
CREATE FUNCTION "table_log_restore_table" (VARCHAR, VARCHAR, CHAR, CHAR, CHAR, TIMESTAMPTZ, CHAR)
    RETURNS VARCHAR
    AS '$libdir/table_log', 'table_log_restore_table' LANGUAGE 'C';
CREATE FUNCTION "table_log_restore_table" (VARCHAR, VARCHAR, CHAR, CHAR, CHAR, TIMESTAMPTZ)
    RETURNS VARCHAR
    AS '$libdir/table_log', 'table_log_restore_table' LANGUAGE 'C';
```

There's also a script available in PATH_TO_YOUR_PGSQL/share/contrib, which contains all
required function definitions plus some regression tests, for example:

```
psql -f PATH_TO_YOUR_PGSQL/share/contrib/table_log.sql
```

Install table_log_init() by running `psql < table_log_init.sql`.
This function does all the work which is necessary for creating a
logging table.

NOTE:

Currently the Makefile doesn't distinguish between extension and contrib
installation procedures and will install table_log--x.x.sql and control files
in pre-9.1 versions nevertheless.
This might get solved in the future, but don't use those scripts in pre-9.1
installations.

## 3.2 Installation procedure with versions >= 9.1

Starting with PostgreSQL 9.1 you should use the new extension
infrastructure. table_log supports this starting with version 0.5. After
compiling and installing the module, all you need to do is to connect
to the target database and issue the following command:

```
CREATE EXTENSION table_log;
```

This will install all functions including table_log_init() into the
public schema.

NOTE: The old contrib scripts are going to be installed, still. We currently don't have
distinguished the installation procedure version-wise within the Makefile, so don't
get confused if you still find the old table_log.sql scripts in share/extension
directories.

## 3.3 Upgrading from earlier installations to the new extension infrastructure

If you are upgrading from an earlier pre-9.1 installation, and you want to use
the new extension infrastructure, you could use the unpackaged creation
procedure to migrate an existing table_log installation within a database into
an extension. To accomplish this, you need to execute the following command
in the new 9.1+ database (of course, after installing the new version of table_log):

```
CREATE EXTENSION table_log FROM unpackaged;
```

# 4. Documentation

The entire log table and trigger creation can be done using the
table_log_init(ncols, ...) function. The parameter ncols decides how many
extra columns will be added to the created log table, it can be 3, 4 or 5.
The extra columns are described in chapter 4.1.

The function can be used with the following parameters:

  table_log_init(ncols, tablename):
    create the log table as tablename_log
    
  table_log_init(ncols, tablename, logschema):
    create the log table with the same name as the original table, but in
    the schema logschema.

  table_log_init(ncols, tableschema, tablename, logschema, logname):
    log the changes in table tableschema.tablename into the log table
    logschema.logname.

  table_log_init(ncols, tableschema, tablename, logschema, logname, partition_mode, basic_mode, log_actions):
    log the changes in table tableschema.tablename into the log table
    logschema.logname. The parameter partition_mode can be SINGLE or PARTITION, which
    creates two log tables *_0 and *_1 which can be switched by setting
    table_log.active_partition to the corresponding partition id 1 or 2.
    basic_mode defines wether we use full logging mode or basic mode. When set to TRUE,
    table_log_basic() will be used internally which suppresses logging of NEW values
    by UPDATE actions.
    log_actions is a TEXT[] array which specifies a list of either INSERT, DELETE or UPDATE
    or any combinations from them to tell when a table_log trigger should be fired.

    NOTE:

    When using basic_mode and/or log_actions without all actions, you won't be
    able to use table_log_restore_table() anymore.

    When calling table_log_init(), you can omit logname in any case. The function
    will then generate a log tablename from the given source tablename and a
    `_log` string appended.

## 4.1. Manual table log and trigger creation

Create an trigger on a table with log table name as argument.
If no table name is given, the actual table name
plus `_log` will be used by table_log.

Example:

```
CREATE TRIGGER test_log_chg AFTER UPDATE OR INSERT OR DELETE ON test_table FOR EACH ROW
               EXECUTE PROCEDURE table_log();
^^^^^ 'test_table_log' will be used to log changes

CREATE TRIGGER test_log_chg AFTER UPDATE OR INSERT OR DELETE ON test_table FOR EACH ROW
               EXECUTE PROCEDURE table_log('log_table');
^^^^^ 'log_table' will be used to log changes
```


The log table needs exact the same columns as the original table
(but without any constraints)
plus three, four or five extra columns:

```
trigger_mode VARCHAR(10)
trigger_tuple VARCHAR(5)
trigger_changed TIMESTAMPTZ
trigger_user VARCHAR(32)     -- optional

trigger_mode contains 'INSERT', 'UPDATE' or 'DELETE'
trigger_tuple contains 'old' or 'new'
trigger_changed is the actual timestamp inside the trancaction
   (or maybe i should use here the actual system timestamp?)
trigger_user contains the session user name (the one who connected to the
   database, this must not be the actual one)
```

On INSERT, a log entry with the 'new' tuple will be written.
On DELETE, a log entry with the 'old' tuple will be written.
On UPDATE, a log entry with the old tuple and a log entry with
the new tuple will be written.

A fourth column is possible on the log table:
trigger_id BIGINT
contains an unique id for sorting table log entries

NOTE: for the restore function you must have this 4. column!

  Q: Why do i need this column?
  A: Because we have to sort the logs to get them back in correct order.
     Hint: if you are sure, you don't have an OID wrapover yet, you can
           use the OID column as unique id (but if you have an OID wrapover
           later, the new OIDs doesn't follow a linear scheme,
           see VACUUM documentation)

A fifth column is possible on the log table:
trigger_user VARCHAR(32)
contains the username from the user who originally opened the database connection

  Q: Why not the actual user?
  A: If someone changed the actual user with a setuid function,
     you always know the original username

  Q: Why 32 bytes?
  A: The function doesn't uses 32 bytes but instead NAMEDATALEN defined
     at compile time (which defaults to 32 bytes)

  Q: May i skip the log table name?
  A: No, because Pg then thinks, the '1' is the first parameter and will fail
     to use '1' as logging table
     This is an backwards compatibility issue, sorry for this.


For backward compatibility table_log() works with 3, 4 or 5 extra
columns, but you should use the 4 or 5 column version everytimes.

A good method to create the log table is to use the existing table:

```
-- create the table without data
SELECT * INTO test_log FROM test LIMIT 0;
ALTER TABLE test_log ADD COLUMN trigger_mode VARCHAR(10);
ALTER TABLE test_log ADD COLUMN trigger_tuple VARCHAR(5);
ALTER TABLE test_log ADD COLUMN trigger_changed TIMESTAMPTZ;
-- now activate the history function
CREATE TRIGGER test_log_chg AFTER UPDATE OR INSERT OR DELETE ON test FOR EACH ROW
               EXECUTE PROCEDURE table_log();

-- or the 4 column method:
SELECT * INTO test_log FROM test LIMIT 0;
ALTER TABLE test_log ADD COLUMN trigger_mode VARCHAR(10);
ALTER TABLE test_log ADD COLUMN trigger_tuple VARCHAR(5);
ALTER TABLE test_log ADD COLUMN trigger_changed TIMESTAMPTZ;
ALTER TABLE test_log ADD COLUMN trigger_id BIGINT;
CREATE SEQUENCE test_log_id;
SELECT SETVAL('test_log_id', 1, FALSE);
ALTER TABLE test_log ALTER COLUMN trigger_id SET DEFAULT NEXTVAL('test_log_id');
-- now activate the history function
CREATE TRIGGER test_log_chg AFTER UPDATE OR INSERT OR DELETE ON test FOR EACH ROW
               EXECUTE PROCEDURE table_log();

-- or the 5 column method (with user name in 'trigger_user'):
SELECT * INTO test_log FROM test LIMIT 0;
ALTER TABLE test_log ADD COLUMN trigger_mode VARCHAR(10);
ALTER TABLE test_log ADD COLUMN trigger_tuple VARCHAR(5);
ALTER TABLE test_log ADD COLUMN trigger_changed TIMESTAMPTZ;
ALTER TABLE test_log ADD COLUMN trigger_user VARCHAR(32);
ALTER TABLE test_log ADD COLUMN trigger_id BIGINT;
CREATE SEQUENCE test_log_id;
SELECT SETVAL('test_log_id', 1, FALSE);
ALTER TABLE test_log ALTER COLUMN trigger_id SET DEFAULT NEXTVAL('test_log_id');
-- now activate the history function
-- you have to give the log table name!
CREATE TRIGGER test_log_chg AFTER UPDATE OR INSERT OR DELETE ON test FOR EACH ROW
               EXECUTE PROCEDURE table_log('test_log', 1);
```

See table_log.sql for a demo



## 4.2. Restore table data

Now insert, update and delete some data in table 'test'.
After this, you may want to restore your table data:

```
SELECT table_log_restore_table(<original table name>,
                               <original table primary key>,
                               <log table name>,
                               <log table primary key>,
                               <restore table name>,
                               <timestamp>,
                               <primary key to restore>,
                               <restore method: 0/1>,
                               <dont create temporary table: 0/1>);
```

The parameter list means:

- original table name: string
  The name of the original table (test in your example above)
- original table primary key: string
  The primary key name of the original table
- log table name: string
  The name of the logging table
- log table primary key: string
  The primary key of the logging table (trigger_id in your example above)
  Note: this cannot be the same as the original table pkey!
- restore table name: string
  The name for the restore table
  Note: this table must not exist!
  Also see <dont create temporary table>
- timestamp: timestamp
  The timestamp in past
  Note: if you give a timestamp where no logging data exists,
        absolutly nothing will happen. But see <restore method>
- primary key to restore: string (or NULL)
  If you want to restore only a single primary key, name it here.
  Then only data for this pkey will be searched and restored
  Note: this parameter is optional and defaults to NULL (restore all pkeys)
        you can say NULL here, if you want to skip this parameter
- restore method: 0/1 (or NULL)
  0 means: first create the restore table and then restore forward from the
           beginning of the log table
  1 means: first create the log table and copy the actual content of the
           original table into the log table, then restore backwards
  Note: this can speed up things, if you know, that your timestamp point
        is near the end or the beginning
  Note: this parameter is optional and defaults to NULL (= 0)
- dont create temporary table: 0/1 (or NULL)
  Normal the restore table will be created temporarly, this means, the table
  is only available inside your session and will be deleted, if your
  session (session means connection, not transaction) is closed
  This parameter allows you to create a normal table instead
  Note: if you want to use the restore function sometimes inside a session
        and you want to use the same restore table name again, you have to
        drop the restore table or the restore function will blame you
  Note: this parameter is optional and defaults to NULL (= 0)



# 5. Hints

- an index on the log table primary key (trigger_id) and the trigger_changed
  column will speed up things
- You can find another nice explanation in my blog:
  http://ads.wars-nicht.de/blog/archives/100-Log-Table-Changes-in-PostgreSQL-with-tablelog.html



## 5.1. Security Tips

- You can create the table_log functions with 'SECURITY DEFINER' privileges
  and revoke the permissions for the logging table for any normal user.
  This allows you to have an audit which cannot be modified by users who
  are accessing the data table.
- Existing functions can be modified with:
  ALTER FUNCTION table_log() SECURITY DEFINER;



# 6. Bugs

- nothing known, but tell me, if you find one



# 7. Todo

- table_log_show_column()
  allows select of previous state (possible with PostgreSQL 7.3 and higher)
    see Table Function API
- is it binary safe? (\000)
- do not only check the number columns in both tables,
  really check the names of the columns



# 8. Changes:

- 2002-04-06: Andreas 'ads' Scherbaum (ads@ufp.de)
  first release

- 2002-04-25: Steve Head (smhf@onthe.net.au)
  there was a bug with NULL values, thanks to
  Steve Head for reporting this.

- 2002-04-25: Andreas 'ads' Scherbaum (ads@ufp.de)
  now using version numbers (0.0.5 for this release)

- 2002-09-09: Andreas 'ads' Scherbaum (ads@ufp.de)
  fix bug in calculating log table name
  release 0.0.6

- 2003-03-22: Andreas 'ads' Scherbaum (ads@ufp.de)
  fix some error messages (old name from 'noup' renamed to 'table_log')
  one additional check that the trigger is fired after
  release 0.0.7

- 2003-03-23: Andreas 'ads' Scherbaum (ads@ufp.de)
  create a second Makefile for installing from source
  release 0.1.0

- 2003-04-20: Andreas 'ads' Scherbaum (ads@ufp.de)
  change Makefile.nocontrib to Linux only and make a comment about
  installation informations for other platforms
  (its too difficult to have all install options here,
   i dont have the ability to test all platforms)

- 2003-06-12: Andreas 'ads' Scherbaum (ads@ufp.de)
  update documentation (thanks to Erik Thiele <erik@thiele-hydraulik.de>
  for pointing this out)

- 2003-06-13: Andreas 'ads' Scherbaum (ads@ufp.de)
  - release 0.2.0
  now allow 3 or 4 columns
  update documentation about trigger_id column

- 2003-06-13: Andreas 'ads' Scherbaum (ads@ufp.de)
  - release 0.2.1
  add debugging (activate TABLE_LOG_DEBUG in head of table_log.c and recompile)

- 2003-11-27: Andreas 'ads' Scherbaum (ads@ufp.de)
  - release 0.3.0
  add function for restoring table from log
  cleanup source
  add more debugging

- 2003-12-11: Andreas 'ads' Scherbaum (ads@ufp.de)
  - release 0.3.1
  add session_user to log table on request
  thanks to iago@patela.org.uk for the feature request
  fix a minor bug in returning the table name

- 2005-01-14: Andreas 'ads' Scherbaum (ads@wars-nicht.de)
  - release 0.4.0
  ignore dropped columns on tables (this may cause errors, if you
  restore or use older backups)
  change email address, the old one does no longer work
  
- 2005-01-24: Andreas 'ads' Scherbaum (ads@wars-nicht.de)
  - release 0.4.1
  there seems to be an problem with session_user
  
- 2005-04-22: Kim Hansen <kimhanse@gmail.com>
  - release 0.4.2
  added table_log_init()
  added schema support

- 2006-08-30: Andreas 'ads' Scherbaum (ads@wars-nicht.de)
  - release 0.4.3
  drop support for < 7.4 (return type is TRIGGER now)
  fix bug with dropped columns

- 2007-05-18: Andreas 'ads' Scherbaum (ads@pgug.de)
  - release 0.4.4
  compatibility issues with 8.2.x
  some small fixes in table_log.sql.in
  remove some warnings
  docu cleanups
  Thanks to Michael Graziano <mgraziano@invision.net> for pointing
    out the 8.2 fix
  Thanks to Alexander Wirt <formorer@formorer.de> for Debian packaging
  Thanks to Devrim GÜNDÜZ <devrim@CommandPrompt.com> for RPM packaging


# 9. Contact

The project is hosted at http://pgfoundry.org/projects/tablelog/

If you have any hints, changes or improvements, please contact me.

my gpg key:
pub  1024D/4813B5FE 2000-09-29 Andreas Scherbaum <ads@wars-nicht.de>
     Key fingerprint = 9F67 73D3 43AA B30E CA8F  56E5 3002 8D24 4813 B5FE
