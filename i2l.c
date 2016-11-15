// I2L interpreter
// Copyright 2016 Eric Smith <spacewar@gmail.com>

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

#include "i2l.h"


char *progname;

bool error_longjmp;
char error_str[81];

uint16_t display[MAX_LEVEL];

uint8_t mem[MAX_MEM];

uint16_t heap_start;
uint16_t heap_limit;


int level;    // current level
uint16_t pc;  // program counter
uint16_t sp;  // stack pointer
uint16_t hp;  // heap pointer

bool run;
bool rerun;

FILE *tracef;

bool trap;
int err;

int16_t div_remainder;

char *disk_in_fn;
FILE *disk_in_f;

char *disk_out_fn;
FILE *disk_out_f;


static jmp_buf fatal_error_jmp_buf;


noreturn void v_fatal_error(int num, char *fmt, va_list ap)
{
  int i;
  
  err = num;
  i = snprintf(error_str, sizeof(error_str), "%s: ", progname);
  if (fmt)
    vsnprintf(& error_str[i], sizeof(error_str) - i, fmt, ap);
  else
    snprintf(& error_str[i], sizeof(error_str) - i, "fatal error %d", err);

  run = false;
  
  if (error_longjmp)
    longjmp(fatal_error_jmp_buf, 1);

  fprintf(stderr, "%s\n", error_str);
  exit(err);
}

noreturn void fatal_error(int num, char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  v_fatal_error(num, fmt, ap);
  va_end(ap);
}


// used for errors for which trapping is optional
void runtime_error(int num, char *fmt, ...)
{
  va_list ap;

  if ((num == ERR_IO_ERROR) && (! trap))
    {
      err = num;
      return;
    }

  va_start(ap, fmt);
  v_fatal_error(num, fmt, ap);
  va_end(ap);
}


static inline uint16_t read16(uint16_t addr)
{
  return mem[addr] | (mem[addr+1] << 8);
}

static inline void write16(uint16_t addr, uint16_t data)
{
  mem[addr] = data & 0xff;
  mem[addr+1] = data >> 8;
}


static inline uint16_t peek_tos16(void)
{
  uint16_t high = mem[sp+1] << 8;
  uint16_t low = mem[sp+2];
  return high | low;
}

static inline uint16_t peek_nos16(void)
{
  uint16_t high = mem[sp+3] << 8;
  uint16_t low = mem[sp+4];
  return high | low;
}

static inline uint8_t pop8(void)
{
  if (sp >= (INITIAL_STACK))
    fatal_error(ERR_STACK_UNDERFLOW, NULL);
  return mem[++sp];
}

static inline uint16_t pop16(void)
{
  if (sp >= (INITIAL_STACK - 1))
    fatal_error(ERR_STACK_UNDERFLOW, NULL);
  uint16_t high = mem[++sp] << 8;
  return high | mem[++sp];
}

static inline void push8(uint8_t value)
{
  if (sp < STACK_MIN + 1)
    fatal_error(ERR_STACK_OVERFLOW, NULL);
  mem[sp--] = value;
}

static inline void push16(uint16_t value)
{
  if (sp < STACK_MIN + 2)
    fatal_error(ERR_STACK_OVERFLOW, NULL);
  mem[sp--] = value & 0xff;
  mem[sp--] = value >> 8;
}


static inline uint8_t heap_pop_8(void)
{
  if (hp < (heap_start + 1))
    fatal_error(ERR_HEAP_UNDERFLOW, NULL);
  uint16_t val = mem[--hp];
  return val;
}

static inline uint16_t heap_pop_16(void)
{
  if (hp < (heap_start + 2))
    fatal_error(ERR_HEAP_UNDERFLOW, NULL);
  hp -= 2;
  uint16_t val = read16(hp);
  return val;
}

static inline void heap_push_8(uint8_t value)
{
  if (sp >= (heap_limit - 1))
    fatal_error(ERR_HEAP_OVERFLOW, NULL);
  mem[hp++] = value;
}

static inline void heap_push_16(uint16_t value)
{
  if (sp >= (heap_limit - 2))
    fatal_error(ERR_HEAP_OVERFLOW, NULL);
  write16(hp, value);
  hp += 2;
}


static inline uint8_t fetch8(void)
{
  return mem[pc++];
}

static inline uint16_t fetch16(void)
{
  uint16_t low = mem[pc++];
  uint16_t high = mem[pc++] << 8;
  return high | low;
}

static inline uint8_t fetch_level(void)
{
  uint8_t level = fetch8();
  if (level & 1)
    fatal_error(ERR_BAD_LEVEL, NULL);
  level >>= 1;
  if (level >= MAX_LEVEL)
    fatal_error(ERR_BAD_LEVEL, NULL);
  return level;
}

const uint8_t class_bytes[256] =
{
  [CLASS_NO_OPERAND]            = 1,
  [CLASS_ONE_BYTE_OPERAND]      = 2,
  [CLASS_TWO_BYTE_OPERAND]      = 3,
  [CLASS_ADDRESS]               = 3,
  [CLASS_LEVEL_OFFSET]          = 3,
  [CLASS_LEVEL_ADDRESS]         = 4,
  [CLASS_REAL_OPERAND]          = 1 + REAL_SIZE,
  [CLASS_ADDRESS_REAL_ARRAY]    = 1 + REAL_SIZE,
  [CLASS_ADDRESS_BASE_RELATIVE] = 3,
};


