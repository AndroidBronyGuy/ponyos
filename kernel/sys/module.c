#include <system.h>
#include <logging.h>
#include <fs.h>
#include <hashmap.h>
#include <elf.h>
#include <module.h>

#define SYMBOLTABLE_HASHMAP_SIZE 10
#define MODULE_HASHMAP_SIZE 10

static hashmap_t * symboltable = NULL;
static hashmap_t * modules = NULL;

typedef struct {
	uintptr_t addr;
	char name[];
} kernel_symbol_t;

extern char kernel_symbols_start[];
extern char kernel_symbols_end[];

int module_quickcheck(void * blob) {

	Elf32_Header * target = (Elf32_Header *)blob;

	if (target->e_ident[0] != ELFMAG0 ||
		target->e_ident[1] != ELFMAG1 ||
		target->e_ident[2] != ELFMAG2 ||
		target->e_ident[3] != ELFMAG3) {

		return 0;
	}

	return 1;
}

void * module_load_direct(void * blob, size_t length) {
	Elf32_Header * target = (Elf32_Header *)blob;

	if (target->e_ident[0] != ELFMAG0 ||
		target->e_ident[1] != ELFMAG1 ||
		target->e_ident[2] != ELFMAG2 ||
		target->e_ident[3] != ELFMAG3) {

		debug_print(ERROR, "Module is not a valid ELF object.");

		goto mod_load_error_unload;
	}

	char * shstrtab = NULL;
	char * symstrtab = NULL;
	Elf32_Shdr * sym_shdr = NULL;
	char * deps = NULL;
	size_t deps_length = 0;

	/* TODO: Actually load the ELF somewhere! This is moronic, you're not initializing a BSS! */
	/*       (and maybe keep the elf header somewhere) */

	{
		unsigned int i = 0;
		for (unsigned int x = 0; x < (unsigned int)target->e_shentsize * target->e_shnum; x += target->e_shentsize) {
			Elf32_Shdr * shdr = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + x));
			if (i == target->e_shstrndx) {
				shstrtab = (char *)((uintptr_t)target + shdr->sh_offset);
			}
			i++;
		}
	}
	if (!shstrtab) {
		debug_print(ERROR, "Could not locate module section header string table.");
		goto mod_load_error_unload;
	}

	{
		for (unsigned int x = 0; x < (unsigned int)target->e_shentsize * target->e_shnum; x += target->e_shentsize) {
			Elf32_Shdr * shdr = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + x));
			if (shdr->sh_type == SHT_STRTAB && (!strcmp((char *)((uintptr_t)shstrtab + shdr->sh_name), ".strtab"))) {
				symstrtab = (char *)((uintptr_t)target + shdr->sh_offset);
			}
		}
	}
	if (!shstrtab) {
		debug_print(ERROR, "Could not locate module symbol string table.");
		goto mod_load_error_unload;
	}

	{
		debug_print(INFO, "Checking dependencies.");
		for (unsigned int x = 0; x < (unsigned int)target->e_shentsize * target->e_shnum; x += target->e_shentsize) {
			Elf32_Shdr * shdr = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + x));
			if ((!strcmp((char *)((uintptr_t)shstrtab + shdr->sh_name), "moddeps"))) {
				deps = (char*)((Elf32_Addr)target + shdr->sh_offset);
				deps_length = shdr->sh_size;

				unsigned int i = 0;
				while (i < deps_length) {
					if (strlen(&deps[i]) && !hashmap_get(modules, &deps[i])) {
						debug_print(ERROR, "   %s - not loaded", &deps[i]);
						goto mod_load_error_unload;
					}
					debug_print(INFO, "   %s", &deps[i]);
					i += strlen(&deps[i]) + 1;
				}
			}
		}
	}

	{
		for (unsigned int x = 0; x < (unsigned int)target->e_shentsize * target->e_shnum; x += target->e_shentsize) {
			Elf32_Shdr * shdr = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + x));
			if (shdr->sh_type == SHT_SYMTAB) {
				sym_shdr = shdr;
			}
		}
	}
	if (!sym_shdr) {
		debug_print(ERROR, "Could not locate section for symbol table.");
		goto mod_load_error_unload;
	}

	{
		debug_print(INFO, "Loading sections.");
		for (unsigned int x = 0; x < (unsigned int)target->e_shentsize * target->e_shnum; x += target->e_shentsize) {
			Elf32_Shdr * shdr = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + x));
			if (shdr->sh_type == SHT_NOBITS) {
				shdr->sh_addr = (Elf32_Addr)malloc(shdr->sh_size);
				memset((void *)shdr->sh_addr, 0x00, shdr->sh_size);
			} else {
				shdr->sh_addr = (Elf32_Addr)target + shdr->sh_offset;
			}
		}
	}

	hashmap_t * local_symbols = hashmap_create(10);
	{
		Elf32_Sym * table = (Elf32_Sym *)((uintptr_t)target + sym_shdr->sh_offset);
		while ((uintptr_t)table - ((uintptr_t)target + sym_shdr->sh_offset) < sym_shdr->sh_size) {
			if (table->st_name) {
				if (ELF32_ST_BIND(table->st_info) == STB_GLOBAL) {
					char * name = (char *)((uintptr_t)symstrtab + table->st_name);
					if (table->st_shndx == 0) {
						if (!hashmap_get(symboltable, name)) {
							debug_print(ERROR, "Unresolved symbol in module: %s", name);
							debug_print(ERROR, "This module is faulty! Verify it specifies all of its");
							debug_print(ERROR, "dependencies properly with MODULE_DEPENDS.");
							goto mod_load_error;
						}
					} else {
						Elf32_Shdr * s = NULL;
						{
							int i = 0;
							for (unsigned int x = 0; x < (unsigned int)target->e_shentsize * target->e_shnum; x += target->e_shentsize) {
								Elf32_Shdr * shdr = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + x));
								if (i == table->st_shndx) {
									s = shdr;
									break;
								}
								i++;
							}
						}
						if (s) {
							uintptr_t final = s->sh_addr + table->st_value;
							hashmap_set(symboltable, name, (void *)final);
							hashmap_set(local_symbols, name, (void *)final);
						}
					}
				}
			}
			table++;
		}
	}

	{
		for (unsigned int x = 0; x < (unsigned int)target->e_shentsize * target->e_shnum; x += target->e_shentsize) {
			Elf32_Shdr * shdr = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + x));
			if (shdr->sh_type == SHT_REL) {
				Elf32_Rel * section_rel = (void *)(shdr->sh_addr);
				Elf32_Rel * table = section_rel;
				Elf32_Sym * symtable = (Elf32_Sym *)(sym_shdr->sh_addr);
				while ((uintptr_t)table - (shdr->sh_addr) < shdr->sh_size) {
					Elf32_Sym * sym = &symtable[ELF32_R_SYM(table->r_info)];
					Elf32_Shdr * rs = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + shdr->sh_info * target->e_shentsize));

					uintptr_t addend = 0;
					uintptr_t place  = 0;
					uintptr_t symbol = 0;
					uintptr_t *ptr   = NULL;

					if (ELF32_ST_TYPE(sym->st_info) == STT_SECTION) {
						Elf32_Shdr * s = (Elf32_Shdr *)((uintptr_t)target + (target->e_shoff + sym->st_shndx * target->e_shentsize));
						ptr = (uintptr_t *)(table->r_offset + rs->sh_addr);
						addend = *ptr;
						place  = (uintptr_t)ptr;
						symbol = s->sh_addr;
					} else {
						char * name = (char *)((uintptr_t)symstrtab + sym->st_name);
						ptr = (uintptr_t *)(table->r_offset + rs->sh_addr);
						addend = *ptr;
						place  = (uintptr_t)ptr;
						symbol = (uintptr_t)hashmap_get(symboltable, name);
					}
					switch (ELF32_R_TYPE(table->r_info)) {
						case 1:
							*ptr = addend + symbol;
							break;
						case 2:
							*ptr = addend + symbol - place;
							break;
						default:
							debug_print(ERROR, "Unsupported relocation type: %d", ELF32_R_TYPE(table->r_info));
							goto mod_load_error;
					}

					table++;
				}
			}
		}
	}

	debug_print(INFO, "Locating module information...");
	module_defs * mod_info = NULL;
	list_t * hash_keys = hashmap_keys(local_symbols);
	foreach(_key, hash_keys) {
		char * key = (char *)_key->value;
		if (startswith(key, "module_info_")) {
			mod_info = hashmap_get(local_symbols, key);
		}
	}
	list_free(hash_keys);
	free(hash_keys);
	if (!mod_info) {
		debug_print(ERROR, "Failed to locate module information structure!");
		goto mod_load_error;
	}

	mod_info->initialize();

	debug_print(NOTICE, "Finished loading module %s", mod_info->name);

	/* We don't do this anymore
	 * TODO: Do this in the module unload function
	hashmap_free(local_symbols);
	free(local_symbols);
	*/

	module_data_t * mod_data = malloc(sizeof(module_data_t));
	mod_data->mod_info = mod_info;
	mod_data->bin_data = target;
	mod_data->symbols  = local_symbols;
	mod_data->end      = (uintptr_t)target + length;
	mod_data->deps     = deps;
	mod_data->deps_length = deps_length;

	hashmap_set(modules, mod_info->name, (void *)mod_data);

	return mod_data;

