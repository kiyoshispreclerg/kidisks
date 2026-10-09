include config.mk

BUILD ?= build

# Everything in src/ but main.c goes into a static lib the unit tests link
# against.
LIB_SRC = src/log.c \
      src/util.c \
      src/ini.c \
      src/prefs.c
LIBKD = $(BUILD)/libkd.a

# The window: needs GTK.
APP_SRC = src/main.c \
      ui/window.c
APP_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(APP_SRC))

UNIT_TESTS = $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(wildcard tests/unit/test_*.c))

ALL_CFLAGS  = $(STDFLAGS) $(OPTFLAGS) $(SANFLAGS) $(PKG_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(SANFLAGS) $(LDFLAGS)

.PHONY: all test run asan asan-run install uninstall lint clean locale pot update-po

# Translations (po/LINGUAS lists the languages). Built into $(BUILD)/locale,
# which the program finds next to its own executable.
LINGUAS = $(shell sed 's/\#.*//' po/LINGUAS)
HAVE_MSGFMT := $(shell command -v msgfmt >/dev/null 2>&1 && echo 1)
MO_FILES = $(if $(HAVE_MSGFMT),$(foreach l,$(LINGUAS),$(BUILD)/locale/$(l)/LC_MESSAGES/kidisks.mo))

all: $(BUILD)/kidisks $(MO_FILES)
locale: $(MO_FILES)

$(BUILD)/locale/%/LC_MESSAGES/kidisks.mo: po/%.po
	@mkdir -p $(@D)
	msgfmt -c -o $@ $<

# Rebuilds the template from the sources and merges it into every .po. No
# --package-version: config.mk's VERSION is the only place the version
# lives, so bumping it doesn't touch the .po files too.
pot update-po:
	xgettext --from-code=UTF-8 -L C --keyword=_ --keyword=N_ --keyword=ngettext:1,2 \
		--add-comments=TRANSLATORS -f po/POTFILES.in -o po/kidisks.pot \
		--package-name=kidisks \
		--copyright-holder="Kidisks contributors" --msgid-bugs-address=""
	for l in $(LINGUAS); do msgmerge -q -U --backup=none po/$$l.po po/kidisks.pot; done

$(BUILD)/kidisks: $(APP_OBJ) $(LIBKD)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(GTK_LIBS) $(LDLIBS)

$(APP_OBJ): $(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DLOCALEDIR='"$(LOCALEDIR)"' $(ALL_CFLAGS) $(GTK_CFLAGS) $(WARNFLAGS) -MMD -MP -c -o $@ $<

$(LIBKD): $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRC))
	$(AR) rcs $@ $^

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -MMD -MP -c -o $@ $<

# ---- tests ----------------------------------------------------------------

TEST_CPPFLAGS = -DKD_FIXTURES_DIR='"$(CURDIR)/tests/fixtures"'

$(BUILD)/tests/%: tests/unit/%.c tests/harness.h $(LIBKD)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -MMD -MP \
		$(ALL_LDFLAGS) -o $@ $< $(LIBKD) $(LDLIBS)

test: all $(UNIT_TESTS)
	@fail=0; for t in $(UNIT_TESTS); do \
		$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "*** unit tests FAILED"; exit 1; fi; \
	echo "*** all unit tests passed"

run: $(BUILD)/kidisks $(MO_FILES)
	$(BUILD)/kidisks $(ARGS)

asan:
	LSAN_OPTIONS=suppressions=$(CURDIR)/tools/lsan.supp \
		$(MAKE) BUILD=$(BUILD)/asan SANITIZE=1 test

asan-run: asan
	LSAN_OPTIONS=suppressions=$(CURDIR)/tools/lsan.supp $(BUILD)/asan/kidisks $(ARGS)

install: $(BUILD)/kidisks $(MO_FILES)
	install -Dm755 $< $(DESTDIR)$(BINDIR)/kidisks
	install -Dm644 data/kidisks.desktop $(DESTDIR)$(DATADIR)/applications/kidisks.desktop
	install -Dm644 data/kidisks.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/kidisks.svg
	$(foreach l,$(if $(MO_FILES),$(LINGUAS)),install -Dm644 $(BUILD)/locale/$(l)/LC_MESSAGES/kidisks.mo \
		$(DESTDIR)$(LOCALEDIR)/$(l)/LC_MESSAGES/kidisks.mo;)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kidisks
	for l in $(LINGUAS); do rm -f $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/kidisks.mo; done
	rm -f $(DESTDIR)$(DATADIR)/applications/kidisks.desktop \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/kidisks.svg

lint:
	@if grep -nE '\b(strcpy|strcat|sprintf|vsprintf|gets)[[:space:]]*\(' src/*.c src/*.h ui/*.c ui/*.h tests/unit/*.c; then \
		echo "*** banned function used"; exit 1; fi
	@command -v cppcheck >/dev/null || { echo "*** cppcheck not installed"; exit 1; }
	cppcheck --std=c11 --enable=warning,performance,portability \
		--error-exitcode=1 --inline-suppr --quiet -Isrc -Iui \
		-D_XOPEN_SOURCE=700 -DKD_VERSION='"lint"' -DKD_FIXTURES_DIR='"."' src tests/unit

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
