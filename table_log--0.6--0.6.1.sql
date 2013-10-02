CREATE FUNCTION table_log_basic()
    RETURNS TRIGGER
    AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OR REPLACE FUNCTION table_log_init(int, text, text, text, text, text DEFAULT 'SINGLE',
                                          boolean DEFAULT false,
                                          text[] DEFAULT '{INSERT, UPDATE, DELETE}'::text[]) RETURNS void AS
$table_log_init$
DECLARE
    level        ALIAS FOR $1;
    orig_schema  ALIAS FOR $2;
    orig_name    ALIAS FOR $3;
    log_schema   ALIAS FOR $4;
    log_name     ALIAS FOR $5;
    partition_mode ALIAS FOR $6;
    basic_mode     ALIAS FOR $7;
    log_actions  ALIAS FOR $8;
    do_log_user  int = 0;
    level_create text = '';
    orig_qq      text;
    log_qq       text;
    log_part     text[];
    log_seq      text;
    num_log_tables integer;
    trigger_func text := 'table_log';
    trigger_actions text := '';
    i integer;
BEGIN
    -- Handle if someone doesn't want an explicit log table name
    log_name := COALESCE(log_name, orig_name || '_log');

    -- Quoted qualified names
    orig_qq := quote_ident(orig_schema) || '.' || quote_ident(orig_name);
    log_qq := quote_ident(log_schema) || '.'  || quote_ident(log_name);
    log_seq := quote_ident(log_schema) || '.' || quote_ident(log_name || '_seq');
    log_part[0] := quote_ident(log_schema) || '.' || quote_ident(log_name || '_0');
    log_part[1] := quote_ident(log_schema) || '.' || quote_ident(log_name || '_1');

    -- Valid trigger actions?
    IF (COALESCE(array_length(log_actions, 1), 0) = 0) THEN
       RAISE EXCEPTION 'table_log_init: at least one trigger action must be specified';
    END IF;

    -- Valid partition mode ?
    IF (partition_mode NOT IN ('SINGLE', 'PARTITION')) THEN
        RAISE EXCEPTION 'table_log_init: unsupported partition mode %', partition_mode;
    END IF;

    IF level <> 3 THEN

       --
       -- Create a sequence used by trigger_id, if requested.
       --
       EXECUTE 'CREATE SEQUENCE ' || log_seq;

       level_create := level_create
           || ', trigger_id BIGINT'
           || ' DEFAULT nextval($$' || log_seq || '$$::regclass)'
           || ' NOT NULL PRIMARY KEY';

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
        EXECUTE  'CREATE TABLE ' || log_part[0]
              || '(LIKE ' || orig_qq
              || ', trigger_mode VARCHAR(10) NOT NULL'
              || ', trigger_tuple VARCHAR(5) NOT NULL'
              || ', trigger_changed TIMESTAMPTZ NOT NULL'
              || level_create
              || ')';

        EXECUTE  'CREATE TABLE ' || log_part[1]
              || '(LIKE ' || orig_qq
              || ', trigger_mode VARCHAR(10) NOT NULL'
              || ', trigger_tuple VARCHAR(5) NOT NULL'
              || ', trigger_changed TIMESTAMPTZ NOT NULL'
              || level_create
              || ')';

        EXECUTE 'CREATE VIEW ' || log_qq
              || ' AS SELECT * FROM ' || log_part[0] || ' UNION ALL '
              || 'SELECT * FROM ' || log_part[1] || '';
    END IF;

    --
    -- Either use basic or full trigger mode
    --
    IF basic_mode THEN
       trigger_func := 'table_log_basic';
    END IF;

    --
    -- Build action string for trigger DDL
    --
    FOR i IN 1..array_length(log_actions, 1)
    LOOP

        trigger_actions := trigger_actions || log_actions[i];

        IF i < array_length(log_actions, 1) THEN
           trigger_actions := trigger_actions || ' OR ';
        END IF;

    END LOOP;

    EXECUTE 'CREATE TRIGGER "table_log_trigger" AFTER ' || trigger_actions || ' ON '
            || orig_qq || ' FOR EACH ROW EXECUTE PROCEDURE ' || trigger_func || '('
            || quote_literal(log_name) || ','
            || do_log_user || ','
            || quote_literal(log_schema) || ','
            || quote_literal(partition_mode)
            || ')';

    RETURN;
END;
$table_log_init$
LANGUAGE plpgsql;

