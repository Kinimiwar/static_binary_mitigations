/*
 * Copyright (c) 2018, Ryan O'Neill
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * gcc relros.c -o relros
 * ./relros <target_executable>
 */

#define _GNU_SOURCE

#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PADDING_SIZE 1024
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define PAGE_ALIGN(x) (x & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x) (PAGE_ALIGN(x) + PAGE_SIZE)
#define PAGE_ROUND(x) (PAGE_ALIGN_UP(x))


struct segment {
	uint64_t vaddr;
	uint64_t offset;
	uint64_t memsz;
	uint64_t filesz;
};

/*
 * We have removed all calls to libelfmaster since it is not yet open sourced and
 * are using a minimalistic set of code for resolving symbols.
 */
typedef struct elfobj2 {
	uint8_t *mem;
	ElfW(Ehdr) *ehdr;
	ElfW(Phdr) *phdr;
	ElfW(Shdr) *shdr;
	ElfW(Sym) *symtab;
	bool dynamic_linked;
	size_t symcount;
	int fd;
	char *shstrtab;
	char *strtab;
	struct stat st;
	uint64_t text_offset;
	uint64_t text_base;
	size_t size;
	char *path;
} elfobj2_t;

bool _elf_symbol_by_name(elfobj2_t *, char *, ElfW(Sym) *);
void * _elf_address_pointer(elfobj2_t *, uint64_t);
bool _elf_open_object(char *, elfobj2_t *);

#define IP_RELATIVE_ADDR(target) \
    (get_rip() - ((char *)&get_rip_label - (char *)target))

extern long get_rip_label;

unsigned long get_rip(void)
{
        long ret;
        __asm__ __volatile__ 
        (
        "call get_rip_label     \n"
        ".globl get_rip_label   \n"
        "get_rip_label:         \n"
        "pop %%rax              \n"
        "mov %%rax, %0" : "=r"(ret)
        );

        return ret;
}
#if DEBUG
static inline __attribute__((always_inline)) long
__write(long fd, char *buf, unsigned long len)
{
	long ret;
        __asm__ volatile(
		"mov %0, %%rdi\n"
		"mov %1, %%rsi\n"
		"mov %2, %%rdx\n"
		"mov $1, %%rax\n"
		"syscall" : : "g"(fd), "g"(buf), "g"(len));
        asm volatile("mov %%rax, %0" : "=r"(ret));
        return ret;
}
#endif

void *
_elf_text_pointer(elfobj2_t *obj, uint64_t addr)
{
	uint64_t offset = obj->text_offset + addr - obj->text_base;

	printf("%lx - %lx = %lx\n", addr, obj->text_base, addr - obj->text_base);
	if (offset > obj->size - 1)
		return NULL;
	return (void *)((uint8_t *)&obj->mem[offset]);
}

bool
_elf_symbol_by_name(elfobj2_t *obj, char *name, ElfW(Sym) *out)
{

	ElfW(Sym) *sym = obj->symtab;
	char *strtab = obj->strtab;
	char *tmp;
	int i;

	for (i = 0; i < obj->symcount; i++) {
		if (strcmp(&strtab[sym[i].st_name], name) == 0) {
			memcpy(out, &sym[i], sizeof(*out));
			return true;
		}
	}
	return false;
}

bool
_elf_close_object(elfobj2_t *obj)
{
	int ret;

	ret = munmap(obj->mem, obj->size);
	return ret != 0 ? false : true;
}