// opcode 0x00: EXIT exit interpreter
void op_exit(void)
{
  run = false;
}

// opcode 0x01: LOD load a variable
void op_lod(void)
{
  int level = fetch_level();
  int offset = fetch8();
  push16(read16(display[level]+offset));
}

// opcode 0x02: LDX indexed load byte
void op_ldx(void)
{
  int level = fetch_level();
  int offset = fetch8();
  uint16_t index = pop16();
  uint16_t base = read16(display[level]+offset);
  uint8_t value = mem[base+index];
  push16(value);
}

// opcode 0x03: STO store into a variable
void op_sto(void)
{
  int level = fetch_level();
  int offset = fetch8();
  uint16_t value = pop16();
  write16(display[level]+offset, value);
}

// opcode 0x04: STX indexed store to a byte
void op_stx(void)
{
  int level = fetch_level();
  int offset = fetch8();
  uint8_t value = pop16();
  uint16_t index = pop16();
  uint16_t base = read16(display[level]+offset);
  mem[base + index] = value;
}


void do_call(int new_level, uint16_t target)
{
  heap_push_8(level<<1);            // caller's level
  level = new_level;
  heap_push_16(display[level]);  // prev value of display of new level
  heap_push_16(pc);              // caller's PC
  heap_push_8(0x00);             // caller's PC offset, not used
  display[level] = hp;
  pc = target;
}

// opcode 0x05: CAL call an I2L procedure
void op_cal(void)
{
  int new_level = fetch_level();
  uint16_t target = fetch16();
  do_call(new_level, target);
}

// opcode 0x06: RET return from I2L procedure
void op_ret(void)
{
  hp = display[level];  // dispose any reserve()'d memory
  (void) heap_pop_8();  // discard caller's PC offset, not used
  pc = heap_pop_16();   // restore caller's PC
  uint16_t old_display = heap_pop_16();
  int old_level = heap_pop_8() >> 1; // restore caller's level
  display[level] = old_display;  // restore
  level = old_level;
}

// opcode 0x07: JMP jump to I2L code
void op_jmp(void)
{
  pc = fetch16();
}

// opcode 0x08: JPC jump if false
void op_jpc(void)
{
  uint16_t target = fetch16();
  uint16_t val = pop16();
  if (! val)
    pc = target;
}

// opcode 0x09: HPI increment HP by operand
void op_hpi(void)
{
  hp += fetch8();
}

// opcode 0x0a: ARG get procedure arguments
void op_arg(void)
{
  uint8_t count = fetch8();
  int i;
  // start at offset 6 into heap to leave room for frame
  for (i = 0; i <= count; i++)
    mem[hp+6+(count-i)] = pop8();
}

// opcode 0x0b: IMM immediate load of arg
void op_imm(void)
{
  push16(fetch16());
}

// opcode 0x0c: CML call a machine lang function (intrinsic)
void op_cml(void)
{
  int inum = fetch8() - INTRINSIC_OFFSET;
  if ((inum < 0) || (inum >= INTRINSIC_MAX))
    fatal_error(ERR_BAD_INTRINSIC, NULL);
  opfn_t *fn = intrinsic[inum].fn;
  if (! fn)
    fatal_error(ERR_BAD_INTRINSIC, NULL);
  fn();
}

// opcode 0x0d: ADD add
void op_add(void)
{
  uint16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 + op2);
}

// opcode 0x0e: SUB subtract
void op_sub(void)
{
  uint16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 - op2);
}

// opcode 0x0f: MUY multiply
void op_muy(void)
{
  int16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 * op2);
}

// opcode 0x10: DIV divide
void op_div(void)
{
  int16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  if (op2 == 0)
    fatal_error(ERR_DIVISION_BY_ZERO, NULL);
  push16(op1 / op2);
  div_remainder = op1 % op2;
}

// opcode 0x11: NEG monadic minus
void op_neg(void)
{
  int16_t op1;
  op1 = pop16();
  push16(-op1);
}

// opcode 0x12: EQ test for equal
void op_eq(void)
{
  uint16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 == op2 ? -1 : 0);
}

// opcode 0x13: NE test for not equal
void op_ne(void)
{
  uint16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 != op2 ? -1 : 0);
}

// opcode 0x14: GE test for >=
void op_ge(void)
{
  int16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 >= op2 ? -1 : 0);
}

// opcode 0x15: GT test for >
void op_gt(void)
{
  int16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 > op2 ? -1 : 0);
}

// opcode 0x16: LE test for <=
void op_le(void)
{
  int16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 <= op2 ? -1 : 0);
}

// opcode 0x17: LT test for <
void op_lt(void)
{
  int16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 < op2 ? -1 : 0);
}

// opcode 0x18: FOR for loop control
void op_for(void)
{
  uint16_t target = fetch16();
  int16_t value = pop16();
  int16_t limit = peek_tos16();
  if ((limit - value) <= 0)
    {
      pop16();
      pc = target;
    }
}

// opcode 0x19: INC increment and push
void op_inc(void)
{
  int level = fetch_level();
  int offset = fetch8();
  int value = read16(display[level]+offset) + 1;
  write16(display[level]+offset, value);
  push16(value);
}

// opcode 0x1a: OR boolean "or"
void op_or(void)
{
  uint16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 | op2);
}

