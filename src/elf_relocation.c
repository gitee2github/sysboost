// Copyright (c) 2023 Huawei Technologies Co.,Ltd. All rights reserved.
//
// sysboost is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan
// PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
//         http://license.coscl.org.cn/MulanPSL2
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PSL v2 for more details.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <si_debug.h>
#include <si_log.h>

#include "elf_link_common.h"
#include "elf_relocation.h"

#define BYTES_NOP1 0x90

#define INDIRECT_CALL_INSN_OP_SIZE 2

#define CALL_INSN_SIZE 5
#define CALL_INSN_OPCODE 0xE8

#define JMP32_INSN_SIZE 5
#define JMP32_INSN_OPCODE 0xE9

#define MAX_INSN_OFFSET 2147483647L
#define MIN_INSN_OFFSET (-2147483648L)

static void modify_local_call_sec(elf_link_t *elf_link, elf_file_t *ef, Elf64_Shdr *sec)
{
	char *name = NULL;
	int len = sec->sh_size / sec->sh_entsize;
	Elf64_Rela *relas = (void *)ef->hdr + sec->sh_offset;
	Elf64_Rela *rela = NULL;
	int ret = 0;

	name = elf_get_section_name(ef, sec);
	SI_LOG_DEBUG("sec %s\n", name);

	for (int i = 0; i < len; i++) {
		rela = &relas[i];
		ret = modify_local_call_rela(elf_link, ef, rela);
		if (ret > 0) {
			// retrun value tell skip num
			i += ret;
		}
	}
}

static bool is_rela_for_A(elf_file_t *ef, Elf64_Shdr *sec)
{
	Elf64_Shdr *target_sec = NULL;

	if (sec->sh_flags & SHF_ALLOC) {
		return false;
	}
	if (sec->sh_info == 0) {
		return false;
	}
	target_sec = &ef->sechdrs[sec->sh_info];
	if (target_sec->sh_flags & SHF_ALLOC) {
		return true;
	}

	return false;
}

static void modify_local_call_ef(elf_link_t *elf_link, elf_file_t *ef)
{
	Elf64_Shdr *sechdrs = ef->sechdrs;
	unsigned int shnum = ef->hdr->e_shnum;
	unsigned int i;
	Elf64_Shdr *sec = NULL;

	for (i = 1; i < shnum; i++) {
		sec = &sechdrs[i];
		// rela sec is not alloc and sh_info is alloc sec, .rela.text
		// sh_info for SHT_SYMTAB is the first non-local symbol index
		if (sechdrs[i].sh_type != SHT_RELA || !is_rela_for_A(ef, sec)) {
			continue;
		}

		modify_local_call_sec(elf_link, ef, sec);
	}
}

void modify_local_call(elf_link_t *elf_link)
{
	elf_file_t *ef;
	int count = elf_link->in_ef_nr;

	for (int i = 0; i < count; i++) {
		ef = &elf_link->in_efs[i];
		modify_local_call_ef(elf_link, ef);
	}
}

static void rela_change_to_relative(Elf64_Rela *dst_rela, unsigned long addend)
{
	dst_rela->r_addend = addend;

#ifdef __aarch64__
	dst_rela->r_info = ELF64_R_INFO(0, ELF64_R_TYPE(R_AARCH64_RELATIVE));
#else
	dst_rela->r_info = ELF64_R_INFO(0, ELF64_R_TYPE(R_X86_64_RELATIVE));
#endif

	// offset modify by caller
}

// The __stack_chk_guard and __stack_chk_fail symbols are normally supplied by a GCC library called libssp
// we can not change code to direct access the symbol, some code use 2 insn to point symbol, the adrp insn may be shared
static void modify_rela_to_RELATIVE(elf_link_t *elf_link, elf_file_t *src_ef, Elf64_Rela *src_rela, Elf64_Rela *dst_rela)
{
	// some symbol do not export in .dynsym, change to R_AARCH64_RELATIVE
	Elf64_Sym *sym = elf_get_dynsym_by_rela(src_ef, src_rela);
	unsigned long ret = get_new_addr_by_symobj(elf_link, src_ef, sym);
	if (ret == NOT_FOUND) {
		// 1008:	48 8b 05 d9 2f 00 00 	mov    0x2fd9(%rip),%rax        # 3fe8 <__gmon_start__@Base>
		// some addr need be 0, use by cmp jump
		char *name = elf_get_sym_name(src_ef, sym);
		if (!is_symbol_maybe_undefined(name)) {
			si_panic("%s\n", name);
		}
		// do nothing
		return;
	}

	rela_change_to_relative(dst_rela, ret);
}

