#include "just.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    printf("Just Language v%s\n", just_version());
    printf("Type 'exit' or 'quit' to exit\n\n");
    
    JustState *j = just_init();
    if (!j) {
        fprintf(stderr, "FATAL: Failed to initialize Just interpreter\n");
        return 1;
    }
    
    if (argc < 2) {
        // REPL mode
        char *line = malloc(4096);
        if (!line) {
            fprintf(stderr, "FATAL: Memory allocation failed\n");
            just_destroy(j);
            return 1;
        }
        
        while (1) {
            printf("> ");
            if (!fgets(line, 4096, stdin)) break;
            
            // Remove newline
            line[strcspn(line, "\n")] = 0;
            
            // Check exit
            if (strcmp(line, "exit") == 0 || 
                strcmp(line, "quit") == 0 || 
                strcmp(line, "q") == 0) {
                break;
            }
            
            // Skip empty lines
            if (strlen(line) == 0) continue;
            
            // Evaluate and show result
            Value *result = just_eval(j, line);
            if (result && result->type != TYPE_NULL) {
                char *s = just_to_string(j, result);
                if (s) {
                    printf("%s\n", s);
                    free(s);
                }
            }
        }
        
        free(line);
        printf("\nGoodbye!\n");
        
    } else {
        // File mode
        printf("Executing: %s\n", argv[1]);
        just_eval_file(j, argv[1]);
    }
    
    just_destroy(j);
    return 0;
}