// opcode 0x1b: OR boolean "and"
void op_and(void)
{
  uint16_t op1, op2;
  op2 = pop16();
  op1 = pop16();
  push16(op1 & op2);
}

// opcode 0x1c: NOT boolean complement
void op_not(void)
{
  uint16_t op1;
  op1 = pop16();
  push16(~op1);
}

// opcode 0x1d: DUPCAT double TOS
void op_dupcat(void)
{
  push16(peek_tos16());
}

// opcode 0x1e: DOUBL NOS + TOS * 2
void op_dba(void)
{
  uint16_t tos = pop16();
  uint16_t nos = pop16();
  push16(2 * tos + nos);
}

// opcode 0x1f: STD indirect save (aka DEFSAV)
void op_std(void)
{
  uint16_t value = pop16();
  uint16_t addr = pop16();
  write16(addr, value);
}

// opcode 0x20: DBI indirect get (aka DEFER)
void op_dbi(void)
{
  uint16_t tos = pop16();
  uint16_t nos = pop16();
  push16(read16(2 * tos + nos));
}

// opcode 0x21: ADR address of variable
void op_adr(void)
{
  int level = fetch_level();
  int offset = fetch8();
  push16(display[level]+offset);
}

// opcode 0x22: LDI indirect get
void op_ldi(void)
{
  push16(read16(pop16()));
}

// opcode 0x23: LDA absolute get
void op_lda(void)
{
  push16(read16(fetch16()));
}

// opcode 0x24: IMS short immediate
void op_ims(void)
{
  uint16_t val = fetch8();
  if (val & 0x80)
    val |= 0xff00;
  push16(val);
}

// opcode 0x25: CJP case jump
void op_cjp(void)
{
  uint16_t tos = pop16();
  uint16_t nos = peek_tos16();
  uint16_t target = fetch16();
  if (tos == nos)
    pc = target;
}

// opcode 0x26: JSR short call
void op_jsr(void)
{
  uint16_t target = fetch16();
  if (tracef)
    {
      fprintf(tracef, "jsr target %04" PRIx16 "\n", target);
      fflush(tracef);
    }
  push16(pc);
  if (tracef)
    {
      fprintf(tracef, "jsr pushed\n");
      fflush(tracef);
    }
  pc = target;
  if (tracef)
    {
      fprintf(tracef, "pc is %" PRIx16 "\n", pc);
      fflush(tracef);
    }
}

// opcode 0x27: RTS short return
void op_rts(void)
{
  pc = pop16();
}

// opcode 0x28: DRP discard TOS
void op_drp(void)
{
  (void) pop16();
}

// opcode 0x29: ECL call external
void op_ecl(void)
{
  fatal_error(ERR_UNIMPLEMENTED_OPCODE, NULL);
}


// intrinsic 0x00: ABS absolute value
void intrinsic_abs(void)
{
  int16_t op1 = pop16();
  if (op1 < 0)
    op1 = -op1;
  push16(op1);
}

// intrinsic 0x01: RAN random number
void intrinsic_ran(void)
{
  int16_t range = pop16();
  int r = rand();
  push16(r % range);
}

// intrinsic 0x02: REM remainder
void intrinsic_rem(void)
{
  (void) pop16();
  push16(div_remainder);
}

// intrinsic 0x03: RESERVE
void intrinsic_reserve(void)
{
  uint16_t base = hp;
  uint16_t size = pop16();
  if ((hp + size) > heap_limit)
    fatal_error(ERR_HEAP_OVERFLOW, NULL);
  hp += size;
  push16(base);
}

// intrinsic 0x04: SWAP
void intrinsic_swap(void)
{
  uint16_t val = pop16();
  push16(((val >> 8) & 0xff) | ((val & 0xff) << 8));
}

// intrinsic 0x05: EXTEND
void intrinsic_extend(void)
{
  uint16_t val = pop16() & 0xff;
  if (val & 0x80)
    val |= 0xff00;
  push16(val);
}

// intrinsic 0x06: RESTART
void intrinsic_restart(void)
{
  uint16_t val = pop16();
  run = false;
  rerun = true;
}

// intrinsic 0x07: CHIN
void intrinsic_chin(void)
{
  uint16_t dev = pop16();
  int c;
  
  switch (dev)
    {
    case 0:  // console, cooked (line-oriented)
      c = fgetc(stdin);
      if (c == EOF)
	runtime_error(ERR_IO_ERROR, "end of file");
      if (c == '\n')
	c = '\r';
      push16(c);
      return;  // always available
    case 1:  // console, unbuffered, raw (no echo)
      break;
    case 2:  // printer
      break;
    case 3:  // disk input file
      if (! disk_in_f)
	break;
      c = fgetc(disk_in_f);
      if (c == EOF)
	runtime_error(ERR_IO_ERROR, "end of file");
      if (c == '\n')
	c = '\r';
      push16(c);
      return;
    case 4:  // serial
      break;
    case 7:  // null device
      push16(XPL0_EOF);  // end of file
      return;  // always available
    }
  runtime_error(ERR_IO_ERROR, "can't read from device %d", dev);
}

