#include <ctype.h>
#include <dirent.h>
#include <errno.h>
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
#define CMD_M4S_REQ_STATUS 0x72
#define CMD_M4S_REQ_READ 0x73
#define CMD_M4S_REQ_ACK 0x74

#define M4S_INDEX_SIZE 2048
#define M4S_REQUEST_SIZE 256
static unsigned long request_timer = 0;

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

static uint16_t request_status()
{
	spi_uio_cmd_cont(CMD_M4S_REQ_STATUS);
	uint16_t status = spi_w(0);
	DisableIO();
	return status;
}

static uint8_t request_byte(uint8_t addr)
{
	spi_uio_cmd_cont(CMD_M4S_REQ_READ);
	uint16_t data = spi_w(addr);
	DisableIO();
	return data & 0xFF;
}

static void request_ack()
{
	spi_uio_cmd(CMD_M4S_REQ_ACK);
}

static int valid_shared_filename(const char *name)
{
	if (!name[0]) return 0;
	if (strstr(name, "..")) return 0;
	if (strchr(name, '/')) return 0;
	if (strchr(name, '\\')) return 0;
	return 1;
}

static void normalize_shared_filename(char *name)
{
	for (size_t i = 0; name[i]; i++)
	{
		if ((unsigned char)name[i] < ' ')
		{
			name[i] = 0;
			break;
		}
	}

	size_t len = strlen(name);
	while (len && (unsigned char)name[len - 1] <= ' ')
		name[--len] = 0;
}

static void append_request_hex(char *response, size_t response_size, const char *name)
{
	size_t used = strlen(response);
	if (used >= response_size - 1) return;

	used += snprintf(response + used, response_size - used, "HEX=");
	for (size_t i = 0; name[i] && used < response_size - 4; i++)
		used += snprintf(response + used, response_size - used, "%02X", (unsigned char)name[i]);

	snprintf(response + used, response_size - used, "\n");
}

static int resolve_shared_filename(const char *basepath, const char *name, char *path, size_t path_size)
{
	snprintf(path, path_size, "%s/%s", basepath, name);

	FILE *file = fopen(path, "rb");
	if (file)
	{
		fclose(file);
		return 1;
	}

	DIR *dir = opendir(basepath);
	if (!dir) return 0;

	struct dirent *entry;
	while ((entry = readdir(dir)))
	{
		if (!strcasecmp(entry->d_name, name))
		{
			snprintf(path, path_size, "%s/%s", basepath, entry->d_name);
			closedir(dir);
			return 1;
		}
	}

	closedir(dir);
	return 0;
}

static void build_type_response(const char *name, char *response, size_t response_size)
{
	response[0] = 0;
	const char *basepath = shared_basepath();

	if (!valid_shared_filename(name))
	{
		snprintf(response, response_size, "BAD FILENAME\n");
		return;
	}

	char path[1200];
	if (!resolve_shared_filename(basepath, name, path, sizeof(path)))
	{
		snprintf(response, response_size, "OPEN FAILED: %s\nBASE=%s\nPATH=%s/%s\n", name, basepath, basepath, name);
		append_request_hex(response, response_size, name);
		return;
	}

	FILE *file = fopen(path, "rb");
	if (!file)
	{
		snprintf(response, response_size, "OPEN FAILED: %s\nPATH=%s\nERRNO=%d\n", name, path, errno);
		append_request_hex(response, response_size, name);
		return;
	}

	size_t used = fread(response, 1, response_size - 1, file);
	response[used] = 0;
	fclose(file);
}

static int process_host_request()
{
	uint16_t status = request_status();
	if (!(status & 1)) return 0;

	uint16_t len = status >> 8;
	if (len > M4S_REQUEST_SIZE - 1) len = M4S_REQUEST_SIZE - 1;

	char request[M4S_REQUEST_SIZE] = {};
	for (uint16_t i = 0; i < len; i++)
	{
		request[i] = (char)request_byte((uint8_t)i);
		if (!request[i]) break;
	}
	request[M4S_REQUEST_SIZE - 1] = 0;
	normalize_shared_filename(request);

	request_ack();

	char response[M4S_INDEX_SIZE];
	if (len == 0)
	{
		build_listing(response, sizeof(response));
	}
	else
	{
		build_type_response(request, response, sizeof(response));
	}

	send_listing(response);
	return 1;
}

void amstrad_m4s_poll()
{
	if (!is_amstrad_core())
		return;

	if (!request_timer || CheckTimer(request_timer))
	{
		request_timer = GetTimer(50);
		if (process_host_request())
			return;
	}
}
