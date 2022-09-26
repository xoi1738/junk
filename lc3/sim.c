#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <signal.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#   include <windows.h>
#   include <conio.h>
#elif defined(__linux__)
#   include <unistd.h>
#   include <fcntl.h>
#   include <sys/time.h>
#   include <sys/types.h>
#   include <sys/termios.h>
#   include <sys/mman.h>
#endif

enum {
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC,   // program counter
    R_COND, // condition flag
    R_COUNT // number of registers
};

enum {
    FL_POS = 1 << 0, // P
    FL_ZRO = 1 << 1, // Z
    FL_NEG = 1 << 2, // N
};

enum {
    OP_BR = 0, // branch
    OP_ADD,    // add
    OP_LD,     // load
    OP_ST,     // store
    OP_JSR,    // jump register
    OP_AND,    // bitwise and
    OP_LDR,    // load register
    OP_STR,    // store register
    OP_RTI,    // unused
    OP_NOT,    // bitwise not
    OP_LDI,    // load indirect
    OP_STI,    // store indirect
    OP_JMP,    // jump
    OP_RES,    // reserved (unused)
    OP_LEA,    // load effective address
    OP_TRAP    // execute trap
};

enum {
    MR_KBSR = 0xfe00, // keyboard status
    MR_KBDR = 0xfe02  // keyboard data
};

enum {
    TRAP_GETC = 0x20,  // get character from keyboard, not echoed onto the terminal
    TRAP_OUT = 0x21,   // output a character
    TRAP_PUTS = 0x22,  // output a word string
    TRAP_IN = 0x23,    // get character from keyboard, echoed onto the terminal
    TRAP_PUTSP = 0x24, // output a byte string
    TRAP_HALT = 0x25   // halt the program
};

#define MEMORY_MAX UINT16_MAX

uint16_t memory[MEMORY_MAX];  // 65536 locations
uint16_t registers[R_COUNT];

#define PC_START 0x3000

#if defined(_WIN32) || defined(__CYGWIN__)
HANDLE input_handle;
DWORD fdw_mode, fdw_original_mode;
#elif defined(__linux__)
struct termios original_tio;
#endif

void restore_input_buffering(void);

void disable_input_buffering(void) {
    atexit(restore_input_buffering);

#if defined(_WIN32) || defined(__CYGWIN__)
    input_handle = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(input_handle, &fdw_original_mode); // save original mode

    fdw_mode = fdw_original_mode
            ^ ENABLE_ECHO_INPUT  // no input echo
            ^ ENABLE_LINE_INPUT; // return when one or more characters are available

    SetConsoleMode(input_handle, fdw_mode); // set new mode
    FlushConsoleInputBuffer(input_handle); // clear buffer

#elif defined(__linux__)
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;

    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

#endif
}

void restore_input_buffering(void) {
#if defined(_WIN32) || defined(__CYGWIN__)
    SetConsoleMode(input_handle, fdw_original_mode);

#elif defined(__linux__)
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);

#endif
}

uint16_t check_key(void) {
#if defined(_WIN32) || defined(__CYGWIN__)
    return WaitForSingleObject(input_handle, 1000) == WAIT_OBJECT_0 && _kbhit();

#elif defined(__linux__)
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    return select(1, &readfds, NULL, NULL, &timeout) != 0;

#endif
}

