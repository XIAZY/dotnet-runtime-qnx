/**
 * \file
 * QNX: load AOT images (*.dll.so) without dlopen, so that only the pages used
 * cost memory.
 *
 * QNX 6.5's loader commits every page of a shared library when it loads it,
 * read from disk, whether the program uses it or not. PowerShell touches
 * about a third of its AOT images' pages. This loader maps an image the way
 * measurements on QNX 6.5.0 showed to be paid per page: the read-execute segment MAP_SHARED|MAP_LAZY (shared with
 * other processes), the read-write segment MAP_PRIVATE|MAP_LAZY (copied only
 * where written), both over one PROT_NONE reservation of the image's span so
 * that nothing else can be mapped between them.
 *
 * It handles exactly what the AOT compiler emits for QNX, and refuses
 * anything else, in which case mono_dl_open () falls back to dlopen: a 32-bit
 * x86 ET_DYN with two LOAD segments (R+X, then R+W), only R_386_RELATIVE
 * relocations, all in the writable segment, no initialisers, no needed
 * libraries, no TLS, and the symbols looked up through the SysV hash table.
 *
 * MONO_QNX_AOT_LOADER=dlopen disables it; MONO_QNX_AOT_LOADER_VERBOSE prints
 * a line for each image it loads or refuses. The launcher sets both from
 * QNXHOST_AOT_LOADER and QNXHOST_VERBOSE. Native debuggers and dladdr () do
 * not know the images it maps.
 *
 * Copyright (c) Xia Zhongyang.
 * Licensed under the MIT License.
 */

#include <config.h>

#ifdef HOST_QNX

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <glib.h>
#include "mono/utils/mono-dl-qnx.h"

/* The ELF structures and values used, from the ELF and i386 psABI specifications. */
typedef struct {
	unsigned char e_ident [16];
	uint16_t e_type, e_machine;
	uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} QnxElfEhdr;

typedef struct {
	uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} QnxElfPhdr;

typedef struct {
	int32_t d_tag;
	uint32_t d_val;
} QnxElfDyn;

typedef struct {
	uint32_t st_name, st_value, st_size;
	unsigned char st_info, st_other;
	uint16_t st_shndx;
} QnxElfSym;

typedef struct {
	uint32_t r_offset, r_info;
} QnxElfRel;

enum {
	QNX_ET_DYN = 3, QNX_EM_386 = 3,
	QNX_PT_LOAD = 1, QNX_PT_DYNAMIC = 2, QNX_PT_INTERP = 3, QNX_PT_TLS = 7,
	QNX_PF_X = 1, QNX_PF_W = 2, QNX_PF_R = 4,
	QNX_DT_NULL = 0, QNX_DT_NEEDED = 1, QNX_DT_PLTRELSZ = 2, QNX_DT_HASH = 4, QNX_DT_STRTAB = 5,
	QNX_DT_SYMTAB = 6, QNX_DT_RELA = 7, QNX_DT_INIT = 12, QNX_DT_FINI = 13, QNX_DT_REL = 17,
	QNX_DT_RELSZ = 18, QNX_DT_RELENT = 19, QNX_DT_TEXTREL = 22, QNX_DT_JMPREL = 23,
	QNX_DT_INIT_ARRAY = 25, QNX_DT_FINI_ARRAY = 26, QNX_DT_FLAGS = 30, QNX_DT_PREINIT_ARRAY = 32,
	QNX_DF_TEXTREL = 4,
	QNX_R_386_RELATIVE = 8,
	QNX_PAGE = 4096,
};

struct _MonoQnxImage {
	uint8_t *base;
	size_t span;
	const QnxElfSym *symtab;
	const char *strtab;
	const uint32_t *hash;
};

#define PAGE_DOWN(x) ((x) & ~(uint32_t)(QNX_PAGE - 1))
#define PAGE_UP(x) (((x) + QNX_PAGE - 1) & ~(uint32_t)(QNX_PAGE - 1))

static gboolean
verbose (void)
{
	return g_getenv ("MONO_QNX_AOT_LOADER_VERBOSE") != NULL;
}

/* Fails the load with a one-line reason, under verbose mode. */
static MonoQnxImage *
refuse (const char *path, const char *reason, int fd, uint8_t *base, size_t span)
{
	if (verbose ())
		fprintf (stderr, "qnx aot loader: %s: %s; using dlopen\n", path, reason);
	if (base != NULL)
		munmap (base, span);
	if (fd >= 0)
		close (fd);
	return NULL;
}