static void rela_use_relative(elf_link_t *elf_link, elf_file_t *src_ef, Elf64_Rela *src_rela, Elf64_Rela *dst_rela)
{
	// 000000000012dd60  000001b900000005 R_X86_64_COPY          000000000012dd60 stdout@GLIBC_2.2.5 + 0
	// 441: 000000000012dd60     8 OBJECT  GLOBAL DEFAULT   36 stdout@GLIBC_2.2.5 (2)
	// libc: 1407: 00000000001ed688     8 OBJECT  GLOBAL DEFAULT   36 stdout@@GLIBC_2.2.5
	// copy symbol data to bss area

	elf_file_t *ef = get_libc_ef(elf_link);
	Elf64_Sym *sym = elf_get_dynsym_by_rela(src_ef, src_rela);
	if (elf_is_copy_symbol(src_ef, sym) == false) {
		// use local symbol addr
		ef = src_ef;
	}

	char *sym_name = elf_get_sym_name(src_ef, sym);
	unsigned long old_sym_addr = elf_find_symbol_addr_by_name(ef, sym_name);
	unsigned long new_sym_addr = get_new_addr_by_old_addr(elf_link, ef, old_sym_addr);
	if (new_sym_addr == NOT_FOUND) {
		si_panic("%s %lx\n", src_ef->file_name, src_rela->r_offset);
		return;
	}

	unsigned long data = elf_read_u64_va(ef, new_sym_addr);
	// check copy size
	if (sym->st_size == sizeof(unsigned long)) {
		// 8 byte
	} else if (sym->st_size == sizeof(unsigned char)) {
		// NOTE: target mem is bss, so relative type write 7 zero is OK
		// 1 byte
		data = (unsigned char)data;
	} else {
		si_panic("size wrong %s %lx\n", src_ef->file_name, src_rela->r_offset);
		return;
	}

	rela_change_to_relative(dst_rela, data);
}

void modify_rela_dyn_item(elf_link_t *elf_link, elf_file_t *src_ef, Elf64_Rela *src_rela, Elf64_Rela *dst_rela)
{
	int type;
	Elf64_Sym *sym = elf_get_dynsym_by_rela(src_ef, src_rela);

	// modify offset
	dst_rela->r_offset = get_new_addr_by_old_addr(elf_link, src_ef, src_rela->r_offset);
	// old sym index to new index of .dynsym
	unsigned int old_index = ELF64_R_SYM(src_rela->r_info);
	int new_index = get_new_sym_index_no_clear(elf_link, src_ef, old_index);
	dst_rela->r_info = ELF64_R_INFO(new_index, ELF64_R_TYPE(src_rela->r_info));

	type = ELF64_R_TYPE(src_rela->r_info);
	switch (type) {
	case R_X86_64_GLOB_DAT:
		// set addr of so path list
		if (elf_link->hook_func) {
			// .got var point to ___g_so_path_list data area, change point to real addr
			// .rela.dyn
			// 0000000000003ff0  0000003000000006 R_X86_64_GLOB_DAT      0000000000004000 ___g_so_path_list + 0
			// .rela.text
			// 000000000000129d  0000006e0000002a R_X86_64_REX_GOTPCRELX 0000000000004000 ___g_so_path_list - 4
			// 129a:	4c 8b 2d 4f 2d 00 00 	mov    0x2d4f(%rip),%r13        # 3ff0 <___g_so_path_list@@Base-0x10>
			// 48: 0000000000004000  4096 OBJECT  GLOBAL DEFAULT   27 ___g_so_path_list
			new_index = ELF64_R_SYM(dst_rela->r_info);
			const char *sym_name = elf_get_dynsym_name_by_index(&elf_link->out_ef, new_index);
			if (elf_is_same_symbol_name(sym_name, "___g_so_path_list")) {
				// when ELF load, real addr will set
				dst_rela->r_info = ELF64_R_INFO(new_index, ELF64_R_TYPE(R_X86_64_RELATIVE));
				dst_rela->r_addend = (unsigned long)elf_link->so_path_struct;
				break;
			}
		}
		fallthrough;
	case R_AARCH64_GLOB_DAT:
		// some symbol do not export in .dynsym, change to R_AARCH64_RELATIVE
		modify_rela_to_RELATIVE(elf_link, src_ef, src_rela, dst_rela);
		break;
	case R_X86_64_64:
	case R_AARCH64_ABS64:
		// [44] .bss              NOBITS          00000000001f3520 1f2510 00d590 00  WA  0   0 32
		// 00000000001f2698  0000000e00000001 R_X86_64_64            0000000000000000 _rtld_global@GLIBC_PRIVATE + 0
		// 14: 0000000000000000     0 OBJECT  GLOBAL DEFAULT  UND _rtld_global@GLIBC_PRIVATE (37)
		if ((ELF64_ST_TYPE(sym->st_info) == STT_FUNC) || (ELF64_ST_TYPE(sym->st_info) == STT_OBJECT)) {
			modify_rela_to_RELATIVE(elf_link, src_ef, src_rela, dst_rela);
		}
		break;
	case R_X86_64_IRELATIVE:
		// 000000000002f9e0  0000000000000025 R_X86_64_IRELATIVE                        15ec0
		// 129: 0000000000015ec0    40 FUNC    LOCAL  DEFAULT   13 __x86_cpu_features_ifunc
		fallthrough;
	case R_X86_64_RELATIVE:
	case R_AARCH64_RELATIVE:
		dst_rela->r_addend = get_new_addr_by_old_addr(elf_link, src_ef, src_rela->r_addend);
		break;
	case R_AARCH64_TLS_TPREL:
		// all TLS got entry will be modified directly when processing instructions later,
		// so no .dyn.rela entry is needed.
		dst_rela->r_info = ELF64_R_INFO(0, R_AARCH64_NONE);
		break;
	case R_X86_64_TPOFF64:
	case R_X86_64_TPOFF32:
		// Offset in initial TLS block
		// 00000000001f0d78  0000000000000012 R_X86_64_TPOFF64                          38
		dst_rela->r_addend = elf_get_new_tls_offset(elf_link, src_ef, src_rela->r_addend);
		break;
	case R_X86_64_COPY:
		rela_use_relative(elf_link, src_ef, src_rela, dst_rela);
		break;
	case R_AARCH64_COPY:
		// Variables in the bss section, some from glibc, some declared by the application
		// Redefined in the template file temporarily, so skip here
	case R_AARCH64_NONE:
		/* nothing need to do */
		break;
	default:
		SI_LOG_ERR("%s %lx\n", src_ef->file_name, src_rela->r_offset);
		si_panic("error not supported modify_rela_dyn type %d\n", type);
	}

	SI_LOG_DEBUG("old r_offset %016lx r_info %016lx r_addend %016lx -> new r_offset %016lx r_info %016lx r_addend %016lx\n",
		     src_rela->r_offset, src_rela->r_info, src_rela->r_addend,
		     dst_rela->r_offset, dst_rela->r_info, dst_rela->r_addend);
}