bool
_elf_open_object(char *path, struct elfobj2 *obj)
{
	int fd, i;
	ElfW(Ehdr) *ehdr;
	ElfW(Phdr) *phdr;
	ElfW(Shdr) *shdr;
	ElfW(Sym) *symtab;
	char *shstrtab;
	char *strtab;
	struct stat st;
	uint8_t *mem;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror("open");
		return false;
	}
	if (fstat(fd, &st) < 0) {
		perror("fstat");
		return false;
	}
	memset(obj, 0, sizeof(*obj));

	mem = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return false;
	}
	ehdr = (ElfW(Ehdr) *)mem;
	phdr = (ElfW(Phdr) *)&mem[ehdr->e_phoff];
	shdr = (ElfW(Shdr) *)&mem[ehdr->e_shoff];
	shstrtab = (char *)&mem[shdr[ehdr->e_shstrndx].sh_offset];

	/*
	 * Grab the data segments offset and base for use in our
	 * elf_address_pointer function
	 */
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset == 0) {
			obj->text_offset = phdr[i].p_offset;
			obj->text_base = phdr[i].p_vaddr;
		}
		if (phdr[i].p_type == PT_DYNAMIC)
			obj->dynamic_linked = true;
	}
	/*
	 * Grab the symbol table and string table .strtab. for future
	 * symbol resolution.
	 */
	for (i = 0; i < ehdr->e_shnum; i++) {
		    if (strcmp(&shstrtab[shdr[i].sh_name], ".symtab") == 0) {
			symtab = (ElfW(Sym) *)&mem[shdr[i].sh_offset];
			obj->symcount = shdr[i].sh_size / shdr[i].sh_entsize;
		} else if (strcmp(&shstrtab[shdr[i].sh_name], ".strtab") == 0) {
			strtab = (char *)&mem[shdr[i].sh_offset];
		}
	}

	obj->symtab = symtab;
	obj->path = path;
	obj->size = st.st_size;
	obj->mem = mem;
	obj->st = st;
	obj->ehdr = ehdr;
	obj->phdr = phdr;
	obj->shdr = shdr;
	obj->fd = fd;
	obj->strtab = strtab;
	obj->shstrtab = shstrtab;
	return true;
}

#define PUSH_LEN 5
#define PUSH_RET_LEN 6 /* push 0x00000000; ret */
/*
 * enable_relro() is injected into the target static executable
 * and is invoked instead of main() by the glibc initialization
 * routine known as generic_start_main(). We do it this way because
 * we must allow all of the initialization routines, including
 * generic_start_main() to issue writes to the areas that we will
 * eventually be mprotecting as read-only. Currently we have some
 * limitations that won't allow multi-threaded applications to work
 * right since we mark .tbss and .tdata as read-only. However .data
 * is not touched, and any single threaded static executable should
 * fair OK (Yah right; prototype, yikes!).
 */
volatile void
unused_delta_begin(void) { volatile int esoteric; return; }
#pragma GCC push_options
#pragma GCC optimize ("-O0")
volatile uint64_t
enable_relro(void)
{
	int i;
	uint8_t *mem =
	    (uint8_t *)(sizeof(uintptr_t) == 4 ? 0x8048000 : 0x400000);
	ElfW(Ehdr) *ehdr = (ElfW(Ehdr) *)mem;
	ElfW(Phdr) *phdr = (ElfW(Phdr) *)&mem[ehdr->e_phoff];
	uint32_t *ptr;
	uint64_t main_addr = 0, stub_vaddr = 0;
	uint64_t data_vaddr, rsi, rdi, rdx, rcx;
	uint64_t relro_vaddr = 0, relro_size;
	uint64_t retaddr;

	bool found_data = false;
	/*
	 * Save register state for main(argc, argv, envp)
	 */
	asm volatile("mov %%rsi, %0" : "=r"(rsi));
	asm volatile("mov %%rdi, %0" : "=r"(rdi));
	asm volatile("mov %%rdx, %0" : "=r"(rdx));
	asm volatile("mov %%rcx, %0" : "=r"(rcx));

	/*
	 * If the PT_GNU_RELRO segment exists, which is ironically useless
	 * until now for statically linked executables, we will make use
	 * of it to determine where we want to apply the mitigation.
	 * We also want to find the 3rd loadable segment which is where
	 * our enable_relro() code is stored as a secondary code segment.
	 */
	for (relro_vaddr = 0, i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type == PT_GNU_RELRO) {
			relro_vaddr = phdr[i].p_vaddr;
			relro_size = PAGE_ALIGN_UP(relro_size);
		} else if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset != 0) {
			if (found_data == true) {
				stub_vaddr = phdr[i].p_vaddr;
				break;
			}
			found_data = true;
			continue;
		}
	}
	if (relro_vaddr > 0) {
		relro_vaddr = PAGE_ALIGN(relro_vaddr);
		asm volatile(
			"mov %0, %%rdi	\n"
			"mov %1, %%rsi	\n"
			"mov %2, %%rdx	\n"
			"mov $10, %%rax \n"
			"syscall" : : "g"(relro_vaddr),
			"g"(4096), "g"(PROT_READ));
		goto process_main; /* Lets go call main() */
	}
	/*
	 * Why have we arrived at this code?
	 * If for some reason the linker script used to build the static
	 * executable didn't include the RELRO segment, then we will simply
	 * use the data segment, which isn't as reliable since it gives the
	 * p_memsz of the entire datasegment, instead of just the areas that
	 * should be relro'd up until the .data section (Don't confuse .data
	 * section and the data segment). Consequently we assume to mprotect
	 * only 1 PAGE as read-only (Although this is 99% of the time enough).
	 */
	for (data_vaddr = 0, i = 0; i < ehdr->e_phnum; i++) {                      
		if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset != 0) {
			if (data_vaddr == 0) {
				data_vaddr = phdr[i].p_vaddr;
			}
			else {
				/*
				 * Get the address of our stub code. Don't
				 * forget the first 4 bytes are the magic
				 * number of the address to main()
				 */
				stub_vaddr = phdr[i].p_vaddr;
				if (stub_vaddr && data_vaddr)
					break;
			}
		}
	}
	if (data_vaddr == 0) {
		asm("int3");
	}
	/*
	 * Mprotect everything from .tdata to the beginning of .data
	 * this means that only single threaded data sections can be
	 * written to. Eventually we will fix this with a more sophisticated
	 * solution that requires moving more things around.
	 */
	__asm__ volatile(
	"mov %0, %%rdi\n"
	"mov %1, %%rsi\n"
	"mov %2, %%rdx\n"
	"mov $10, %%rax\n"
	"syscall" : : "g"(data_vaddr & ~4095), "g"(4096), "g"(PROT_READ));