MonoQnxImage *
mono_qnx_image_open (const char *path)
{
	QnxElfEhdr eh;
	QnxElfPhdr ph [16];
	const QnxElfPhdr *text = NULL, *data = NULL, *dynamic = NULL;
	uint8_t *base = NULL;
	size_t span = 0;
	const char *loader = g_getenv ("MONO_QNX_AOT_LOADER");
	int fd;

	if (loader != NULL && strcmp (loader, "dlopen") == 0)
		return NULL;

	/* A file mapped executable cannot be opened for writing (ETXTBSY), so read-only. */
	fd = open (path, O_RDONLY);
	if (fd < 0)
		return NULL; /* dlopen reports the error */

	if (pread (fd, &eh, sizeof (eh), 0) != sizeof (eh) || memcmp (eh.e_ident, "\177ELF\1\1", 6) != 0 ||
	    eh.e_type != QNX_ET_DYN || eh.e_machine != QNX_EM_386 || eh.e_phentsize != sizeof (QnxElfPhdr) ||
	    eh.e_phnum == 0 || eh.e_phnum > G_N_ELEMENTS (ph))
		return refuse (path, "not a 32-bit x86 shared object of the expected shape", fd, NULL, 0);
	if (pread (fd, ph, eh.e_phnum * sizeof (QnxElfPhdr), eh.e_phoff) != (ssize_t)(eh.e_phnum * sizeof (QnxElfPhdr)))
		return refuse (path, "short program header table", fd, NULL, 0);

	for (int i = 0; i < eh.e_phnum; ++i) {
		const QnxElfPhdr *p = &ph [i];
		if (p->p_type == QNX_PT_LOAD) {
			if (text == NULL && p->p_flags == (QNX_PF_R | QNX_PF_X))
				text = p;
			else if (text != NULL && data == NULL && p->p_flags == (QNX_PF_R | QNX_PF_W))
				data = p;
			else
				return refuse (path, "LOAD segments other than one R+X followed by one R+W", fd, NULL, 0);
		} else if (p->p_type == QNX_PT_DYNAMIC) {
			dynamic = p;
		} else if (p->p_type == QNX_PT_TLS || p->p_type == QNX_PT_INTERP) {
			return refuse (path, "TLS or INTERP segment", fd, NULL, 0);
		}
	}
	if (text == NULL || data == NULL || dynamic == NULL)
		return refuse (path, "missing text, data or dynamic segment", fd, NULL, 0);
	if (text->p_vaddr != 0 || text->p_offset != 0 || text->p_memsz != text->p_filesz ||
	    data->p_vaddr % QNX_PAGE != data->p_offset % QNX_PAGE || data->p_memsz < data->p_filesz ||
	    PAGE_DOWN (data->p_vaddr) < PAGE_UP (text->p_memsz) ||
	    dynamic->p_vaddr < data->p_vaddr || dynamic->p_vaddr + dynamic->p_memsz > data->p_vaddr + data->p_filesz)
		return refuse (path, "unexpected segment layout", fd, NULL, 0);

	/* One reservation of the whole span, then the segments over it. */
	span = PAGE_UP (data->p_vaddr + data->p_memsz);
	base = (uint8_t *)mmap (NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_LAZY, -1, 0);
	if (base == MAP_FAILED)
		return refuse (path, "cannot reserve the address range", fd, NULL, 0);
	if (mmap (base, PAGE_UP (text->p_memsz), PROT_READ | PROT_EXEC, MAP_SHARED | MAP_LAZY | MAP_FIXED, fd, 0) != base)
		return refuse (path, "cannot map the text segment", fd, base, span);

	uint32_t data_start = PAGE_DOWN (data->p_vaddr);
	uint32_t file_end = data->p_vaddr + data->p_filesz;
	uint32_t file_pages_end = PAGE_UP (file_end);
	uint32_t mem_end = PAGE_UP (data->p_vaddr + data->p_memsz);
	if (mmap (base + data_start, file_pages_end - data_start, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_LAZY | MAP_FIXED,
		  fd, PAGE_DOWN (data->p_offset)) != base + data_start)
		return refuse (path, "cannot map the data segment", fd, base, span);
	/* .bss: the rest of the last file page, then anonymous pages */
	memset (base + file_end, 0, file_pages_end - file_end);
	if (mem_end > file_pages_end &&
	    mmap (base + file_pages_end, mem_end - file_pages_end, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_LAZY | MAP_FIXED,
		  -1, 0) != base + file_pages_end)
		return refuse (path, "cannot map .bss", fd, base, span);
	close (fd);
	fd = -1;

	/* The dynamic section: relocations and symbols only. */
	uint32_t rel = 0, relsz = 0, relent = sizeof (QnxElfRel), symtab = 0, strtab = 0, hash = 0;
	for (const QnxElfDyn *d = (const QnxElfDyn *)(base + dynamic->p_vaddr);
	     (const uint8_t *)(d + 1) <= base + dynamic->p_vaddr + dynamic->p_memsz && d->d_tag != QNX_DT_NULL; ++d) {
		switch (d->d_tag) {
		case QNX_DT_REL: rel = d->d_val; break;
		case QNX_DT_RELSZ: relsz = d->d_val; break;
		case QNX_DT_RELENT: relent = d->d_val; break;
		case QNX_DT_SYMTAB: symtab = d->d_val; break;
		case QNX_DT_STRTAB: strtab = d->d_val; break;
		case QNX_DT_HASH: hash = d->d_val; break;
		case QNX_DT_NEEDED:
		case QNX_DT_INIT: case QNX_DT_FINI:
		case QNX_DT_INIT_ARRAY: case QNX_DT_FINI_ARRAY: case QNX_DT_PREINIT_ARRAY:
		case QNX_DT_TEXTREL: case QNX_DT_RELA: case QNX_DT_JMPREL:
			return refuse (path, "needed libraries, initialisers, text, RELA or PLT relocations", -1, base, span);
		case QNX_DT_PLTRELSZ:
			if (d->d_val != 0)
				return refuse (path, "PLT relocations", -1, base, span);
			break;
		case QNX_DT_FLAGS:
			if (d->d_val & QNX_DF_TEXTREL)
				return refuse (path, "text relocations", -1, base, span);
			break;
		default:
			break; /* sizes, counts and version tags need nothing */
		}
	}
	if (symtab == 0 || strtab == 0 || hash == 0 || symtab >= text->p_memsz || strtab >= text->p_memsz ||
	    hash + 8 > text->p_memsz || relent != sizeof (QnxElfRel) || relsz % sizeof (QnxElfRel) != 0 ||
	    (relsz != 0 && rel + relsz > text->p_memsz))
		return refuse (path, "missing symbol table, string table or hash table, or bad relocation table", -1, base, span);

	/* Check every relocation before applying any, so a refusal leaves nothing half done. */
	const QnxElfRel *rels = (const QnxElfRel *)(base + rel);
	size_t nrel = relsz / sizeof (QnxElfRel);
	for (size_t i = 0; i < nrel; ++i) {
		if (rels [i].r_info != QNX_R_386_RELATIVE ||
		    rels [i].r_offset < data->p_vaddr || rels [i].r_offset + 4 > data->p_vaddr + data->p_filesz)
			return refuse (path, "a relocation other than R_386_RELATIVE in the data segment", -1, base, span);
	}
	for (size_t i = 0; i < nrel; ++i) {
		uint32_t *where = (uint32_t *)(base + rels [i].r_offset);
		*where += (uint32_t)(uintptr_t)base;
	}

	MonoQnxImage *image = g_new0 (MonoQnxImage, 1);
	image->base = base;
	image->span = span;
	image->symtab = (const QnxElfSym *)(base + symtab);
	image->strtab = (const char *)(base + strtab);
	image->hash = (const uint32_t *)(base + hash);
	if (verbose ())
		fprintf (stderr, "qnx aot loader: %s: mapped at %p, %zu bytes, %zu relocations\n", path, base, span, nrel);
	return image;
}

static uint32_t
elf_hash (const char *name)
{
	uint32_t h = 0, g;
	for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
		h = (h << 4) + *p;
		g = h & 0xf0000000;
		if (g)
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

void *
mono_qnx_image_symbol (MonoQnxImage *image, const char *name)
{
	uint32_t nbucket = image->hash [0], nchain = image->hash [1];
	const uint32_t *bucket = image->hash + 2, *chain = bucket + nbucket;

	if (nbucket == 0)
		return NULL;
	for (uint32_t i = bucket [elf_hash (name) % nbucket]; i != 0 && i < nchain; i = chain [i]) {
		const QnxElfSym *sym = &image->symtab [i];
		if (sym->st_shndx != 0 && strcmp (image->strtab + sym->st_name, name) == 0)
			return image->base + sym->st_value;
	}
	return NULL;
}

void
mono_qnx_image_close (MonoQnxImage *image)
{
	munmap (image->base, image->span);
	g_free (image);
}

#endif /* HOST_QNX */
