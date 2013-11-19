CREATE EXTENSION table_log;
SET client_min_messages TO warning;

-- drop old trigger
DROP TRIGGER test_log_chg ON test; -- ignore any error

-- create demo table
DROP TABLE test; -- ignore any error
CREATE TABLE test (
  id                    INT                 NOT NULL
                                            PRIMARY KEY,
  name                  VARCHAR(20)         NOT NULL
);

-- create the table without data from demo table
DROP TABLE test_log; -- ignore any error
SELECT * INTO test_log FROM test LIMIT 0;
ALTER TABLE test_log ADD COLUMN trigger_mode VARCHAR(10);
ALTER TABLE test_log ADD COLUMN trigger_tuple VARCHAR(5);
ALTER TABLE test_log ADD COLUMN trigger_changed TIMESTAMPTZ;
ALTER TABLE test_log ADD COLUMN trigger_id BIGINT;
CREATE SEQUENCE test_log_id;
SELECT SETVAL('test_log_id', 1, FALSE);
ALTER TABLE test_log ALTER COLUMN trigger_id SET DEFAULT NEXTVAL('test_log_id');

-- create trigger
CREATE TRIGGER test_log_chg AFTER UPDATE OR INSERT OR DELETE ON test FOR EACH ROW
               EXECUTE PROCEDURE table_log();

-- test trigger
INSERT INTO test VALUES (1, 'name');
SELECT id, name FROM test;
SELECT id, name, trigger_mode, trigger_tuple, trigger_id FROM test_log;
UPDATE test SET name='other name' WHERE id=1;
SELECT id, name FROM test;
SELECT id, name, trigger_mode, trigger_tuple, trigger_id FROM test_log;

-- create restore table
SELECT table_log_restore_table('test', 'id', 'test_log', 'trigger_id', 'test_recover', NOW());
SELECT id, name FROM test_recover;

DROP TABLE test;
DROP TABLE test_log;
DROP TABLE test_recover;

-- test table_log_init with all arguments
-- trigger_user and trigger_changed might differ, so ignore it

SET client_min_messages TO warning;

CREATE TABLE test(id integer, name text);
SELECT table_log_init(5, 'test');
INSERT INTO test VALUES(1, 'joe');
INSERT INTO test VALUES(2, 'barney');
INSERT INTO test VALUES(3, 'monica');
UPDATE test SET name = 'veronica' WHERE id = 3;
DELETE FROM test WHERE id = 1;
SELECT id, name, trigger_mode, trigger_tuple, trigger_id FROM test_log;
DROP TABLE test;
DROP TABLE test_log;
DROP SEQUENCE test_log_seq;

CREATE TABLE test(id integer, name text);
SELECT table_log_init(4, 'test');
INSERT INTO test VALUES(1, 'joe');
INSERT INTO test VALUES(2, 'barney');
INSERT INTO test VALUES(3, 'monica');
UPDATE test SET name = 'veronica' WHERE id = 3;
DELETE FROM test WHERE id = 1;
SELECT id, name, trigger_mode, trigger_tuple, trigger_id FROM test_log;
DROP TABLE test;
DROP TABLE test_log;
DROP SEQUENCE test_log_seq;

CREATE TABLE test(id integer, name text);
SELECT table_log_init(3, 'test');
INSERT INTO test VALUES(1, 'joe');
INSERT INTO test VALUES(2, 'barney');
INSERT INTO test VALUES(3, 'monica');
UPDATE test SET name = 'veronica' WHERE id = 3;
DELETE FROM test WHERE id = 1;
SELECT id, name, trigger_mode, trigger_tuple FROM test_log;
DROP TABLE test;
DROP TABLE test_log;

-- Check table_log_restore_table()

CREATE TABLE test(id integer, name text);
ALTER TABLE test ADD PRIMARY KEY(id);
SELECT table_log_init(5, 'test');
INSERT INTO test VALUES(1, 'joe');
INSERT INTO test VALUES(2, 'barney');
INSERT INTO test VALUES(3, 'monica');
UPDATE test SET name = 'veronica' WHERE id = 3;
DELETE FROM test WHERE id = 1;

SELECT table_log_restore_table('test', 'id', 'test_log', 'trigger_id', 'test_recover', NOW(), '2', NULL::int, 1);
SELECT id, name FROM test_recover;

DROP TABLE test;
DROP TABLE test_log;
DROP TABLE test_recover;
DROP SEQUENCE test_log_seq;

-- Check partition support with auto-generated
-- log table name.
CREATE TABLE test(id integer, name text);
ALTER TABLE test ADD PRIMARY KEY(id);
SELECT table_log_init(5, 'public', 'test', 'public', NULL, 'PARTITION');

SET table_log.active_partition = 0;

INSERT INTO test VALUES(1, 'joe');
INSERT INTO test VALUES(2, 'barney');
INSERT INTO test VALUES(3, 'monica');