void handle_interrupt(int signal) {
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

uint16_t sign_extend(uint16_t x, int bit_count) {
    if ((x >> (bit_count - 1)) & 1) {
        x |= (0xFFFF << bit_count);
    }
    return x;
}

uint16_t swap16(uint16_t x) {
    return (x << 8) | (x >> 8);
}

void update_flags(uint16_t r) {
    if (registers[r] == 0) {
        registers[R_COND] = FL_ZRO;
    } else if (registers[r] >> 15) {
        // a 1 in the left-most bit indicates negative sign
        registers[R_COND] = FL_NEG;
    } else {
        registers[R_COND] = FL_POS;
    }
}

void read_image_file(FILE *file) {
    // the origin tells us where in memory to place the image
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    // we know the maximum file size so we only need one fread
    uint16_t max_read = MEMORY_MAX - origin;
    uint16_t *p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    // swap to little endian
    while (read-- > 0) {
        *p = swap16(*p);
        ++p;
    }
}

int read_image(const char *image_path) {
    FILE *file = fopen(image_path, "rb");
    if (!file) {
        return 0;
    };

    read_image_file(file);
    fclose(file);
    return 1;
}

void mem_write(uint16_t address, uint16_t val) {
    memory[address] = val;
}

uint16_t mem_read(uint16_t address) {
    if (address == MR_KBSR) {
        if (check_key()) {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        } else {
            memory[MR_KBSR] = 0;
        }
    }
    return memory[address];
}

void cmd_add(uint16_t instr) {
    // destination register
    uint16_t dr = (instr >> 9) & 0x7;
    // first operand
    uint16_t sr1 = (instr >> 6) & 0x7;
    // whether we are in immediate mode
    uint16_t imm_flag = (instr >> 5) & 0x1;

    if (imm_flag) {
        uint16_t imm5 = sign_extend(instr & 0x1f, 5);
        registers[dr] = registers[sr1] + imm5;
    } else {
        // second operand
        uint16_t sr2 = instr & 0x7;
        registers[dr] = registers[sr1] + registers[sr2];
    }
    // update cond flags based on dr as it contains the result
    update_flags(dr);
}

void cmd_and(uint16_t instr) {
    uint16_t dr = (instr >> 9) & 0x7;
    uint16_t sr1 = (instr >> 6) & 0x7;
    uint16_t imm_flag = (instr >> 5) & 0x1;

    if (imm_flag) {
        uint16_t imm5 = sign_extend(instr & 0x1f, 5);
        registers[dr] = registers[sr1] & imm5;
    } else {
        uint16_t sr2 = instr & 0x7;
        registers[dr] = registers[sr1] & registers[sr2];
    }
    update_flags(dr);
}

void cmd_not(uint16_t instr) {
    uint16_t dr = (instr >> 9) & 0x7;
    uint16_t sr = (instr >> 6) & 0x7;

    registers[dr] = ~registers[sr];
    update_flags(dr);
}

void cmd_br(uint16_t instr) {
    uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);
    uint16_t cond_flag = (instr >> 9) & 0x7;

    // check if any of flags is set in register
    // if yes, pc counter points to instruction guided by pc_offset
    // otherwise, pc counter points to the next instruction
    if (cond_flag & registers[R_COND]) {
        registers[R_PC] += pc_offset;
    }
}

void cmd_jmp(uint16_t instr) {
    // Also handles RET when sr1 (base register position) is 7
    uint16_t sr1 = (instr >> 6) & 0x7;
    registers[R_PC] = registers[sr1];
}

void cmd_jsr(uint16_t instr) {
    uint16_t long_flag = (instr >> 11) & 1;
    registers[R_R7] = registers[R_PC];

    if (long_flag) {
        uint16_t long_pc_offset = sign_extend(instr & 0x7ff, 11);
        registers[R_PC] += long_pc_offset;  // JSR
    } else {
        uint16_t sr1 = (instr >> 6) & 0x7;
        registers[R_PC] = registers[sr1]; // JSRR
    }
}

void cmd_ld(uint16_t instr) {
    uint16_t dr = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);
    registers[dr] = mem_read(registers[R_PC] + pc_offset);
    update_flags(dr);
}

void cmd_ldi(uint16_t instr) {
    // destination register (DR)
    uint16_t dr = (instr >> 9) & 0x7;
    // PCoffset 9
    uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);
    // add pc_offset to the current PC, look at that memory location to get the final address
    registers[dr] = mem_read(mem_read(registers[R_PC] + pc_offset));
    update_flags(dr);
}