// intrinsic 0x08: CHOUT
void intrinsic_chout(void)
{
  uint16_t c = pop16();
  uint16_t dev = pop16();
  int r;
  
  switch (dev)
    {
    case 0:  // console, cooked (line-oriented)
      r = fputc(c, stdout);
      if (r == EOF)
	runtime_error(ERR_IO_ERROR, "end of file");
      return;  // always available
    case 1:  // console, unbuffered, raw (no echo)
      break;
    case 2:  // printer
      break;
    case 3:  // disk input file
      if (! disk_out_f)
	break;
      r = fputc(c, disk_out_f);
      if (r == EOF)
	runtime_error(ERR_IO_ERROR, "end of file");
      return;
    case 4:  // serial
      break;
    case 7:  // null device
      return;  // always available
    }
  if (dev != 0)
    runtime_error(ERR_IO_ERROR, "unimplemented device %d", dev);
}

// intrinsic 0x09: CRLF
void intrinsic_crlf(void)
{
  uint16_t dev = pop16();
  if (dev != 0)
    runtime_error(ERR_IO_ERROR, "unimplemented device %d", dev);
  fprintf(stdout, "\n");
}

// intrinsic 0x0a: NUMIN
void intrinsic_numin(void)
{
  int16_t num;
  uint16_t dev = pop16();
  if (dev != 0)
    runtime_error(ERR_IO_ERROR, "unimplemented device %d", dev);
  fscanf(stdin, "%" SCNd16, & num);
  // XXX should check for I/O error
  push16(num);
}

// intrinsic 0x0b: NUMOUT
void intrinsic_numout(void)
{
  int16_t num = pop16();
  uint16_t dev = pop16();
  if (dev != 0)
    runtime_error(ERR_IO_ERROR, "unimplemented device %d", dev);
  fprintf(stdout, "%d", num);
  // XXX should check for I/O error
}

// intrinsic 0x0c: TEXT
void intrinsic_text(void)
{
  int r;
  uint16_t si = pop16();
  uint16_t dev = pop16();
  if (dev != 0)
    runtime_error(ERR_IO_ERROR, "unimplemented device %d", dev);
  while (1)
    {
      uint8_t c = mem[si++];
      r = fputc(c & 0x7f, stdout);
      if (r == EOF)
	runtime_error(ERR_IO_ERROR, "end of file");
      if (c & 0x80)
	break;
    }
}

// intrinsic 0x0d: OPENI
void intrinsic_openi(void)
{
  uint16_t dev = pop16();
  switch (dev)
    {
    case 0:  // console, cooked (line-oriented)
      return;  // always available
    case 1:  // console, unbuffered, raw (no echo)
      break;
    case 2:  // printer
      break;
    case 3:  // disk input file
      if (disk_in_f)
	{
	  fclose(disk_in_f);
	  disk_in_f = NULL;
	}
      if (! disk_in_fn)
	break;
      disk_in_f = fopen(disk_in_fn, "r");
      if (! disk_in_f)
	break;
      return;
    case 4:  // serial
      break;
    case 7:  // null device
      return;  // always available
    }
  runtime_error(ERR_IO_ERROR, "can't open input device %d", dev);
}

// intrinsic 0x0e: OPENO
void intrinsic_openo(void)
{
  uint16_t dev = pop16();
  switch(dev)
    {
    case 0:  // console, cooked (line-oriented)
      return;  // always available
    case 1:  // console, unbuffered, raw (no echo)
      break;
    case 2:  // printer
      break;
    case 3:  // disk input file
      if (disk_out_f)
	{
	  fclose(disk_out_f);
	  disk_out_f = NULL;
	}
      if (! disk_out_fn)
	break;
      disk_out_f = fopen(disk_out_fn, "w");
      if (! disk_out_f)
	break;
      return;
    case 4:  // serial
      break;
    case 7:  // null device
      return;  // always available
    }
  runtime_error(ERR_IO_ERROR, "can't open output device %d", dev);
}

// intrinsic 0x0f: CLOSE
void intrinsic_close(void)
{
  uint16_t dev = pop16();
  switch (dev)
    {
    case 0:  // console, cooked (line-oriented)
      return;  // always available
    case 1:  // console, unbuffered, raw (no echo)
      break;
    case 2:  // printer
      break;
    case 3:  // disk in and out files
      if (disk_in_f)
	{
	  fclose(disk_in_f);
	  disk_in_f = NULL;
	}
      if (disk_out_f)
	{
	  fclose(disk_out_f);
	  disk_out_f = NULL;
	}
      return;
    case 4:  // serial
      break;
    case 7:  // null device
      return;  // always available
    }
  runtime_error(ERR_IO_ERROR, "can't close device %d", dev);
}

// intrinsic 0x10: ABORT
void intrinsic_abort(void)
{
  fatal_error(ERR_ABORT, NULL);
}

// intrinsic 0x11: TRAP
void intrinsic_trap(void)
{
  uint16_t val = pop16();
  trap = val != 0;
}

// intrinsic 0x12: SPACE
void intrinsic_space(void)
{
  push16(heap_limit - hp);
}

// intrinsic 0x13: RERUN
void intrinsic_rerun(void)
{
  push16(rerun ? 0xffff : 0x0000);
}

// intrinsic 0x14: GETHP
void intrinsic_gethp(void)
{
  push16(hp);
}

// intrinsic 0x15: SETHP  // dangerous!
void intrinsic_sethp(void)
{
  hp = pop16();
}

