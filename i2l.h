// I2L interpreter
// Copyright 2016 Eric Smith <spacewar@gmail.com>

#define MAX_LEVEL 8
extern uint16_t display[MAX_LEVEL];

#define MAX_MEM 0x10000
extern uint8_t mem[MAX_MEM];

#define STACK_MIN     0x0100
#define INITIAL_STACK 0x01ff

#define CODE_START 0x1700

extern uint16_t heap_start;
extern uint16_t heap_limit;


#define REAL_SIZE 5
#define FLOATING_POINT false


// XPL V4D intrinsics have 0x40 added to intrinsic number,
// XPL V5.6D does not
#define INTRINSIC_OFFSET 0x40
#define INTRINSIC_MAX 128


extern int level;    // current level
extern uint16_t pc;  // program counter
extern uint16_t sp;  // stack pointer
extern uint16_t hp;  // heap pointer

extern bool run;
extern bool rerun;

extern bool trace;

extern bool trap;
extern int err;

extern int16_t div_remainder;


#define XPL0_EOF 0x1a


typedef void opfn_t(void);

typedef enum
{
  CLASS_NO_OPERAND            = 0,   // 1
  CLASS_ONE_BYTE_OPERAND      = 2,   // 2  8-bit operand
  CLASS_TWO_BYTE_OPERAND      = 3,   // 3  16-bit value
  CLASS_ADDRESS               = 7,   // 3  16-bit address
  CLASS_LEVEL_OFFSET          = 10,  // 3  8-bit level, 8-bit offset
  CLASS_LEVEL_ADDRESS         = 15,  // 4  8-bit level, 16-bit address
  CLASS_REAL_OPERAND          = 16,  // 1+RLSIZE
  CLASS_ADDRESS_REAL_ARRAY    = 36,  // 1+RLSIZE, but contains pointer to reloc
  CLASS_ADDRESS_BASE_RELATIVE = 64,  // 3  external procedure
} class_t;

typedef struct
{
  opfn_t *fn;
  char *name;
  class_t class;
} opinfo_t;

extern const opinfo_t op[];

typedef struct
{
  char *name;
  opfn_t *fn;
} intrinsic_info_t;

extern const intrinsic_info_t intrinsic[];


enum
{
  ERR_NONE = 0,

  // standard I2L error numbers
  ERR_DIVISION_BY_ZERO = 1,
  ERR_HEAP_OVERFLOW = 2,
  ERR_IO_ERROR = 3,
  ERR_BAD_OPCODE = 4,
  ERR_BAD_INTRINSIC = 5,
  ERR_LOADER_FAILURE = 10,

  // more specific I2L loader errors, all mapped to ERR_LOADER_FAILURE
  ERR_NO_I2L_FILE = 10,
  ERR_I2L_UNEXPECTED_EOF = 10,
  ERR_I2L_UNEXPECTED_CHAR = 10,

  // non-standard I2L errors
  ERR_BAD_CMD_LINE,
  ERR_ABORT,
  ERR_UNIMPLEMENTED_OPCODE,
  ERR_UNIMPLEMENTED_INTRINSIC,
  ERR_BAD_LEVEL,
  ERR_STACK_UNDERFLOW,
  ERR_STACK_OVERFLOW,
  ERR_HEAP_UNDERFLOW,
  ERR_INTERNAL_ERROR,
};

void fatal_error(int num, char *fmt, ...);
