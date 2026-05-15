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
static char current_dir[1024] = {};

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

static void shared_current_path(char *path, size_t path_size)
{
	const char *basepath = shared_basepath();
	if (current_dir[0])
		snprintf(path, path_size, "%s/%s", basepath, current_dir);
	else
		snprintf(path, path_size, "%s", basepath);
}

static void append_current_dir(char *response, size_t response_size)
{
	size_t used = strlen(response);
	if (used >= response_size - 1) return;

	snprintf(response + used, response_size - used, "CWD: /%s\n", current_dir);
}

static void append_listing(char *listing, size_t listing_size, const char *name, int is_dir)
{
	size_t used = strlen(listing);
	if (used >= listing_size - 1) return;

	snprintf(listing + used, listing_size - used, "%s%s\n", name, is_dir ? "/" : "");
}

static void build_listing(char *listing, size_t listing_size)
{
	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));
	DIR *dir = opendir(basepath);

	listing[0] = 0;
	if (!dir)
	{
		snprintf(listing, listing_size, "NO SHARED FOLDER\n");
		return;
	}

	append_listing(listing, listing_size, "M4S SHARED", 0);
	append_current_dir(listing, listing_size);

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

static int valid_shared_save_filename(const char *name)
{
	if (!valid_shared_filename(name)) return 0;
	if (strchr(name, ':')) return 0;
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

static int append_path_component(char *path, size_t path_size, const char *component)
{
	size_t used = strlen(path);
	size_t len = strlen(component);
	if (!len) return 1;
	if (used + len + (used ? 1 : 0) >= path_size) return 0;

	if (used)
		path[used++] = '/';

	memcpy(path + used, component, len + 1);
	return 1;
}

static int pop_path_component(char *path)
{
	char *slash = strrchr(path, '/');
	if (slash)
	{
		*slash = 0;
		return 1;
	}

	if (path[0])
	{
		path[0] = 0;
		return 1;
	}

	return 0;
}

static int split_next_path_component(const char **cursor, char *component, size_t component_size)
{
	const char *start = *cursor;
	while (*start == '/' || *start == '\\')
		start++;

	if (!*start)
	{
		*cursor = start;
		return 0;
	}

	const char *end = start;
	while (*end && *end != '/' && *end != '\\')
		end++;

	size_t len = end - start;
	if (len >= component_size)
		len = component_size - 1;

	memcpy(component, start, len);
	component[len] = 0;
	*cursor = end;
	return 1;
}

static int resolve_shared_directory_component(const char *basepath, const char *component, char *resolved, size_t resolved_size)
{
	if (!component[0] || strchr(component, '/') || strchr(component, '\\'))
		return 0;

	char path[1200];
	snprintf(path, sizeof(path), "%s/%s", basepath, component);

	struct stat st;
	if (!stat(path, &st) && S_ISDIR(st.st_mode))
	{
		snprintf(resolved, resolved_size, "%s", component);
		return 1;
	}

	DIR *dir = opendir(basepath);
	if (!dir) return 0;

	struct dirent *entry;
	while ((entry = readdir(dir)))
	{
		if (!strcasecmp(entry->d_name, component))
		{
			snprintf(path, sizeof(path), "%s/%s", basepath, entry->d_name);
			if (!stat(path, &st) && S_ISDIR(st.st_mode))
			{
				snprintf(resolved, resolved_size, "%s", entry->d_name);
				closedir(dir);
				return 1;
			}
		}
	}

	closedir(dir);
	return 0;
}

static int resolve_shared_relative_dir(const char *requested, char *resolved, size_t resolved_size)
{
	char candidate[1024] = {};
	if (requested[0] != '/' && requested[0] != '\\')
		snprintf(candidate, sizeof(candidate), "%s", current_dir);

	const char *cursor = requested;
	char component[256];
	while (split_next_path_component(&cursor, component, sizeof(component)))
	{
		if (!strcmp(component, "."))
		{
			continue;
		}
		else if (!strcmp(component, ".."))
		{
			if (!pop_path_component(candidate))
				return 0;
		}
		else
		{
			char basepath[1200];
			const char *root = shared_basepath();
			if (candidate[0])
				snprintf(basepath, sizeof(basepath), "%s/%s", root, candidate);
			else
				snprintf(basepath, sizeof(basepath), "%s", root);

			char resolved_component[256];
			if (!resolve_shared_directory_component(basepath, component, resolved_component, sizeof(resolved_component)))
				return 0;

			if (!append_path_component(candidate, sizeof(candidate), resolved_component))
				return 0;
		}
	}

	snprintf(resolved, resolved_size, "%s", candidate);
	return 1;
}

static uint16_t le16(const uint8_t *data)
{
	return data[0] | (data[1] << 8);
}

static uint32_t le24(const uint8_t *data)
{
	return data[0] | (data[1] << 8) | (data[2] << 16);
}

static void build_type_response(const char *name, char *response, size_t response_size)
{
	response[0] = 0;
	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));

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