// intrinsic 0x16: ERRFLG
void intrinsic_errflg(void)
{
  push16(err ? 0xffff : 0x0000);
  err = 0;
}

// intrinsic 0x17: CURSOR
void intrinsic_cursor(void)
{
  uint16_t y = pop16();
  uint16_t x = pop16();
  fatal_error(ERR_UNIMPLEMENTED_INTRINSIC, "unimplemented intrinsic CURSOR");
}

// intrinsic 0x19: SETRUN
void intrinsic_setrun(void)
{
  rerun = pop16();
}

// intrinsic 0x1a: HEXIN
void intrinsic_hexin(void)
{
  uint16_t num;
  uint16_t dev = pop16();
  if (dev != 0)
    runtime_error(ERR_IO_ERROR, "unimplemented device %d", dev);
  fscanf(stdin, "%" SCNx16, & num);
  // XXX should check for I/O error
  push16(num);
}

// intrinsic 0x1b: HEXOUT
void intrinsic_hexout(void)
{
  uint16_t num = pop16();
  uint16_t dev = pop16();
  if (dev != 0)
    runtime_error(ERR_IO_ERROR, "unimplemented device %d", dev);
  fprintf(stdout, "%x", num);
  // XXX should check for I/O error
}


const opinfo_t op[128] =
  {
    [0x00] = { op_exit,   "exi",    CLASS_NO_OPERAND },
    [0x01] = { op_lod,    "lod",    CLASS_LEVEL_OFFSET },
    [0x02] = { op_ldx,    "ldx",    CLASS_LEVEL_OFFSET },
    [0x03] = { op_sto,    "sto",    CLASS_LEVEL_OFFSET },
    [0x04] = { op_stx,    "stx",    CLASS_LEVEL_OFFSET },
    [0x05] = { op_cal,    "cal",    CLASS_LEVEL_ADDRESS },
    [0x06] = { op_ret,    "ret",    CLASS_NO_OPERAND },
    [0x07] = { op_jmp,    "jmp",    CLASS_ADDRESS },
    [0x08] = { op_jpc,    "jpc",    CLASS_ADDRESS },
    [0x09] = { op_hpi,    "hpi",    CLASS_ONE_BYTE_OPERAND },       // aka spi
    [0x0a] = { op_arg,    "arg",    CLASS_ONE_BYTE_OPERAND },
    [0x0b] = { op_imm,    "imm",    CLASS_ADDRESS },
    [0x0c] = { op_cml,    "cml",    CLASS_ONE_BYTE_OPERAND },
    [0x0d] = { op_add,    "add",    CLASS_NO_OPERAND },
    [0x0e] = { op_sub,    "sub",    CLASS_NO_OPERAND },
    [0x0f] = { op_muy,    "muy",    CLASS_NO_OPERAND },
    [0x10] = { op_div,    "div",    CLASS_NO_OPERAND },
    [0x11] = { op_neg,    "neg",    CLASS_NO_OPERAND },
    [0x12] = { op_eq,     "eq",     CLASS_NO_OPERAND },
    [0x13] = { op_ne,     "ne",     CLASS_NO_OPERAND },
    [0x14] = { op_ge,     "ge",     CLASS_NO_OPERAND },
    [0x15] = { op_gt,     "gt",     CLASS_NO_OPERAND },
    [0x16] = { op_le,     "le",     CLASS_NO_OPERAND },
    [0x17] = { op_lt,     "lt",     CLASS_NO_OPERAND },
    [0x18] = { op_for,    "for",    CLASS_ADDRESS },
    [0x19] = { op_inc,    "inc",    CLASS_LEVEL_OFFSET },
    [0x1a] = { op_or,     "or",     CLASS_NO_OPERAND },
    [0x1b] = { op_and,    "and",    CLASS_NO_OPERAND },
    [0x1c] = { op_not,    "not",    CLASS_NO_OPERAND },
    [0x1d] = { op_dupcat, "dupcat", CLASS_NO_OPERAND },
    [0x1e] = { op_dba,    "dba",    CLASS_NO_OPERAND },       // aka doubl
    [0x1f] = { op_std,    "std",    CLASS_NO_OPERAND },       // aka defsav
    [0x20] = { op_dbi,    "dbi",    CLASS_NO_OPERAND },       // aka defer
    [0x21] = { op_adr,    "adr",    CLASS_LEVEL_OFFSET },     // aka addr
    [0x22] = { op_ldi,    "ldi",    CLASS_NO_OPERAND },
    [0x23] = { op_lda,    "lda",    CLASS_ADDRESS },
    [0x24] = { op_ims,    "ims",    CLASS_ONE_BYTE_OPERAND }, // aka SIMM
    [0x25] = { op_cjp,    "cjp",    CLASS_ADDRESS },          // aka CAJMP
    [0x26] = { op_jsr,    "jsr",    CLASS_ADDRESS },
    [0x27] = { op_rts,    "rts",    CLASS_NO_OPERAND },
    [0x28] = { op_drp,    "drp",    CLASS_NO_OPERAND },
    [0x29] = { op_ecl,    "ecl",    CLASS_TWO_BYTE_OPERAND }, // aka EXT
#if FLOATING_POINT
    [0x2a] = { op_ldf,    "lodf",   CLASS_LEVEL_OFFSET },
    [0x2b] = { op_stof,   "stof",   CLASS_LEVEL_OFFSET },
    [0x2c] = { op_immf,   "immf",   CLASS_REAL_OPERAND }, // or CLASS_ADDRESS_REAL_ARRAY
    [0x2d] = { op_addf,   "addf",   CLASS_NO_OPERAND },
    [0x2e] = { op_subf,   "subf",   CLASS_NO_OPERAND },
    [0x2f] = { op_mulf,   "mulf",   CLASS_NO_OPERAND },
    [0x30] = { op_divf,   "divf",   CLASS_NO_OPERAND },
    [0x31] = { op_negf,   "negf",   CLASS_NO_OPERAND },
    [0x32] = { op_eqf,    "eqf",    CLASS_NO_OPERAND },
    [0x33] = { op_nef,    "nef",    CLASS_NO_OPERAND },
    [0x34] = { op_gef,    "gef",    CLASS_NO_OPERAND },
    [0x35] = { op_gtf,    "gtf",    CLASS_NO_OPERAND },
    [0x36] = { op_lef,    "lef",    CLASS_NO_OPERAND },
    [0x37] = { op_ltf,    "ltf",    CLASS_NO_OPERAND },
    [0x38] = { op_tra,    "tra",    CLASS_NO_OPERAND },  // FP equiv of DBA
    [0x39] = { op_trx,    "trx",    CLASS_NO_OPERAND },  // FP equiv of DBI?
    [0x3a] = { op_tri,    "tri",    CLASS_NO_OPERAND },  // FP equiv of DBI?
    [0x3b] = { op_stt,    "stt",    CLASS_NO_OPERAND },  // FP equiv of STD
#endif
  };

