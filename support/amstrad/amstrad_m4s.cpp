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
#define CMD_M4S_RESP_DONE 0x77

#define M4S_INDEX_SIZE 2048
#define M4S_REQUEST_SIZE 256
#define M4S_LOAD_CHUNK_SIZE 512
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

static void send_response(const uint8_t *data, size_t len)
{
	spi_uio_cmd(CMD_M4S_DIR_BEGIN);

	spi_uio_cmd_cont(CMD_M4S_DIR_WRITE);

	for (size_t i = 0; i < len; i++)
		spi_w(data[i]);

	DisableIO();

	spi_uio_cmd(CMD_M4S_RESP_DONE);
}

static void send_listing(const char *listing)
{
	size_t len = strlen(listing);
	if (len > M4S_INDEX_SIZE - 1) len = M4S_INDEX_SIZE - 1;

	send_response((const uint8_t *)listing, len + 1);
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

static void append_hex_byte(char *response, size_t response_size, uint8_t value)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t used = strlen(response);
	if (used >= response_size - 3) return;

	response[used++] = hex[value >> 4];
	response[used++] = hex[value & 0x0F];
	response[used] = 0;
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

static void build_dump_response(const char *name, char *response, size_t response_size)
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

	uint8_t bytes[16];
	size_t offset = 0;
	while (strlen(response) < response_size - 80)
	{
		size_t count = fread(bytes, 1, sizeof(bytes), file);
		if (!count) break;

		size_t used = strlen(response);
		snprintf(response + used, response_size - used, "%04X:", (unsigned int)offset);

		for (size_t i = 0; i < count; i++)
		{
			used = strlen(response);
			snprintf(response + used, response_size - used, " ");
			append_hex_byte(response, response_size, bytes[i]);
		}

		used = strlen(response);
		snprintf(response + used, response_size - used, "\n");
		offset += count;
	}

	if (!feof(file))
	{
		size_t used = strlen(response);
		snprintf(response + used, response_size - used, "... TRUNCATED\n");
	}

	fclose(file);
}

static int parse_hex_nibble(char value)
{
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	return -1;
}

static int parse_hex16(const char *text, uint16_t *value)
{
	uint16_t parsed = 0;
	for (int i = 0; i < 4; i++)
	{
		int nibble = parse_hex_nibble(text[i]);
		if (nibble < 0) return 0;
		parsed = (parsed << 4) | nibble;
	}

	*value = parsed;
	return 1;
}

static size_t build_load_response(const char *request, uint8_t *response, size_t response_size)
{
	response[0] = 0;
	response[1] = 0;

	if (request[0] != 'L' || request[1] != ':' || request[6] != ':')
		return 2;

	uint16_t offset = 0;
	if (!parse_hex16(request + 2, &offset))
		return 2;

	const char *name = request + 7;
	if (!valid_shared_filename(name))
		return 2;

	const char *basepath = shared_basepath();
	char path[1200];
	if (!resolve_shared_filename(basepath, name, path, sizeof(path)))
		return 2;

	FILE *file = fopen(path, "rb");
	if (!file)
		return 2;

	if (fseek(file, offset, SEEK_SET))
	{
		fclose(file);
		return 2;
	}

	size_t max_count = M4S_LOAD_CHUNK_SIZE;
	if (max_count > response_size - 2) max_count = response_size - 2;

	size_t count = fread(response + 2, 1, max_count, file);
	fclose(file);

	response[0] = count & 0xFF;
	response[1] = (count >> 8) & 0xFF;
	return count + 2;
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

	if (!strncmp(request, "L:", 2))
	{
		uint8_t response[M4S_INDEX_SIZE];
		size_t response_len = build_load_response(request, response, sizeof(response));
		send_response(response, response_len);
	}
	else
	{
		char response[M4S_INDEX_SIZE];
		if (len == 0)
			build_listing(response, sizeof(response));
		else if (!strncmp(request, "D:", 2))
			build_dump_response(request + 2, response, sizeof(response));
		else
			build_type_response(request, response, sizeof(response));

		send_listing(response);
	}

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
