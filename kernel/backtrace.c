#include "backtrace.h"

#include "process.h"
#include "spike_interface/spike_utils.h"
#include "util/types.h"
#include "util/string.h"
#include "elf.h"
#include "string.h"
#include "riscv.h"

typedef union {
  uint64 buf[MAX_CMDLINE_ARGS];
  char *argv[MAX_CMDLINE_ARGS];
} arg_buf;

typedef struct elf_info_t {
  spike_file_t *f;
  process *p;
} elf_info;

typedef struct {
  uint32   sh_name;
  uint32   sh_type;
  uint64   sh_flags;
  uint64   sh_addr;
  uint64   sh_offset;
  uint64   sh_size;
  uint32   sh_link;
  uint32   sh_info;
  uint64   sh_addralign;
  uint64   sh_entsize;
} shdr;

typedef struct {
  uint32      st_name;
  unsigned char st_info;
  unsigned char st_other;
  uint16      st_shndx;
  uint64 st_value;
  uint64      st_size;
} symbol;

static size_t parse_args(arg_buf *arg_bug_msg) {
  // HTIFSYS_getmainvars frontend call reads command arguments to (input) *arg_bug_msg
  long r = frontend_syscall(HTIFSYS_getmainvars, (uint64)arg_bug_msg,
      sizeof(*arg_bug_msg), 0, 0, 0, 0, 0);
  kassert(r == 0);

  size_t pk_argc = arg_bug_msg->buf[0];
  uint64 *pk_argv = &arg_bug_msg->buf[1];

  int arg = 1;  // skip the PKE OS kernel string, leave behind only the application name
  for (size_t i = 0; arg + i < pk_argc; i++)
    arg_bug_msg->argv[i] = (char *)(uintptr_t)pk_argv[arg + i];

  //returns the number of strings after PKE kernel in command line
  return pk_argc - arg;
}

static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset) {
  elf_info *msg = (elf_info *)ctx->info;
  // call spike file utility to load the content of elf file into memory.
  // spike_file_pread will read the elf file (msg->f) from offset to memory (indicated by
  // *dest) for nb bytes.
  return spike_file_pread(msg->f, dest, nb, offset);
}

void init_tables(char *symtab, char *strtab, uint64* symtab_size, char* text, uint64 *text_offset, uint64 *entry) {
  arg_buf arg_bug_msg;

  // retrieve command line arguements
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) panic("You need to specify the application program!\n");

  sprint("Application: %s\n", arg_bug_msg.argv[0]);

  //elf loading. elf_ctx is defined in kernel/elf.h, used to track the loading process.
  elf_ctx elfloader;
  // elf_info is defined above, used to tie the elf file and its corresponding process.
  elf_info info;

  info.f = spike_file_open(arg_bug_msg.argv[0], O_RDONLY, 0);
  info.p = current;
  // IS_ERR_VALUE is a macro defined in spike_interface/spike_htif.h
  if (IS_ERR_VALUE(info.f)) panic("Fail on openning the input application program.\n");

  // init elfloader context. elf_init() is defined above.
  if (elf_init(&elfloader, &info) != EL_OK)
    panic("fail to init elfloader.\n");

  const uint64 shstroff = elfloader.ehdr.shoff + elfloader.ehdr.shentsize * elfloader.ehdr.shstrndx;
  // shdr shstr;
  shdr section_header;
  // elf_fpread(&elfloader, (void *)&shstr, sizeof(shdr), shstroff);
  elf_fpread(&elfloader, (void *)&section_header, sizeof(shdr), shstroff);
  char buf[section_header.sh_size]; // used GCC's extension of arrays of variable length
  elf_fpread(&elfloader, (void *)&buf, section_header.sh_size, section_header.sh_offset);
  shdr symtab_header, strtab_header, text_header;
  for (int i = 1; i < elfloader.ehdr.shnum; ++i) {
    elf_fpread(&elfloader, (void *)&section_header, sizeof(shdr), elfloader.ehdr.shoff + elfloader.ehdr.shentsize * i);
    if (strcmp(".symtab", &buf[section_header.sh_name]) == 0) {
      symtab_header = section_header;
    }
    if (strcmp(".strtab", &buf[section_header.sh_name]) == 0) {
      strtab_header = section_header;
    }
    if (strcmp(".text", &buf[section_header.sh_name]) == 0) {
      text_header = section_header;
    }
  }
  // char symtab[symtab_header.sh_size], strtab[strtab_header.sh_size];
  elf_fpread(&elfloader, symtab, symtab_header.sh_size, symtab_header.sh_offset);
  elf_fpread(&elfloader, strtab, strtab_header.sh_size, strtab_header.sh_offset);
  elf_fpread(&elfloader, text, text_header.sh_size, text_header.sh_offset);
  *text_offset = text_header.sh_addr;
  *entry = elfloader.ehdr.entry;
  
  *symtab_size = symtab_header.sh_size;

  spike_file_close( info.f );
}

static char *find_symbol_name(void *symtab, uint64 symtab_size, char *strtab, uint64 addr) {
  symbol sym;
  for (uint64 i = 0; i < symtab_size; i += sizeof(symbol)) {
    // uint64 addr = (uint64)symtab + i;
    // sprint("%lx\n", addr);
    // sym = *((symbol *)addr);
    sym = *((symbol *)((uint64)(symtab) + i));
    if (sym.st_value == addr && sym.st_info == (unsigned char)18) {
    // if (sym.st_value == addr) {
    // sprint("BUG\n");
      return (char *)((uint64)(strtab) + sym.st_name);
    }
  }
  return NULL;
}

inline uint64 get_callee_from_call_instr(uint64 instr) {
  return ((instr & 0xff000) | \
  ((instr >> 9) & (1ULL << 11)) | \
  ((instr >> 20) & (0x7fe)) | \
  ((instr >> 11) & (1ULL << 20)) | \
  ((instr & (1ULL << 20)) ? 0xfffffffffff00000 : 0));
}

uint64 get_callee_from_call_addr(char *text, uint64 call_addr, uint64 text_offset) {
  uint16 *instr_addr = (uint16 *)(((uint64)text + call_addr - text_offset));
  uint32 call_instr = (uint32)(*instr_addr) | (((uint32)(*(instr_addr + 1))) << 16);
  return (uint64)((int64)call_addr + (int64)get_callee_from_call_instr(call_instr));
}

void backtrace(int level) {
  char symtab[1000], strtab[200], text[1000];
  uint64 symtab_size, text_offset, entry;
  init_tables(symtab, strtab, &symtab_size, text, &text_offset, &entry);
  uint64 fp = current->trapframe->regs.s0; // do_user_call()
  // sprint("fp = %lx\n", fp);
  fp = *(uint64*)(fp - 8); // print_backtrace()
  // sprint("fp = %lx\n", fp);
  for (; level > 0; level--) {
    fp = (*(uint64 *)(fp - 16)); // f8()
    // sprint("fp = %lx\n", fp);
    uint64 ra = *(uint64*)(fp - 8);
    if (ra == 0) break;
    // sprint("ra = %lx\n", ra);
    uint64 call_addr = ra - 4;
    // sprint("addr = %lx\n", call_addr);
    
    uint64 callee = get_callee_from_call_addr(text, call_addr, text_offset);
    // sprint("callee = %lx\n", callee);
    sprint("%s\n", find_symbol_name(symtab, symtab_size, strtab, callee));
  }
}