const intrinsic_info_t intrinsic[INTRINSIC_MAX] =
  {
    [0x00] = { "abs",     intrinsic_abs },
    [0x01] = { "ran",     intrinsic_ran },
    [0x02] = { "rem",     intrinsic_rem },
    [0x03] = { "reserve", intrinsic_reserve },
    [0x04] = { "swap",    intrinsic_swap },
    [0x05] = { "extend",  intrinsic_extend },
    [0x06] = { "restart", intrinsic_restart },
    [0x07] = { "chin",    intrinsic_chin },
    [0x08] = { "chout",   intrinsic_chout },
    [0x09] = { "crlf",    intrinsic_crlf },   // aka SKIP
    [0x0a] = { "numin",   intrinsic_numin },  // aka INTIN
    [0x0b] = { "numout",  intrinsic_numout }, // aka INTOUT
    [0x0c] = { "text",    intrinsic_text },
    [0x0d] = { "openi",   intrinsic_openi },
    [0x0e] = { "openo",   intrinsic_openo },
    [0x0f] = { "close",   intrinsic_close },
    [0x10] = { "abort",   intrinsic_abort },
    [0x11] = { "trap",    intrinsic_trap },   // note argument differs between Apple, PC
    [0x12] = { "space",   intrinsic_space },  // aka FREE
    [0x13] = { "rerun",   intrinsic_rerun },
    [0x14] = { "gethp",   intrinsic_gethp },  // aka GETSP, equivalent to Reserve(0)
    [0x15] = { "sethp",   intrinsic_sethp },  // aka SETSP
    [0x16] = { "errflg",  intrinsic_errflg }, // aka GETERR
    [0x17] = { "cursor",  intrinsic_cursor },
    [0x19] = { "setrun",  intrinsic_setrun },

    [0x1a] = { "hexin",   intrinsic_hexin },
    [0x1b] = { "hexout",  intrinsic_hexout },

#ifdef APEX_INTRINSICS
    [0x18] = { "scan",    intrinsic_scan },

    [0x1c] = { "chain",   intrinsic_chain },
    [0x1d] = { "openf",   intrinsic_openf },
    [0x1e] = { "write",   intrinsic_write },
    [0x1f] = { "read",    intrinsic_read },    // write 256-byte blocks to disk
    [0x20] = { "restore", intrinsic_restore }, // read 256-byte blocks from disk
#endif

#if APPLE_II_INTRINSICS
    [0x21] = { "settxt",  intrinsic_settxt },
    [0x22] = { "sethi",   intrinsic_sethi },
    [0x23] = { "setmix",  intrinsic_setmix },
    [0x24] = { "setlo",   intrinsic_setlo },
    [0x25] = { "switch",  intrinsic_switch },
    [0x26] = { "paddle",  intrinsic_paddle },
    [0x27] = { "noise",   intrinsic_noise },
    [0x28] = { "clear",   intrinsic_clear },
    [0x29] = { "dot",     intrinsic_dot },
    [0x2a] = { "line",    intrinsic_line },
    [0x2b] = { "move",    intrinsic_move },
    [0x2c] = { "screen",  intrinsic_screen },
    [0x2d] = { "block",   intrinsic_block },
#endif

#if MSDOS_INTRINSICS
    [0x18] = { "fset",    intrinsic_fset },

    [0x1c] = { "chain",   intrinsic_chain },
    [0x1d] = { "fopen",   intrinsic_fopen },
    [0x1e] = { "write",   intrinsic_write },  // write 512-byte blocks to disk
    [0x1f] = { "read",    intrinsic_read },   // read 512-byte blocks from disk
    [0x20] = { "fclose",  intrinsic_fclose },
    [0x21] = { "chkkey",  intrinsic_chkkey },
    [0x22] = { "softint", intrinsic_softint },
    [0x23] = { "getreg",  intrinsic_getreg },
    [0x24] = { "blit",    intrinsic_blit },
    [0x25] = { "peek",    intrinsic_peek },
    [0x26] = { "poke",    intrinsic_poke },
    [0x27] = { "sound",   intrinsic_sound },
    [0x28] = { "clear",   intrinsic_clear },
    [0x29] = { "point",   intrinsic_point },
    [0x2a] = { "line",    intrinsic_line },
    [0x2b] = { "move",    intrinsic_move },
    [0x2c] = { "readpix", intrinsic_readpix },
    [0x2d] = { "setvid",  intrinsic_setvid },

    [0x40] = { "pout",    intrinsic_pout },
    [0x41] = { "pin",     intrinsic_pin },
    [0x42] = { "intret",  intrinsic_intret },
    [0x43] = { "extjmp",  intrinsic_extjmp },
    [0x44] = { "extcal",  intrinsic_extcal },
    [0x45] = { "attrib",  intrinsic_attrib },
    [0x46] = { "setwind", intrinsic_setwind },
    [0x47] = { "rawtext", intrinsic_rawtext },
    [0x48] = { "hilight", intrinsic_hilight },
    [0x49] = { "malloc",  intrinsic_malloc },
    [0x4a] = { "release", intrinsic_release },
    [0x4b] = { "trapc",   intrinsic_trapc },
    [0x4c] = { "testc",   intrinsic_testc },
    [0x4d] = { "equip",   intrinsic_equip },
    [0x4e] = { "shrink",  intrinsic_shrink },
    [0x4f] = { "ranseed", intrinsic_ranseed },
    [0x50] = { "irq",     intrinsic_irq },
#endif

#if FLOATING_POINT
    [0x2e] = { "rlres",   intrinsic_rlres },
    [0x2f] = { "rlin",    intrinsic_rlin },
    [0x30] = { "rlout",   intrinsic_rlout },
    [0x31] = { "float",   intrinsic_float },
    [0x32] = { "fix",     intrinsic_fix },
    [0x33] = { "rlabs",   intrinsic_rlabs },
    [0x34] = { "format",  intrinsic_format },
    [0x35] = { "sqrt",    intrinsic_sqrt },
    [0x36] = { "ln",      intrinsic_ln },
    [0x37] = { "exp",     intrinsic_exp },
    [0x38] = { "sin",     intrinsic_sin },
    [0x39] = { "atan2",   intrinsic_atan2 },
    [0x3a] = { "mod",     intrinsic_mod },
    [0x3b] = { "log",     intrinsic_log },
    [0x3c] = { "cos",     intrinsic_cos },
    [0x3d] = { "tan",     intrinsic_tan },
    [0x3e] = { "asin",    intrinsic_asin },
    [0x3f] = { "acos",    intrinsic_acos },
#endif
  };



