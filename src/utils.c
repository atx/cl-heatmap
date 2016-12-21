
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <bsd/string.h>
#include <string.h>

#include "utils.h"

int file_read_whole(char *path, char **data, size_t *len)
{
	size_t llen;
	if (len == NULL) {
		len = &llen;
	}

	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return -1;

	struct stat st;
	fstat(fileno(f), &st);
	*len = st.st_size;

	rewind(f);
	*data = malloc(*len);
	if (*data == NULL) {
		return -1;
	}

	fread(*data, 1, *len, f);
	fclose(f);

	return 0;
}

int mkdir_recursive(char *path, mode_t mode)
{
	char *save;
	char dir[PATH_MAX];
	bzero(dir, sizeof(dir));
	while (true) {
		char *tok = strtok_r(path, "/", &save);
		path = NULL;
		if (tok == NULL) {
			break;
		}
		strlcat(dir, tok, sizeof(dir));
		strlcat(dir, "/", sizeof(dir));
		int ret = mkdir(dir, mode);
		if (ret <= 0 && errno != EEXIST) {
			perror("Failed to mkdir!");
			return ret;
		}

	}
	return 0;
}