static void append_amsdos_name(char *response, size_t response_size, const uint8_t *header)
{
	size_t used = strlen(response);
	if (used >= response_size - 1) return;

	used += snprintf(response + used, response_size - used, "AMSDOS NAME: ");

	for (int i = 1; i <= 8 && used < response_size - 1; i++)
	{
		if (header[i] == ' ') break;
		response[used++] = header[i];
	}

	if (used < response_size - 1)
		response[used++] = '.';

	for (int i = 9; i <= 11 && used < response_size - 1; i++)
	{
		if (header[i] == ' ') break;
		response[used++] = header[i];
	}

	if (used < response_size - 2)
	{
		response[used++] = '\n';
		response[used] = 0;
	}
}

static void build_info_response(const char *name, char *response, size_t response_size)
{
	response[0] = 0;
	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));

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

	struct stat st;
	long file_size = !stat(path, &st) ? (long)st.st_size : -1;

	FILE *file = fopen(path, "rb");
	if (!file)
	{
		snprintf(response, response_size, "OPEN FAILED: %s\nPATH=%s\nERRNO=%d\n", name, path, errno);
		append_request_hex(response, response_size, name);
		return;
	}

	uint8_t header[128] = {};
	size_t count = fread(header, 1, sizeof(header), file);
	fclose(file);

	snprintf(response, response_size, "FILE: %s\nSIZE: %ld\n", name, file_size);

	if (count < sizeof(header))
	{
		size_t used = strlen(response);
		snprintf(response + used, response_size - used, "AMSDOS: NO HEADER\n");
		return;
	}

	uint16_t checksum = 0;
	for (int i = 0; i <= 66; i++)
		checksum += header[i];

	uint16_t stored_checksum = le16(header + 67);
	if (checksum != stored_checksum)
	{
		size_t used = strlen(response);
		snprintf(response + used, response_size - used,
		         "AMSDOS: NO HEADER\nCHECKSUM: %04X EXPECTED %04X\n",
		         stored_checksum, checksum);
		return;
	}

	append_amsdos_name(response, response_size, header);

	size_t used = strlen(response);
	snprintf(response + used, response_size - used,
	         "AMSDOS: HEADER OK\nTYPE: %02X\nDATA LEN: %u\nLOAD: &%04X\nLOGICAL LEN: %u\nENTRY: &%04X\nREAL LEN: %lu\n",
	         header[18],
	         le16(header + 19),
	         le16(header + 21),
	         le16(header + 24),
	         le16(header + 26),
	         (unsigned long)le24(header + 64));
}

static void build_dump_response(const char *name, char *response, size_t response_size)
{
	response[0] = 0;
	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));

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

static int parse_hex8(const char *text, uint8_t *value)
{
	uint8_t parsed = 0;
	for (int i = 0; i < 2; i++)
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

	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));
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

static int read_amsdos_header(const char *name, uint8_t *header, char *path, size_t path_size)
{
	if (!valid_shared_filename(name))
		return 0;

	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));
	if (!resolve_shared_filename(basepath, name, path, path_size))
		return 0;

	FILE *file = fopen(path, "rb");
	if (!file)
		return 0;

	size_t count = fread(header, 1, 128, file);
	fclose(file);
	if (count != 128)
		return 0;

	uint16_t checksum = 0;
	for (int i = 0; i <= 66; i++)
		checksum += header[i];

	return checksum == le16(header + 67);
}

