MODULES = table_log
EXTENSION = table_log
DATA = table_log--0.6.1.sql table_log--unpackaged--0.6.1.sql table_log--0.5--0.6.1.sql table_log--0.6--0.6.1.sql
## keep it for non-EXTENSION installations
## DATA_built = table_log.sql uninstall_table_log.sql
DOCS = README.md
REGRESS = table_log
ifndef PG_CONFIG
PG_CONFIG = pg_config
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
