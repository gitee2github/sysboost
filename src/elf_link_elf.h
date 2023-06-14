/* SPDX-License-Identifier: MulanPSL-2.0 */
#ifndef _LINK_ELF_H
#define _LINK_ELF_H

#include "elf_link_common.h"
#include "elf_read_elf.h"
#include <si_array.h>
#include <si_common.h>

elf_link_t *elf_link_new(void);
int elf_link_set_mode(elf_link_t *elf_link, unsigned int mode);
elf_file_t *elf_link_add_infile(elf_link_t *elf_link, char *name);
int elf_link_write(elf_link_t *elf_link);

#endif /* _LINK_ELF_H */