mod_load_error_unload:
	return (void *)-1;

mod_load_error:
	return NULL;
}

/**
 * Install a module from a file and return
 * a pointer to its module_info structure.
 */
void * module_load(char * filename) {
	fs_node_t * file = kopen(filename, 0);
	if (!file) {
		debug_print(ERROR, "Failed to load module: %s", filename);
		return NULL;
	}

	debug_print(NOTICE, "Attempting to load kernel module: %s", filename);

	void * blob = (void *)kvmalloc(file->length);
	read_fs(file, 0, file->length, (uint8_t *)blob);

	void * result = module_load_direct(blob, file->length);

	if (result == (void *)-1) {
		debug_print(ERROR, "Error loading module.");
		free(blob);
		result = NULL;
	}

	close_fs(file);
	return result;
}

/**
 * Remove a loaded module.
 */
void module_unload(char * name) {
	/* XXX: Lookup the module by name and verify it has no dependencies loaded. */
	/* XXX: Call module_info->finish() */
	/* XXX: Unmap symbols defined the module that weren't otherwise defined. */
	/* XXX: Deallocate the regions the module was mapped into */
}

void modules_install(void) {
	/* Initialize the symboltable, we use a hashmap of symbols to addresses  */
	symboltable = hashmap_create(SYMBOLTABLE_HASHMAP_SIZE);

	/* Load all of the kernel symbols into the symboltable */
	kernel_symbol_t * k = (kernel_symbol_t *)&kernel_symbols_start;

	while ((uintptr_t)k < (uintptr_t)&kernel_symbols_end) {
		hashmap_set(symboltable, k->name, (void *)k->addr);
		k = (kernel_symbol_t *)((uintptr_t)k + sizeof(kernel_symbol_t) + strlen(k->name) + 1);
	}

	/* Also add the kernel_symbol_start and kernel_symbol_end (these were excluded from the generator) */
	hashmap_set(symboltable, "kernel_symbols_start", &kernel_symbols_start);
	hashmap_set(symboltable, "kernel_symbols_end",   &kernel_symbols_end);

	/* Initialize the module name -> object hashmap */
	modules = hashmap_create(MODULE_HASHMAP_SIZE);
}

/* Accessors. */
hashmap_t * modules_get_list(void) {
	return modules;
}

hashmap_t * modules_get_symbols(void) {
	return symboltable;
}
