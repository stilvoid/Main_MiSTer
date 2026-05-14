#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../../cfg.h"
#include "../../file_io.h"
#include "../../hardware.h"
#include "../../spi.h"
#include "../../user_io.h"
#include "amstrad_m4s.h"

#define CMD_M4S_DIR_BEGIN 0x70
#define CMD_M4S_DIR_WRITE 0x71

#define M4S_INDEX_SIZE 2048

static unsigned long refresh_timer = 0;

static int is_amstrad_core()
{
	return !strcasecmp(user_io_get_core_name(), "Amstrad") ||
	       !strcasecmp(user_io_get_core_name(1), "Amstrad");
}

static const char *shared_basepath()
{
	static char basepath[1024] = {};
	char mister_path[1024] = {};

	if (strlen(cfg.shared_folder))
	{
		if (cfg.shared_folder[0] == '/')
		{
			snprintf(basepath, sizeof(basepath), "%s", cfg.shared_folder);
			FileCreatePath(basepath);
			return basepath;
		}
		else
		{
			snprintf(mister_path, sizeof(mister_path), "%s/%s", HomeDir(), cfg.shared_folder);
		}
	}
	else
	{
		snprintf(mister_path, sizeof(mister_path), "%s/shared", HomeDir());
	}

	snprintf(basepath, sizeof(basepath), "%s/%s", getRootDir(), mister_path);

	FileCreatePath(basepath);
	return basepath;
}

static void append_listing(char *listing, size_t listing_size, const char *name, int is_dir)
{
	size_t used = strlen(listing);
	if (used >= listing_size - 1) return;

	snprintf(listing + used, listing_size - used, "%s%s\n", name, is_dir ? "/" : "");
}

static void build_listing(char *listing, size_t listing_size)
{
	const char *basepath = shared_basepath();
	DIR *dir = opendir(basepath);

	listing[0] = 0;
	if (!dir)
	{
		snprintf(listing, listing_size, "NO SHARED FOLDER\n");
		return;
	}

	append_listing(listing, listing_size, "M4S SHARED", 0);

	struct dirent *entry;
	while ((entry = readdir(dir)))
	{
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;

		char fullpath[1024];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", basepath, entry->d_name);

		struct stat st;
		int is_dir = !stat(fullpath, &st) && S_ISDIR(st.st_mode);
		append_listing(listing, listing_size, entry->d_name, is_dir);
	}

	closedir(dir);
}

static void send_listing(const char *listing)
{
	spi_uio_cmd(CMD_M4S_DIR_BEGIN);

	spi_uio_cmd_cont(CMD_M4S_DIR_WRITE);

	size_t len = strlen(listing);
	if (len > M4S_INDEX_SIZE - 1) len = M4S_INDEX_SIZE - 1;

	for (size_t i = 0; i < len; i++)
		spi_w((uint8_t)listing[i]);

	spi_w(0);
	DisableIO();
}

void amstrad_m4s_poll()
{
	if (!is_amstrad_core())
		return;

	if (refresh_timer && !CheckTimer(refresh_timer))
		return;

	refresh_timer = GetTimer(2000);

	char listing[M4S_INDEX_SIZE];
	build_listing(listing, sizeof(listing));
	send_listing(listing);
}