static size_t build_header_load_response(const char *request, uint8_t *response, size_t response_size)
{
	for (int i = 0; i < 7; i++)
		response[i] = 0;

	if (request[0] != 'H' || request[1] != ':' || request[6] != ':')
		return 7;

	uint16_t offset = 0;
	if (!parse_hex16(request + 2, &offset))
		return 7;

	const char *name = request + 7;
	uint8_t header[128] = {};
	char path[1200];
	if (!read_amsdos_header(name, header, path, sizeof(path)))
		return 7;

	uint16_t logical_len = le16(header + 24);
	uint16_t load_addr = le16(header + 21);
	uint16_t entry_addr = le16(header + 26);

	response[2] = load_addr & 0xFF;
	response[3] = load_addr >> 8;
	response[4] = entry_addr & 0xFF;
	response[5] = entry_addr >> 8;
	response[6] = header[18];

	if (offset >= logical_len)
		return 7;

	FILE *file = fopen(path, "rb");
	if (!file)
		return 7;

	if (fseek(file, 128 + offset, SEEK_SET))
	{
		fclose(file);
		return 7;
	}

	size_t max_count = M4S_LOAD_CHUNK_SIZE;
	size_t remaining = logical_len - offset;
	if (max_count > remaining) max_count = remaining;
	if (max_count > response_size - 7) max_count = response_size - 7;

	size_t count = fread(response + 7, 1, max_count, file);
	fclose(file);

	response[0] = count & 0xFF;
	response[1] = count >> 8;
	return count + 7;
}

static void build_save_response(const char *request, char *response, size_t response_size)
{
	response[0] = 0;

	if (request[0] != 'S' || request[1] != ':' || request[6] != ':' || request[9] != ':')
	{
		snprintf(response, response_size, "BAD SAVE REQUEST\n");
		return;
	}

	uint16_t offset = 0;
	uint8_t count = 0;
	if (!parse_hex16(request + 2, &offset) || !parse_hex8(request + 7, &count))
	{
		snprintf(response, response_size, "BAD SAVE OFFSET\n");
		return;
	}

	const char *name = request + 10;
	const char *separator = strchr(name, ':');
	if (!separator)
	{
		snprintf(response, response_size, "BAD SAVE NAME\n");
		return;
	}

	size_t name_len = separator - name;
	char filename[256];
	if (!name_len || name_len >= sizeof(filename))
	{
		snprintf(response, response_size, "BAD SAVE NAME\n");
		return;
	}

	memcpy(filename, name, name_len);
	filename[name_len] = 0;
	if (!valid_shared_save_filename(filename))
	{
		snprintf(response, response_size, "BAD FILENAME\n");
		return;
	}

	const char *hex = separator + 1;
	for (uint8_t i = 0; i < count; i++)
	{
		if (!hex[i * 2] || !hex[i * 2 + 1])
		{
			snprintf(response, response_size, "BAD SAVE DATA\n");
			return;
		}
	}

	uint8_t bytes[64];
	if (count > sizeof(bytes))
	{
		snprintf(response, response_size, "SAVE CHUNK TOO LARGE\n");
		return;
	}

	for (uint8_t i = 0; i < count; i++)
	{
		if (!parse_hex8(hex + (i * 2), bytes + i))
		{
			snprintf(response, response_size, "BAD SAVE DATA\n");
			return;
		}
	}

	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));
	char path[1200];
	snprintf(path, sizeof(path), "%s/%s", basepath, filename);

	FILE *file = fopen(path, offset ? "r+b" : "wb");
	if (!file)
	{
		snprintf(response, response_size, "SAVE OPEN FAILED: %s\nERRNO=%d\n", filename, errno);
		return;
	}

	if (fseek(file, offset, SEEK_SET))
	{
		fclose(file);
		snprintf(response, response_size, "SAVE SEEK FAILED: %s\nERRNO=%d\n", filename, errno);
		return;
	}

	size_t written = fwrite(bytes, 1, count, file);
	fclose(file);
	if (written != count)
	{
		snprintf(response, response_size, "SAVE WRITE FAILED: %s\n", filename);
		return;
	}

	snprintf(response, response_size, "OK\n");
}

