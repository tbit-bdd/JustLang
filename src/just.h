#ifndef JUST_H
#define JUST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #include <windows.h>
#else
    #include <dlfcn.h>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define closesocket close
#endif

#ifndef MAX_CODE
    #define MAX_CODE 100000000
#endif
#ifndef MAX_TOKENS
    #define MAX_TOKENS 10000000
#endif
#ifndef MAX_VARS
    #define MAX_VARS 1000000
#endif
#ifndef MAX_FUNCS
    #define MAX_FUNCS 100000
#endif
#ifndef MAX_ARGS
    #define MAX_ARGS 256
#endif
#ifndef MAX_STRING
    #define MAX_STRING 65536
#endif
#ifndef MAX_SCOPE_DEPTH
    #define MAX_SCOPE_DEPTH 10000
#endif
#ifndef MAX_ITERATIONS
    #define MAX_ITERATIONS 100000000
#endif
#ifndef GC_THRESHOLD
    #define GC_THRESHOLD 50000
#endif

typedef struct Value Value;
typedef struct GCNode GCNode;
typedef struct JustState JustState;

typedef Value* (*NativeFunc)(JustState*, Value**, int);
typedef void (*RegisterFunc)(const char*, NativeFunc);

struct GCNode {
    Value *value;
    GCNode *next;
    bool marked;
};

typedef enum { 
    TYPE_NULL, 
    TYPE_NUMBER, 
    TYPE_STRING, 
    TYPE_BOOL, 
    TYPE_OBJECT, 
    TYPE_ARRAY, 
    TYPE_FUNCTION, 
    TYPE_NATIVE_FUNC 
} ValueType;

struct Value { 
    ValueType type; 
    GCNode *gc_node;
    union { 
        double number; 
        char *string; 
        bool boolean; 
        struct { 
            char **keys; 
            Value **values; 
            int count, capacity; 
        } object; 
        struct { 
            Value **items; 
            int count, capacity; 
        } array; 
        NativeFunc native_func; 
    } data; 
    bool marked;
};

typedef enum { 
    FLOW_NORMAL, 
    FLOW_BREAK, 
    FLOW_CONTINUE, 
    FLOW_RETURN 
} ControlFlow;

typedef struct { 
    char *name; 
    Value *value; 
    bool constant; 
} Variable;

typedef struct { 
    char *name;
    char **params; 
    int param_count; 
    int body_start; 
    int body_end; 
    bool is_native; 
    NativeFunc native_func; 
} Function;

typedef struct { 
    Variable *vars; 
    int var_count; 
    int var_capacity; 
} Scope;

struct JustState {
    // GC
    GCNode *gc_head;
    GCNode *gc_tail;
    int gc_count;
    int total_allocations;
    int gc_threshold;
    
    // Scopes
    Scope *scopes[MAX_SCOPE_DEPTH];
    int scope_depth;
    
    // Functions
    Function *funcs;
    int func_count;
    int func_capacity;
    
    // Tokens
    char **tokens;
    int token_count;
    int token_capacity;
    
    // Control flow
    ControlFlow current_flow;
    Value *return_value;
    
    // State
    int current_line;
    bool winsock_initialized;
    void **loaded_plugins;
    int loaded_plugin_count;
    
    bool builtins_registered;
    char *imported_files[100];
    int imported_file_count;
    
#ifdef _WIN32
    bool win10_ansi_supported;
#endif
};

JustState* just_init(void);
void just_destroy(JustState *j);

Value* just_eval(JustState *j, const char *code);
void just_eval_file(JustState *j, const char *filename);

void just_register_function(JustState *j, const char *name, NativeFunc func);

Value* just_get_var(JustState *j, const char *name);
void just_set_var(JustState *j, const char *name, Value *val);
void just_set_const(JustState *j, const char *name, Value *val);

Value* just_number(double n);
Value* just_string(const char *s);
Value* just_bool(bool b);
Value* just_null(void);

double just_as_number(Value *v);
const char* just_as_string(Value *v);
bool just_as_bool(Value *v);
ValueType just_get_type(Value *v);
const char* just_type_name(Value *v);

void just_gc_collect(JustState *j);
void just_gc_set_threshold(JustState *j, int threshold);
int just_gc_get_count(JustState *j);
int just_gc_get_allocations(JustState *j);

Value* just_object_new(JustState *j);
void just_object_set(Value *obj, const char *key, Value *val);
Value* just_object_get(Value *obj, const char *key);
bool just_object_has(Value *obj, const char *key);

Value* just_array_new(JustState *j);
void just_array_push(Value *arr, Value *val);
Value* just_array_get(Value *arr, int index);
void just_array_set(Value *arr, int index, Value *val);
int just_array_length(Value *arr);

char* just_to_json(JustState *j, Value *v);
Value* just_from_json(JustState *j, const char *json);
char* just_to_string(JustState *j, Value *v);

void just_set_debug(JustState *j, bool on);
void just_print_state(JustState *j);

const char* just_version(void);
int just_version_major(void);
int just_version_minor(void);
int just_version_patch(void);

#endif