process_main:
	ptr = (uint32_t *)stub_vaddr;
	main_addr = *ptr;

	/*
	 * Restore register state for argc, argv, envp
	 * passed to main()
	 */
	asm volatile("mov %0, %%rsi" : : "r"(rsi));
	asm volatile("mov %0, %%rdi" : : "r"(rdi));
	asm volatile("mov %0, %%rcx" : : "r"(rcx));
	asm volatile("mov %0, %%rdx" : : "r"(rdx));
	/*
	 * Finally lets go to main() by using pushes and rets
	 * to pretend we are a call. This allows us to avoid
	 * using a 'call imm' for an absolute address which
	 * works within same segment, but is not technically correct.
	 */
#if 0
	retaddr = get_rip() + PUSH_RET_LEN + PUSH_LEN;
	asm volatile("push %0" : : "r"(retaddr));
	asm volatile("push %0" : : "r"(main_addr));
	asm volatile("ret");
#endif

	asm volatile("call %0" : : "r"(main_addr));
	asm volatile("mov $60, %rax\n"
		     "syscall");

	/*
	 * The return value doesn't matter until we move to our
	 * most advanced approach. This is for future use. We
	 * exit before we hit this return.
	 */
	return main_addr;
}
#pragma GCC pop_options

/*
 * NOTE: We assign esoteric to environ here, which is random.
 * but we had to assign it to something global to force the
 * linker to put delta_end() after enable_relro() so that we
 * can calculate enable_relro() delta (its size)
 */
volatile void
delta_end(void) { volatile uintptr_t esoteric = (uintptr_t)&__environ; return; }

#define TMP_FILE "/tmp/.xyz.static.fucker"
#define GENERIC_START_MAIN_PATCH_OFFSET 580 /* glibc 2.23 - 2.25? */
#define TRAMPOLINE_OFFSET GENERIC_START_MAIN_PATCH_OFFSET

