/*
 * vda — The vida language interpreter
 * Usage:
 *   vda run <file.vda>    Run a vida script
 *   vda eval "<code>"     Evaluate a vida expression
 *   vda version            Print version info
 *   vda help               Print usage
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vida_lexer.h"
#include "vida_value.h"
#include "vida_vm.h"
#include "vida_compiler.h"

#define VIDA_VERSION "0.2.0"

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: Could not open file '%s'\n", path);
        return NULL;
    }
    
    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);
    
    char *buffer = malloc(file_size + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Error: Not enough memory to read '%s'\n", path);
        fclose(file);
        return NULL;
    }
    
    size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
    if (bytes_read < file_size) {
        fprintf(stderr, "Error: Could not read file '%s'\n", path);
        free(buffer);
        fclose(file);
        return NULL;
    }
    
    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

static int run_source(const char *source) {
    VidaChunk chunk;
    vida_chunk_init(&chunk);
    
    if (!vida_compile(source, &chunk)) {
        vida_chunk_free(&chunk);
        return 65; // compile error
    }
    
    VidaVM vm;
    vida_vm_init(&vm);
    VidaResult result = vida_vm_run(&vm, &chunk);
    vida_vm_free(&vm);
    vida_chunk_free(&chunk);
    
    if (result == VIDA_RUNTIME_ERROR) return 70;
    return 0;
}

static int run_file(const char *path) {
    char *source = read_file(path);
    if (source == NULL) return 74;
    
    int exit_code = run_source(source);
    free(source);
    return exit_code;
}

static void print_usage(void) {
    printf("vida %s\n", VIDA_VERSION);
    printf("\nUsage:\n");
    printf("  vda run <file.vda>    Run a vida script\n");
    printf("  vda eval \"<code>\"     Evaluate vida code\n");
    printf("  vda version           Print version\n");
    printf("  vda help              Print this help\n");
    printf("\nLanguage (use it or remove it: unused locals error, unused globals warn):\n");
    printf("  let x = 1;  fn add(a, b) { return a + b }  if/else  while\n");
    printf("\nCompute builtins:\n");
    printf("  sqrt sin cos tan asin acos atan atan2 pow exp log log10\n");
    printf("  abs floor ceil round min max rand srand\n");
    printf("  str int float type len\n");
    printf("\nExamples:\n");
    printf("  vda run hello.vda\n");
    printf("  vda eval \"print(42 + 8)\"\n");
}

int main(int argc, char *argv[]) {
    // Buffered stdout: print-heavy scripts were flushing per OP_PRINT.
    setvbuf(stdout, NULL, _IOFBF, 8192);
    if (argc < 2) {
        print_usage();
        return 0;
    }
    
    if (strcmp(argv[1], "version") == 0) {
        printf("vida %s\n", VIDA_VERSION);
        return 0;
    }
    
    if (strcmp(argv[1], "help") == 0) {
        print_usage();
        return 0;
    }
    
    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'run' requires a file path\n");
            fprintf(stderr, "Usage: vda run <file.vda>\n");
            return 64;
        }
        return run_file(argv[2]);
    }
    
    if (strcmp(argv[1], "eval") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'eval' requires code\n");
            fprintf(stderr, "Usage: vda eval \"<code>\"\n");
            return 64;
        }
        return run_source(argv[2]);
    }
    
    // If first arg looks like a file, try to run it directly
    const char *ext = strrchr(argv[1], '.');
    if (ext != NULL && strcmp(ext, ".vda") == 0) {
        return run_file(argv[1]);
    }
    
    fprintf(stderr, "Unknown command: '%s'\n", argv[1]);
    fprintf(stderr, "Run 'vda help' for usage.\n");
    return 64;
}
