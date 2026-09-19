#include "core/embedded.h"
#include <string.h>

extern const pf_embedded_file pf_share_files[];
extern const size_t pf_share_count;

const pf_embedded_file *pf_embedded_share(const char *name)
{
	for (size_t i = 0; i < pf_share_count; i++)
		if (!strcmp(pf_share_files[i].name, name)) return &pf_share_files[i];
	return NULL;
}