void cmd_ldr(uint16_t instr) {
    uint16_t dr = (instr >> 9) & 0x7;
    uint16_t sr1 = (instr >> 6) & 0x7;
    uint16_t offset = sign_extend(instr & 0x3f, 6);
    registers[dr] = mem_read(registers[sr1] + offset);
    update_flags(dr);
}

void cmd_lea(uint16_t instr) {
    uint16_t dr = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);
    registers[dr] = registers[R_PC] + pc_offset;
    update_flags(dr);
}

void cmd_st(uint16_t instr) {
    uint16_t dr = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);
    mem_write(registers[R_PC] + pc_offset, registers[dr]);
}

void cmd_sti(uint16_t instr) {
    uint16_t dr = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1ff, 9);
    mem_write(mem_read(registers[R_PC] + pc_offset), registers[dr]);
}

void cmd_str(uint16_t instr) {
    uint16_t dr = (instr >> 9) & 0x7;
    uint16_t sr1 = (instr >> 6) & 0x7;
    uint16_t offset = sign_extend(instr & 0x3f, 6);
    mem_write(registers[sr1] + offset, registers[dr]);
}

void cmd_trap(uint16_t instr, int *running) {
    registers[R_R7] = registers[R_PC];

    switch (instr & 0xFF) {
        case TRAP_GETC:
            // read a single ASCII char
            registers[R_R0] = (uint16_t)getchar();
            update_flags(R_R0);
            break;

        case TRAP_OUT:
            putc((char)registers[R_R0], stdout);
            fflush(stdout);
            break;

        case TRAP_PUTS: {
            // one char per word
            uint16_t *c = memory + registers[R_R0];
            while (*c) {
                putc((char)*c, stdout);
                ++c;
            }
            fflush(stdout);
        } break;

        case TRAP_IN: {
            printf("Enter a character: ");
            char c = getchar();
            putc(c, stdout);
            fflush(stdout);
            registers[R_R0] = (uint16_t)c;
            update_flags(R_R0);
        } break;

        case TRAP_PUTSP: {
            // one char per byte (two bytes per word)
            // here we need to swap back to
            // big endian format
            uint16_t *c = memory + registers[R_R0];
            while (*c) {
                char char1 = (*c) & 0xff;
                putc(char1, stdout);
                char char2 = (*c) >> 8;
                if (char2) putc(char2, stdout);
                ++c;
            }
            fflush(stdout);
        } break;

        case TRAP_HALT:
            puts("HALT");
            fflush(stdout);
            *running = 0;
            break;
    }
}

int main(int argc, const char *argv[]) {
    if (argc < 2) {
        // show usage string
        printf("lc3sim [obj-file1] ...\n");
        exit(2);
    }

    for (int j = 1; j < argc; ++j) {
        if (!read_image(argv[j])) {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }

    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    // since exactly one condition flag should be set at any given time, set the Z flag
    registers[R_COND] = FL_ZRO;

    // set the PC to starting position
    // 0x3000 is the default
    registers[R_PC] = PC_START;

    int running = 1;
    while (running) {
        // FETCH
        uint16_t instr = mem_read(registers[R_PC]++);
        uint16_t op = instr >> 12;

        switch (op) {
            case OP_ADD: cmd_add(instr); break;
            case OP_AND: cmd_and(instr); break;
            case OP_NOT: cmd_not(instr); break;
            case OP_BR: cmd_br(instr); break;
            case OP_JMP: cmd_jmp(instr); break;
            case OP_JSR: cmd_jsr(instr); break;
            case OP_LD: cmd_ld(instr); break;
            case OP_LDI: cmd_ldi(instr); break;
            case OP_LDR: cmd_ldr(instr); break;
            case OP_LEA: cmd_lea(instr); break;
            case OP_ST: cmd_st(instr); break;
            case OP_STI: cmd_sti(instr); break;
            case OP_STR: cmd_str(instr); break;
            case OP_TRAP: cmd_trap(instr, &running); break;
            case OP_RES:
            case OP_RTI:
            default: abort();
        }
    }
    return 0;
}