#define MAX_HEX_DIGITS 4
bool read_hex(FILE *f, int digits, bool non_hex_ok, uint16_t *value)
{
  int i;
  char s[MAX_HEX_DIGITS+1];

  assert(digits <= MAX_HEX_DIGITS);
  
  for (i = 0; i < digits; i++)
    {
      char c = fgetc(f);
      if (c == EOF)
	fatal_error(ERR_I2L_UNEXPECTED_EOF, NULL);
      if (! isxdigit(c))
	{
	  ungetc(c, f);
	  if (non_hex_ok && (i == 0))
	    return false;
	  fatal_error(ERR_I2L_UNEXPECTED_CHAR, NULL);
	}
      s[i] = c;
    }

  s[i] = '\0';
  *value = strtoul(s, NULL, 16);
  return true;
}


int loader_debug = 0;

void loader(FILE *f)
{
  uint16_t base = CODE_START;
  uint16_t offset = 0;

  uint16_t value;

  while (true)
    {
      if (read_hex(f, 2, true, & value))
	{
	  if (loader_debug >= 2)
	    printf("loading addr %04x data %02x\n", base + offset, value); 
	  mem[base+(offset++)] = value;
	  if ((base+offset) > heap_start)
	    heap_start = base+offset;
	}
      else
	{
	  int c = fgetc(f);
	  switch (c)
	    {
	    case '\r':
	    case '\n': // don't need to do anything
	      break;
	    case ';':  // new load address
	      (void) read_hex(f, 4, false, & value);
              offset = value;
	      break;
	    case '^':  // fixup
	      (void) read_hex(f, 4, false, & value);
	      if (loader_debug >= 2)
		printf("fixup addr %04x value %04x\n", base + value, base + offset); 
	      write16(base+value, base+offset);
	      break;
	    case '*':  // relative address
	      (void) read_hex(f, 4, false, & value);
	      if (loader_debug >= 2)
		printf("loading addr %04x value %04x\n", base + offset, base + value); 
	      write16(base+offset, base + value);
	      offset += 2;
	      if ((base+offset) > heap_start)
		heap_start = base+offset;
	      break;
	    case '$':  // end of file marker
	      return;
	    default:
	      fatal_error(ERR_I2L_UNEXPECTED_CHAR, NULL);
	    }
	}
    }
}


