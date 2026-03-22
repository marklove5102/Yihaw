REPO_URL = https://github.com/ozkl/doomgeneric.git
REPO_DIR = doomgeneric
BUILD_DIR = $(REPO_DIR)/doomgeneric
HERE = $(CURDIR)

.PHONY: all clean

all:
	git clone $(REPO_URL) $(REPO_DIR)
	cp $(HERE)/doomgeneric.h     $(BUILD_DIR)/doomgeneric.h
	cp $(HERE)/doomgeneric_anyka.c $(BUILD_DIR)/doomgeneric_anyka.c
	cp $(HERE)/Makefile.anyka    $(BUILD_DIR)/Makefile.anyka
	$(MAKE) -C $(BUILD_DIR) -f Makefile.anyka
	cp $(BUILD_DIR)/doom $(HERE)/doom

clean:
	rm -rf $(REPO_DIR)
	rm -f $(HERE)/doom