static void build_cd_response(const char *name, char *response, size_t response_size)
{
	response[0] = 0;

	if (!name[0])
	{
		current_dir[0] = 0;
		append_current_dir(response, response_size);
		return;
	}

	char resolved[1024];
	if (!resolve_shared_relative_dir(name, resolved, sizeof(resolved)))
	{
		snprintf(response, response_size, "NO SUCH DIRECTORY: %s\n", name);
		return;
	}

	snprintf(current_dir, sizeof(current_dir), "%s", resolved);
	append_current_dir(response, response_size);
}

static void build_mkdir_response(const char *name, char *response, size_t response_size)
{
	response[0] = 0;

	if (!valid_shared_save_filename(name))
	{
		snprintf(response, response_size, "BAD DIRECTORY\n");
		return;
	}

	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));

	char path[1200];
	snprintf(path, sizeof(path), "%s/%s", basepath, name);

	struct stat st;
	if (!stat(path, &st))
	{
		snprintf(response, response_size, S_ISDIR(st.st_mode) ? "DIRECTORY EXISTS\n" : "FILE EXISTS\n");
		return;
	}

	if (mkdir(path, 0777))
	{
		snprintf(response, response_size, "MKDIR FAILED: %s\nERRNO=%d\n", name, errno);
		return;
	}

	snprintf(response, response_size, "Created: %s\n", name);
}

static void build_rename_response(const char *request, char *response, size_t response_size)
{
	response[0] = 0;

	if (request[0] != 'N' || request[1] != ':')
	{
		snprintf(response, response_size, "BAD RENAME REQUEST\n");
		return;
	}

	const char *old_name = request + 2;
	const char *separator = strchr(old_name, ':');
	if (!separator)
	{
		snprintf(response, response_size, "BAD RENAME REQUEST\n");
		return;
	}

	size_t old_len = separator - old_name;
	char old_filename[256];
	char new_filename[256];
	if (!old_len || old_len >= sizeof(old_filename))
	{
		snprintf(response, response_size, "BAD FILENAME\n");
		return;
	}

	memcpy(old_filename, old_name, old_len);
	old_filename[old_len] = 0;
	snprintf(new_filename, sizeof(new_filename), "%s", separator + 1);

	if (!valid_shared_save_filename(old_filename) || !valid_shared_save_filename(new_filename))
	{
		snprintf(response, response_size, "BAD FILENAME\n");
		return;
	}

	char basepath[1200];
	shared_current_path(basepath, sizeof(basepath));

	char old_path[1200];
	char new_path[1200];
	snprintf(old_path, sizeof(old_path), "%s/%s", basepath, old_filename);
	snprintf(new_path, sizeof(new_path), "%s/%s", basepath, new_filename);

	struct stat st;
	if (stat(old_path, &st))
	{
		snprintf(response, response_size, "NO SUCH FILE: %s\n", old_filename);
		return;
	}

	if (!stat(new_path, &st))
	{
		snprintf(response, response_size, "DESTINATION EXISTS: %s\n", new_filename);
		return;
	}

	if (rename(old_path, new_path))
	{
		snprintf(response, response_size, "RENAME FAILED: %s\nERRNO=%d\n", old_filename, errno);
		return;
	}

	snprintf(response, response_size, "Renamed: %s -> %s\n", old_filename, new_filename);
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

	if (!strncmp(request, "H:", 2))
	{
		uint8_t response[M4S_INDEX_SIZE];
		size_t response_len = build_header_load_response(request, response, sizeof(response));
		send_response(response, response_len);
	}
	else if (!strncmp(request, "L:", 2))
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
		else if (!strncmp(request, "C:", 2))
			build_cd_response(request + 2, response, sizeof(response));
		else if (!strncmp(request, "I:", 2))
			build_info_response(request + 2, response, sizeof(response));
		else if (!strncmp(request, "D:", 2))
			build_dump_response(request + 2, response, sizeof(response));
		else if (!strncmp(request, "K:", 2))
			build_mkdir_response(request + 2, response, sizeof(response));
		else if (!strncmp(request, "N:", 2))
			build_rename_response(request, response, sizeof(response));
		else if (!strncmp(request, "S:", 2))
			build_save_response(request, response, sizeof(response));
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