void interp_run(void)
{
  while (run)
    {
      uint16_t old_pc = pc;
      uint8_t class;
      uint8_t bytes;
      uint8_t opcode = fetch8();
      if (opcode >= 0x80)
	class = CLASS_NO_OPERAND; // short global load
      else
	class = op[opcode].class;
      bytes = class_bytes[class];
      if (bytes == 0)
	fatal_error(ERR_INTERNAL_ERROR, NULL);

      if (tracef)
	{
	  int i;
	  fprintf(tracef, "  sp: %04x  tos: %04x  nos: %04x\n", sp, peek_tos16(), peek_nos16());
	  fprintf(tracef, "  hp: %04x\n", hp);
	  fprintf(tracef, "  level: %d  display: [", level);
	  for (i = 0; i < 8; i++)
	    {
	      if (i == level)
		fprintf(tracef, "*");
	      fprintf(tracef, "%04" PRIx16 " ", display[i]);
	    }
	  fprintf(tracef, "]\n");
	  fprintf(tracef, "  prev_level: %d  prev_display: %04x  prev_pc: %04x\n",
		  mem[display[level]>>1],
		  read16(display[level]+1),
		  read16(display[level]+3));
	  for (i = 0; i < 8; i++)
	    fprintf(tracef, "  var(%02x)=%04x", i*2, read16(display[level]+i*2));
	  fprintf(tracef, "\n");
	  fprintf(tracef, "%04x: ", old_pc);
	  for (i = 0; i < 4; i++)
	    if (i < bytes)
	      fprintf(tracef, "%02x ", mem[old_pc + i]);
	    else
	      fprintf(tracef, "   ");
	  if (opcode >= 0x80)
	    fprintf(tracef, "lod");  // short global load
	  else if (op[opcode].name)
	    fprintf(tracef, "%s", op[opcode].name);
	  else
	    fprintf(tracef, "???");
	  if (opcode == 0x0c)  // CML
	    {
	      int inum = mem[old_pc + 1] - INTRINSIC_OFFSET;
	      if ((inum < 0) || (inum >= INTRINSIC_MAX))
		fprintf(tracef, " unknown");
	      else
		fprintf(tracef, " %s", intrinsic[inum].name);
	    }
	  fprintf(tracef, "\n");
	  fflush(tracef);
	}
      
      if (opcode >= 0x80)
	{
	  // short global load (short form of LOD)
	  uint16_t offset = opcode & 0x7f;
	    offset <<= 1;
	  push16(read16(display[0] + offset));
	}
      else
	{
	  opfn_t *fn = op[opcode].fn;
	  if (! fn)
	    fatal_error(ERR_BAD_OPCODE, "bad opcode %02" PRIx8 " at %04" PRIx16, opcode, old_pc);
	  else
	    fn();
	}
    }
}

void interp(void)
{
  err = 0;
  bool initialized = false;

  do
    {
      // The following setjmp will return non-zero for
      // an untrapped I/O error.
      if (! setjmp(fatal_error_jmp_buf))
	{
	  sp = INITIAL_STACK;
	  hp = heap_start;

	  level = 0;
	  mem[0xffff] = 0;              // set up an exit opcode
	  pc = 0xffff;

	  // set up main program's stack frame
	  do_call(0, CODE_START);

	  run = true;
	  rerun = false;
	  trap = true;

	  initialized = true;
	}

      interp_run();
    }

  while(rerun);
}


void cleanup(void)
{
  if (disk_out_f)
    {
      fclose(disk_out_f);
      disk_out_f = NULL;
      // XXX should discard the output file, according to Apex doc
    }
}


int main(int argc, char **argv)
{
  error_longjmp = false;
  char *i2lfn = NULL;
  FILE *i2lf = NULL;

  heap_start = 0;  // will be set by loader
  heap_limit = 0x5fff;

  tracef = NULL;

  progname = argv[0];

  disk_in_fn = NULL;
  disk_in_f = NULL;

  disk_out_fn = NULL;
  disk_out_f = NULL;

  atexit(cleanup);
  
  while (++argv, --argc)
    {
      if (argv[0][0] == '-')
	{
	  if ((strcmp(argv[0], "--trace") == 0) && (! tracef) && (argc--))
	    {
	      tracef = fopen(*++argv, "wb");
	      if (! tracef)
		fatal_error(ERR_IO_ERROR, "can't open trace file");
	    }
	  else if ((strcmp(argv[0], "-i") == 0) && (! disk_in_fn) && (argc--))
	    disk_in_fn = *++argv;
	  else if ((strcmp(argv[0], "-o") == 0) && (! disk_out_fn) && (argc--))
	    disk_out_fn = *++argv;
	  else
	    fatal_error(ERR_BAD_CMD_LINE, NULL);
	}
      else if (i2lfn == NULL)
	i2lfn = argv[0];
      else
	fatal_error(ERR_BAD_CMD_LINE, NULL);
    }

  if (! i2lfn)
    fatal_error(ERR_NO_I2L_FILE, NULL);
  i2lf = fopen(i2lfn, "rb");
  if (! i2lf)
    fatal_error(ERR_NO_I2L_FILE, NULL);
  loader(i2lf);
  fclose(i2lf);

  interp();

  exit(err);
}


// I2L format:
// <byte>   store byte at current address
// ;<addr>  new load address (relative to base)
// ^<addr>  fixup (store current address at base+addr
// *<addr>  relative address (store base+addr)
// $        end