// .rela.dyn
void modify_rela_dyn(elf_link_t *elf_link)
{
	int len = elf_link->rela_dyn_arr->len;
	elf_obj_mapping_t *obj_rels = elf_link->rela_dyn_arr->data;
	elf_obj_mapping_t *obj_rel = NULL;

	for (int i = 0; i < len; i++) {
		obj_rel = &obj_rels[i];
		Elf64_Rela *src_rela = obj_rel->src_obj;
		Elf64_Rela *dst_rela = obj_rel->dst_obj;
		elf_file_t *src_ef = obj_rel->src_ef;
		modify_rela_dyn_item(elf_link, src_ef, src_rela, dst_rela);
	}
}

void modify_got(elf_link_t *elf_link)
{
	Elf64_Shdr *got_sec = find_tmp_section_by_name(elf_link, ".got");
	Elf64_Shdr *find_sec = find_tmp_section_by_name(elf_link, ".dynamic");
	void *got_addr = NULL;

	// got[0] is .dynamic addr
	// TODO: clean code, aarch64 got[0] is zero when link
	got_addr = ((void *)elf_link->out_ef.hdr) + got_sec->sh_offset;
	if (is_share_mode(elf_link)) {
		*(unsigned long *)got_addr = find_sec->sh_addr;
	}

	// modify _GLOBAL_OFFSET_TABLE_ point value, offset .dynamic to ELF header
	// _GLOBAL_OFFSET_TABLE_[0] used by _dl_relocate_static_pie to get link_map->l_addr
	//   2006: 00000000003ffbd8     0 OBJECT  LOCAL  DEFAULT  ABS _GLOBAL_OFFSET_TABLE_
	elf_file_t *template_ef = get_template_ef(elf_link);
	Elf64_Sym *sym = elf_find_symbol_by_name(template_ef, "_GLOBAL_OFFSET_TABLE_");
	unsigned long new_addr = get_new_addr_by_old_addr(elf_link, template_ef, sym->st_value);
	elf_file_t *out_ef = &elf_link->out_ef;
	elf_write_u64(out_ef, new_addr, find_sec->sh_addr);

	// modify .rela.plt
	modify_rela_plt(elf_link, elf_link->rela_plt_arr);

	// modify .plt.got
	modify_plt_got(elf_link);
}