SET table_log.active_partition = 1;

UPDATE test SET name = 'veronica' WHERE id = 3;
DELETE FROM test WHERE id = 1;

SELECT id, name FROM test_log;
SELECT id, name FROM test_log_0;
SELECT id, name FROM test_log_1;

--
-- NOTE: since we use partitioning here, we use the special log
--       view to restore the data with id = 2
--
SELECT table_log_restore_table('test', 'id', 'test_log', 'trigger_id', 'test_recover', NOW(), '3', NULL::int, 1);
SELECT id, name FROM test_recover;

DROP TABLE test;
DROP VIEW  test_log;
DROP TABLE test_log_0;
DROP TABLE test_log_1;
DROP TABLE test_recover;
DROP SEQUENCE test_log_seq;

--
-- Check with schema-qualified restore and log table
--
CREATE SCHEMA log;

CREATE TABLE test(id integer, name text);
ALTER TABLE test ADD PRIMARY KEY(id);
SELECT table_log_init(5, 'public', 'test', 'log', NULL, 'PARTITION');

SET table_log.active_partition = 0;

INSERT INTO test VALUES(1, 'joe');
INSERT INTO test VALUES(2, 'barney');
INSERT INTO test VALUES(3, 'monica');

SET table_log.active_partition = 1;

UPDATE test SET name = 'veronica' WHERE id = 3;

SELECT * FROM test;

DELETE FROM test WHERE id = 1;

SELECT * FROM test;

SELECT id, name FROM log.test_log;
SELECT id, name FROM log.test_log_0;
SELECT id, name FROM log.test_log_1;

--
-- NOTE: since we use partitioning here, we use the special log
--       view to restore the data with id = 2
--
SELECT table_log_restore_table('test', 'id', 'log.test_log', 'trigger_id', 'log.test_recover', NOW(), '3', NULL::int, 1);
SELECT id, name FROM log.test_recover;

DROP SCHEMA log CASCADE;
DROP TABLE test;

--
-- Test with a schema-qualified and case sensitive log and
-- restore table
--

CREATE SCHEMA log;

CREATE TABLE test(id integer, name text);
ALTER TABLE test ADD PRIMARY KEY(id);
SELECT table_log_init(5, 'public', 'test', 'log', 'Test', 'PARTITION');

-- Check for log partitions
SELECT 'log."Test_0"'::regclass;
SELECT 'log."Test_1"'::regclass;

SET table_log.active_partition = 0;

INSERT INTO test VALUES(1, 'joe');
INSERT INTO test VALUES(2, 'barney');
INSERT INTO test VALUES(3, 'monica');

SET table_log.active_partition = 1;

UPDATE test SET name = 'veronica' WHERE id = 3;

SELECT * FROM test;

DELETE FROM test WHERE id = 1;

SELECT * FROM test;

SELECT id, name FROM log."Test";
SELECT id, name FROM log."Test_0";
SELECT id, name FROM log."Test_1";

--
-- NOTE: since we use partitioning here, we use the special log
--       view to restore the data with id = 2
--
SELECT table_log_restore_table('test', 'id', 'log."Test"', 'trigger_id', 'log."Test_recover"', NOW(), '3', NULL::int, 1);
SELECT id, name FROM log."Test_recover";

DROP SCHEMA log CASCADE;
DROP TABLE test;

--
-- Test basic log mode
--
CREATE SCHEMA log;

CREATE TABLE test(id integer, name text);
ALTER TABLE test ADD PRIMARY KEY(id);

-- this should fail, no trigger actions specified
SELECT table_log_init(5, 'public', 'test', 'log', NULL, 'PARTITION', true, '{}');

-- this should succeed, but leave out inserts
-- generate the log table this time...
SELECT table_log_init(5, 'public', 'test', 'log', NULL, 'PARTITION', true, '{UPDATE, DELETE}');

-- Log partitions created?
SELECT 'log.test_log_0'::regclass;
SELECT 'log.test_log_1'::regclass;

SET table_log.active_partition = 0;

INSERT INTO test VALUES(1, 'joe');
INSERT INTO test VALUES(2, 'barney');
INSERT INTO test VALUES(3, 'monica');

SET table_log.active_partition = 1;

UPDATE test SET name = 'veronica' WHERE id = 3;

SELECT * FROM test;
DELETE FROM test WHERE id = 1;
SELECT * FROM test;

-- UPDATE logged only, but only old tuples
SELECT id, name FROM log.test_log;
SELECT id, name FROM log.test_log_0;
SELECT id, name FROM log.test_log_1;

-- NOTE:
--    We don't test table_log_restore_table() at this point, since
--    restore from data collected by table_log_basic() is not yet supported.

DROP SCHEMA log CASCADE;
DROP TABLE test;

RESET client_min_messages;