bool
inject_relro_code(elfobj2_t *obj)
{
	int i, fd;
	size_t injection_size, old_size = obj->size;
	uint64_t relro_stub_vaddr;
	uint64_t base = sizeof(uintptr_t) == 8 ? 0x400000 : 0x8048000;
	const size_t relro_stub_size = (const size_t)((char *)&delta_end -
	    (char *)&unused_delta_begin);
	size_t new_map_size;
	ElfW(Sym) generic_start_main, main_sym;
	uint64_t generic_start_main_off, patch_vaddr, main_offset;
	uint8_t *ptr;
	const int magic_mark = 0xdeadbeef;

	if (_elf_symbol_by_name(obj, "generic_start_main",
	    &generic_start_main) == false) {
		fprintf(stderr, "elf_symbol_by_name failed\n");
		return false;
	}
	if (_elf_symbol_by_name(obj, "main",
	    &main_sym) == false) {
		fprintf(stderr, "elf_symbol_by_name failed\n");
		return false;
	}

	ptr = _elf_text_pointer(obj, generic_start_main.st_value);
	if (ptr == NULL) {
		fprintf(stderr, "%#lx could not be found in address range\n",
		    generic_start_main.st_value);
		return false;
	}
	/*
	 * Instead of calling main, lets have generic_start_main
	 * call our enable_relro stub instead, 4 bytes into it
	 * though since we have auxiliary info (main symbol value)
	 * stored in the first 4 bytes hence sizeof(uint32_t);
	 */
	uint32_t enable_relro_vaddr = 0xc000000 + old_size + sizeof(uint32_t);

	ptr += TRAMPOLINE_OFFSET;
	ptr[0] = 0x68; /* push */
	*(uint32_t *)&ptr[1] = 0xc000000 + old_size + sizeof(uint32_t);
	ptr[5] = 0xc3; /* ret */

#if 0
	/*
	 * We cannot use this method because we would need a far call
	 * aka an lcall *addr since the enable_relro() code exists in
	 * another text segment all together created by us. Ideally we
	 * would use a reverse text extension, and just keep the enable_relro
	 * code within the main text segment, and use this instruction patching
	 * instead since it is 5 bytes and not 6. Our current 6 byte technique
	 * clobbers the next instructions and forces us to call exit() after
	 * main() but before the .dtors/.fini_array pointers are called.
	 */
	patch_vaddr = generic_start_main.st_value + TRAMPOLINE_OFFSET;
	ptr += TRAMPOLINE_OFFSET;
	ptr[0] = 0xe8; /* call imm */
	main_offset = vaddr  - patch_vaddr - 5;
	*(int32_t *)&ptr[1] = main_offset;
	printf("Patching with value %#lx\n", main_offset);
#endif
	/*
	 * Locate NOTE segment and change its characteristics
	 * to that of a loadable segment with an offset that
	 * point to our enable_relro code. We must modify
	 * these values directly since libelfmaster doesn't
	 * support modification yet.
	 */
	for (i = 0; i < obj->ehdr->e_phnum; i++) {
		if (obj->phdr[i].p_type != PT_NOTE)
			continue;
		obj->phdr[i].p_type = PT_LOAD;
		relro_stub_vaddr = obj->phdr[i].p_vaddr =
		    0xc000000 + old_size;
		injection_size = relro_stub_size;
		obj->phdr[i].p_filesz = relro_stub_size + PADDING_SIZE;
		obj->phdr[i].p_memsz = obj->phdr[i].p_filesz;
		obj->phdr[i].p_flags = PF_R | PF_X;
		obj->phdr[i].p_align = 0x200000;
		obj->phdr[i].p_paddr = obj->phdr[i].p_vaddr;
		obj->phdr[i].p_offset = old_size;

	}
	fd = open(TMP_FILE, O_RDWR|O_CREAT|O_TRUNC, S_IRWXU);
	if (fd < 0) {
		perror("open");
		return false;
	}
#if DEBUG
	/*
	 * This extends section 1 so that we can
	 * view our enable_relro() code with objdump
	 * during debugging phases.
	 */
	obj->shdr[1].sh_offset = old_size;
	obj->shdr[1].sh_addr = 0xc000000 + old_size;
	obj->shdr[1].sh_size = relro_stub_size + 16;
	obj->shdr[1].sh_type = SHT_PROGBITS;
#endif
	if (write(fd, obj->mem, old_size) < 0) {
		perror("write1");
		return false;
	}
	printf("injection size: %lu\n", injection_size);

	(void) write(fd, &main_sym.st_value, 4);

	if (write(fd, (char *)&enable_relro, injection_size) < 0) {
		perror("write2");
		return false;
	}
	printf("main(): %#lx\n", main_sym.st_value);
	close(fd);
	if (rename(TMP_FILE, obj->path) < 0) {
		perror("rename");
		return false;
	}
	return true;
}	
int main(int argc, char **argv)
{

	elfobj2_t obj;

	if (argc < 2) {
		printf("Usage: %s <static_executable>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if (_elf_open_object(argv[1], &obj) == false) {
		fprintf(stderr, "_elf_open_object failed\n");
		exit(EXIT_FAILURE);
	}

	if (obj.dynamic_linked == true) {
		/*
		 * If there is a PT_DYNAMIC segment then we know
		 * this isn't a static executable.
		 */
		fprintf(stderr, "%s is dynamically linked\n", argv[1]);
		exit(EXIT_FAILURE);
	}

	if (inject_relro_code(&obj) == false) {
		fprintf(stderr, "instrumentation failed\n");
		exit(EXIT_FAILURE);
	}

	_elf_close_object(&obj);
}

