
TMPDIR ?= /tmp
SDK_HOME ?= /opt/vertica/sdk
SHELL = /bin/bash
VSQL ?= /opt/vertica/bin/vsql
LOADER_DEBUG = 0
TARGET ?= ./lib

ALL_CXXFLAGS := $(CXXFLAGS) -I $(SDK_HOME)/include -I $(SDK_HOME)/examples/HelperLibraries -fPIC -shared -Wall -g -std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1
ALL_CXXFLAGS += -DLOADER_DEBUG=$(LOADER_DEBUG)

build: $(TARGET)/ODBCLoader.so
## See targets below for actual build logic

clean:
	rm $(TARGET)/ODBCLoader.so

install: build
	# install ODBCLoader
	@$(VSQL) -f ddl/install.sql

uninstall:
	# uninstall ODBCLoader
	@$(VSQL) -f ddl/uninstall.sql

example:
	@# Try uninstalling first, just in case we have a stale version around
	-@$(MAKE) -s uninstall >/dev/null 2>&1
	@$(MAKE) --no-print-dir install
	@# Use bash's "trap" to uninstall and still return an error
	@trap '$(MAKE) --no-print-dir uninstall' EXIT; $(MAKE) --no-print-dir test_example

test_example:
	# run tests
	@$(VSQL) -f examples/sample_usage.sql > examples/Tests.actual 2>&1
	@# filter out variable messages (i.e., mariadb vs mysql)
	@diff -u examples/Tests.out <(perl -pe 's/^vsql:[\/_:\w\.]* /vsql: /; \
	              s/\[ODBC[^\]]*\]/[...]/g; \
		      s/\[mysql[^\]]*\]/[...]/g; \
		      s/(Error parsing .* )\(.*\)$$/$$1(...)/; \
		      s/mariadb/MySQL/ig; ' examples/Tests.actual)
	@echo ALL TESTS SUCCESSFUL

test:
	@$(VSQL) -f tests/copy_test.sql > $(TMPDIR)/copy_test.out 2>&1
	@diff -u tests/expected/copy_test.out <(perl -pe 's/^vsql:[\/_:\w\.]* /vsql: /; \
	              s/\[ODBC[^\]]*\]/[...]/g; \
		      s/\[mysql[^\]]*\]/[...]/g; \
		      s/(Error parsing .* )\(.*\)$$/$$1(...)/; \
		      s/mariadb/MySQL/ig; ' $(TMPDIR)/copy_test.out)
	@echo ALL TESTS SUCCESSFUL

.PHONY: build clean install uninstall example test_example test


## Actual build target
$(TARGET)/ODBCLoader.so: ODBCLoader.cpp $(SDK_HOME)/include/Vertica.cpp $(SDK_HOME)/include/BuildInfo.h
	mkdir -p $(TARGET)
	$(CXX) $(ALL_CXXFLAGS) -o $@ $(SDK_HOME)/include/Vertica.cpp ODBCLoader.cpp -lodbc -lpcrecpp -lpcre
