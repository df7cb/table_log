ALTER EXTENSION table_log ADD FUNCTION table_log();
ALTER EXTENSION table_log ADD FUNCTION table_log_restore_table(varchar,
                                                               varchar,
                                                               char,
                                                               char,
                                                               char,
                                                               timestamp with time zone,
                                                               char,
                                                               integer,
                                                               integer);
ALTER EXTENSION table_log ADD FUNCTION table_log_restore_table(varchar,
                                                               varchar,
                                                               char,
                                                               char,
                                                               char,
                                                               timestamp with time zone,
                                                               char,
                                                               integer);
ALTER EXTENSION table_log ADD FUNCTION table_log_restore_table(varchar,
                                                               varchar,
                                                               char,
                                                               char,
                                                               char,
                                                               timestamp with time zone,
                                                               char);
ALTER EXTENSION table_log ADD FUNCTION table_log_restore_table(varchar,
                                                               varchar,
                                                               char,
                                                               char,
                                                               char,
                                                               timestamp with time zone);

--
-- NOTE:
--
-- When upgrading from 'unpackaged' we assume that the original
-- version is an old style contrib installation with table_log
-- 0.4 or below. This version doesn't have the six argument
-- version of table_log_init()...so drop the old one and recreate
-- the new version from scratch.
--
DROP FUNCTION FUNCTION table_log_init(integer,
                                      text,
                                      text,
                                      text,
                                      text);

-- Create new version of table_log_init() having the new default partition mode
-- parameter.
CREATE OR REPLACE FUNCTION table_log_init(int, text, text, text, text, text DEFAULT 'SINGLE') RETURNS void AS
$table_log_init$
DECLARE
    level        ALIAS FOR $1;
    orig_schema  ALIAS FOR $2;
    orig_name    ALIAS FOR $3;
    log_schema   ALIAS FOR $4;
    log_name     ALIAS FOR $5;
    do_log_user  int = 0;
    level_create text = '';
    orig_qq      text;
    log_qq       text;
    partition_mode ALIAS FOR $6;
    num_log_tables integer;
BEGIN
    -- Quoted qualified names
    orig_qq := quote_ident(orig_schema) || '.' ||quote_ident(orig_name);
    log_qq := quote_ident(log_schema) || '.' ||quote_ident(log_name);

    -- Valid partition mode ?
    IF (partition_mode NOT IN ('SINGLE', 'PARTITION')) THEN
        RAISE EXCEPTION 'table_log_init: unsupported partition mode %', partition_mode;
    END IF;

    IF level <> 3 THEN
        level_create := level_create
            || ', trigger_id BIGSERIAL NOT NULL PRIMARY KEY';
        IF level <> 4 THEN
            level_create := level_create
                || ', trigger_user VARCHAR(32) NOT NULL';
            do_log_user := 1;
            IF level <> 5 THEN
                RAISE EXCEPTION
                    'table_log_init: First arg has to be 3, 4 or 5.';
            END IF;
        END IF;
    END IF;

    IF (partition_mode = 'SINGLE') THEN
        EXECUTE  'CREATE TABLE ' || log_qq
              || '(LIKE ' || orig_qq
              || ', trigger_mode VARCHAR(10) NOT NULL'
              || ', trigger_tuple VARCHAR(5) NOT NULL'
              || ', trigger_changed TIMESTAMPTZ NOT NULL'
              || level_create
              || ')';

    ELSE
        -- Partitioned mode requested...
        EXECUTE  'CREATE TABLE ' || log_qq || '_0'
              || '(LIKE ' || orig_qq
              || ', trigger_mode VARCHAR(10) NOT NULL'
              || ', trigger_tuple VARCHAR(5) NOT NULL'
              || ', trigger_changed TIMESTAMPTZ NOT NULL'
              || level_create
              || ')';

        EXECUTE  'CREATE TABLE ' || log_qq || '_1'
              || '(LIKE ' || orig_qq
              || ', trigger_mode VARCHAR(10) NOT NULL'
              || ', trigger_tuple VARCHAR(5) NOT NULL'
              || ', trigger_changed TIMESTAMPTZ NOT NULL'
              || level_create
              || ')';

        EXECUTE 'CREATE VIEW ' || log_qq || '_v'
              || ' AS SELECT * FROM ' || log_qq || '_0 UNION ALL '
              || 'SELECT * FROM ' || log_qq || '_1';
    END IF;


    EXECUTE 'CREATE TRIGGER "table_log_trigger" AFTER UPDATE OR INSERT OR DELETE ON '
            || orig_qq || ' FOR EACH ROW EXECUTE PROCEDURE table_log('
            || quote_literal(log_name) || ','
            || do_log_user || ','
            || quote_literal(log_schema) || ','
            || quote_literal(partition_mode)
            || ')';

    RETURN;
END;
$table_log_init$
LANGUAGE plpgsql;

ALTER EXTENSION table_log ADD FUNCTION table_log_init(integer,
                                                      text,
                                                      text,
                                                      text,
                                                      text,
						      text DEFAULT 'SINGLE');
ALTER EXTENSION table_log ADD FUNCTION table_log_init(integer,
                                                      text);
ALTER EXTENSION table_log ADD FUNCTION table_log_init(integer,
                                                      text,
                                                      text);
ALTER EXTENSION table_log ADD FUNCTION table_log_init(integer,
                                                      text,
                                                      text,
                                                      text);
