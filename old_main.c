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

#define MAX_CODE 100000000
#define MAX_TOKENS 10000000
#define MAX_VARS 1000000
#define MAX_FUNCS 100000
#define MAX_ARGS 256
#define MAX_STRING 65536
#define MAX_SCOPE_DEPTH 10000
#define MAX_ITERATIONS 100000000
#define GC_THRESHOLD 50000

int current_line = 1;
int total_allocations = 0;

char* str_dup(const char *s) { if (!s) return NULL; char *d = malloc(strlen(s) + 1); if (d) strcpy(d, s); return d; }

typedef enum { TYPE_NULL, TYPE_NUMBER, TYPE_STRING, TYPE_BOOL, TYPE_OBJECT, TYPE_ARRAY, TYPE_FUNCTION, TYPE_NATIVE_FUNC } ValueType;
typedef struct Value Value;
typedef struct GCNode GCNode;
typedef Value* (*NativeFunc)(Value**, int);

struct GCNode {
    Value *value;
    GCNode *next;
    bool marked;
};

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

GCNode *gc_head = NULL;
GCNode *gc_tail = NULL;
int gc_count = 0;

typedef enum { FLOW_NORMAL, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN } ControlFlow;
typedef struct { char *name; Value *value; bool constant; } Variable;
typedef struct { char *name, **params; int param_count, body_start, body_end; bool is_native; NativeFunc native_func; } Function;
typedef struct { Variable *vars; int var_count, var_capacity; } Scope;
typedef void (*RegisterFunc)(const char*, NativeFunc);

Scope *scopes[MAX_SCOPE_DEPTH]; int scope_depth = 0;
Function *funcs = NULL; int func_count = 0, func_capacity = 0;
char **tokens = NULL; int token_count = 0, token_capacity = 0;
ControlFlow current_flow = FLOW_NORMAL; Value *return_value = NULL;
bool winsock_initialized = false;
void **loaded_plugins = NULL; int loaded_plugin_count = 0;
#ifdef _WIN32
bool win10_ansi_supported = false;
#endif

void just_error(const char *msg) { fprintf(stderr, "\nERROR (line %d): %s\n", current_line, msg); fprintf(stderr, "  Stack trace:\n"); for (int s = scope_depth - 1; s >= 0; s--) fprintf(stderr, "  [%d] scope\n", s); }

// FORWARD DECLARATIONS
Value* create_value(ValueType type);
Value* create_number(double n);
Value* create_string(const char *s);
Value* create_bool(bool b);
Value* create_object();
Value* create_array();
Value* clone_value(Value *v);
void object_set(Value *obj, const char *key, Value *val);
Value* object_get(Value *obj, const char *key);
bool object_has(Value *obj, const char *key);
void array_push(Value *arr, Value *val);
Value* array_get(Value *arr, int index);
void array_set(Value *arr, int index, Value *val);
double value_to_number(Value *v);
bool value_to_bool(Value *v);
char* value_to_string_raw(Value *v);
Value* value_to_string(Value *v);
Value* value_to_json(Value *v);
Value* json_parse_value(const char **p);
void push_scope();
void pop_scope();
Variable* find_var(const char *name);
void set_var(const char *name, Value *value, bool constant);
void add_func(const char *name, char **params, int param_count, int start, int end);
Function* find_func(const char *name);
void register_builtins();
void tokenize(const char *src);
Value* eval_expression(int *pos);
int execute_statement(int pos);
int execute_block(int start, int end);
void run_code(const char *src);
void shutdown_cleanup();
void add_native_func(const char *name, NativeFunc func);

// GC functions
void gc_add_node(Value *v) {
    if (!v) return;
    GCNode *node = malloc(sizeof(GCNode));
    if (!node) { just_error("GC: Out of memory"); exit(1); }
    node->value = v;
    node->marked = false;
    node->next = NULL;
    if (gc_tail) { gc_tail->next = node; gc_tail = node; }
    else { gc_head = gc_tail = node; }
    v->gc_node = node;
    gc_count++;
    total_allocations++;
}

void gc_mark_value(Value *v) {
    if (!v || v->marked) return;
    
    Value *stack[10000];
    int sp = 0;
    stack[sp++] = v;
    v->marked = true;
    
    while (sp > 0) {
        Value *cur = stack[--sp];
        
        switch (cur->type) {
            case TYPE_OBJECT:
                for (int i = 0; i < cur->data.object.count; i++) {
                    Value *child = cur->data.object.values[i];
                    if (child && !child->marked) {
                        child->marked = true;
                        if (sp < 10000) stack[sp++] = child;
                    }
                }
                break;
            case TYPE_ARRAY:
                for (int i = 0; i < cur->data.array.count; i++) {
                    Value *child = cur->data.array.items[i];
                    if (child && !child->marked) {
                        child->marked = true;
                        if (sp < 10000) stack[sp++] = child;
                    }
                }
                break;
            default: break;
        }
    }
}

void gc_mark_roots() {
    for (int s = 0; s < scope_depth; s++)
        for (int i = 0; i < scopes[s]->var_count; i++)
            gc_mark_value(scopes[s]->vars[i].value);
    if (return_value) gc_mark_value(return_value);
}

void gc_sweep() {
    GCNode *prev = NULL, *curr = gc_head;
    while (curr) {
        if (!curr->marked && curr->value) {
            Value *v = curr->value;
            switch (v->type) {
                case TYPE_STRING: free(v->data.string); break;
                case TYPE_OBJECT:
                    for (int i = 0; i < v->data.object.count; i++) free(v->data.object.keys[i]);
                    free(v->data.object.keys); free(v->data.object.values); break;
                case TYPE_ARRAY: free(v->data.array.items); break;
                default: break;
            }
            v->gc_node = NULL;
            free(v);
            curr->value = NULL; gc_count--;
        }
        GCNode *next = curr->next;
        if (!curr->value) {
            if (prev) prev->next = next; else gc_head = next;
            if (curr == gc_tail) gc_tail = prev;
            free(curr);
        } else prev = curr;
        curr = next;
    }
}

void gc_collect() {
    GCNode *node = gc_head;
    while (node) { node->marked = false; if (node->value) node->value->marked = false; node = node->next; }
    gc_mark_roots();
    gc_sweep();
    total_allocations = 0;
}

// Value management
Value* create_value(ValueType type) {
    Value *v = calloc(1, sizeof(Value));
    if (!v) { just_error("Out of memory"); exit(1); }
    v->type = type; v->marked = false;
    gc_add_node(v);
    if (total_allocations >= GC_THRESHOLD) gc_collect();
    return v;
}

Value* create_number(double n) { Value *v = create_value(TYPE_NUMBER); v->data.number = n; return v; }
Value* create_string(const char *s) { Value *v = create_value(TYPE_STRING); v->data.string = s ? str_dup(s) : str_dup(""); return v; }
Value* create_bool(bool b) { Value *v = create_value(TYPE_BOOL); v->data.boolean = b; return v; }
Value* create_object() { Value *v = create_value(TYPE_OBJECT); v->data.object.capacity = 8; v->data.object.keys = malloc(sizeof(char*) * 8); v->data.object.values = malloc(sizeof(Value*) * 8); v->data.object.count = 0; return v; }
Value* create_array() { Value *v = create_value(TYPE_ARRAY); v->data.array.capacity = 8; v->data.array.items = malloc(sizeof(Value*) * 8); v->data.array.count = 0; return v; }

// Object/Array ops
Value* object_get(Value *obj, const char *key) { 
    if (!obj || obj->type != TYPE_OBJECT) return NULL; 
    for (int i = 0; i < obj->data.object.count; i++) 
        if (strcmp(obj->data.object.keys[i], key) == 0) {
            obj->data.object.values[i]->marked = true;
            return obj->data.object.values[i];
        }
    return NULL; 
}

void object_set(Value *obj, const char *key, Value *val) { 
    if (!obj || obj->type != TYPE_OBJECT) return; 
    for (int i = 0; i < obj->data.object.count; i++) 
        if (strcmp(obj->data.object.keys[i], key) == 0) { 
            obj->data.object.values[i] = val; 
            return; 
        } 
    if (obj->data.object.count >= obj->data.object.capacity) { 
        obj->data.object.capacity *= 2; 
        obj->data.object.keys = realloc(obj->data.object.keys, sizeof(char*) * obj->data.object.capacity); 
        obj->data.object.values = realloc(obj->data.object.values, sizeof(Value*) * obj->data.object.capacity); 
    } 
    obj->data.object.keys[obj->data.object.count] = str_dup(key); 
    obj->data.object.values[obj->data.object.count] = val; 
    obj->data.object.count++; 
}
bool object_has(Value *obj, const char *key) { if (!obj || obj->type != TYPE_OBJECT) return false; for (int i = 0; i < obj->data.object.count; i++) if (strcmp(obj->data.object.keys[i], key) == 0) return true; return false; }
Value* clone_value_internal(Value *v, int depth) {
    if (depth > 1000) return create_value(TYPE_NULL);
    if (!v) return create_value(TYPE_NULL);
    switch (v->type) {
        case TYPE_NULL: return create_value(TYPE_NULL);
        case TYPE_NUMBER: return create_number(v->data.number);
        case TYPE_BOOL: return create_bool(v->data.boolean);
        case TYPE_STRING: return create_string(v->data.string);
        case TYPE_OBJECT: {
            Value *n = create_object();
            for (int i = 0; i < v->data.object.count; i++) 
                object_set(n, v->data.object.keys[i], clone_value_internal(v->data.object.values[i], depth + 1));
            return n;
        }
        case TYPE_ARRAY: {
            Value *n = create_array();
            for (int i = 0; i < v->data.array.count; i++) 
                array_push(n, clone_value_internal(v->data.array.items[i], depth + 1));
            return n;
        }
        default: return v;
    }
}
Value* clone_value(Value *v) { return clone_value_internal(v, 0); }
Value* array_get(Value *arr, int index) { 
    if (!arr || arr->type != TYPE_ARRAY || index < 0 || index >= arr->data.array.count) 
        return create_value(TYPE_NULL); 
    arr->data.array.items[index]->marked = true;
    return arr->data.array.items[index];
}

void array_push(Value *arr, Value *val) { 
    if (!arr || arr->type != TYPE_ARRAY) return; 
    if (arr->data.array.count >= arr->data.array.capacity) { 
        arr->data.array.capacity *= 2; 
        arr->data.array.items = realloc(arr->data.array.items, sizeof(Value*) * arr->data.array.capacity); 
    } 
    arr->data.array.items[arr->data.array.count++] = val; 
}
void array_set(Value *arr, int index, Value *val) { 
    if (!arr || arr->type != TYPE_ARRAY || index < 0 || index >= arr->data.array.count) return; 
    arr->data.array.items[index] = val; 
}
double value_to_number(Value *v) { if (!v) return 0; switch (v->type) { case TYPE_NUMBER: return v->data.number; case TYPE_BOOL: return v->data.boolean ? 1.0 : 0.0; case TYPE_STRING: return atof(v->data.string); default: return 0; } }

bool value_to_bool(Value *v) {
    if (!v) return false;
    switch (v->type) {
        case TYPE_BOOL: return v->data.boolean;
        case TYPE_NUMBER: return v->data.number != 0;
        case TYPE_STRING: return strlen(v->data.string) > 0;
        case TYPE_NULL: return false;
        case TYPE_OBJECT: return true;
        case TYPE_ARRAY: return true;
        default: return true;
    }
}

char* value_to_string_raw(Value *v) { if (!v) return str_dup("null"); switch (v->type) { case TYPE_NULL: return str_dup("null"); case TYPE_NUMBER: { char b[64]; if (v->data.number == (int)v->data.number) snprintf(b, 64, "%d", (int)v->data.number); else snprintf(b, 64, "%g", v->data.number); return str_dup(b); } case TYPE_STRING: return str_dup(v->data.string); case TYPE_BOOL: return str_dup(v->data.boolean ? "true" : "false"); case TYPE_OBJECT: { char **fs = malloc(sizeof(char*) * v->data.object.count); int tl = 2; for (int i = 0; i < v->data.object.count; i++) { char *vs = value_to_string_raw(v->data.object.values[i]); if (!vs) vs = str_dup("null"); int n = strlen(v->data.object.keys[i]) + 2 + strlen(vs); fs[i] = malloc(n + 1); sprintf(fs[i], "%s: %s", v->data.object.keys[i], vs); tl += strlen(fs[i]); if (i < v->data.object.count - 1) tl += 2; free(vs); } char *r = malloc(tl + 1); int p = 0; p += sprintf(r + p, "{"); for (int i = 0; i < v->data.object.count; i++) { p += sprintf(r + p, "%s", fs[i]); if (i < v->data.object.count - 1) p += sprintf(r + p, ", "); free(fs[i]); } p += sprintf(r + p, "}"); free(fs); return r; } case TYPE_ARRAY: { char **is = malloc(sizeof(char*) * v->data.array.count); int tl = 2; for (int i = 0; i < v->data.array.count; i++) { is[i] = value_to_string_raw(v->data.array.items[i]); tl += strlen(is[i]); if (i < v->data.array.count - 1) tl += 2; } char *r = malloc(tl + 1); int p = 0; p += sprintf(r + p, "["); for (int i = 0; i < v->data.array.count; i++) { p += sprintf(r + p, "%s", is[i]); if (i < v->data.array.count - 1) p += sprintf(r + p, ", "); free(is[i]); } p += sprintf(r + p, "]"); free(is); return r; } default: return str_dup("<function>"); } }
Value* value_to_string(Value *v) { if (!v) return create_string("null"); if (v->type == TYPE_STRING) return v; char *s = value_to_string_raw(v); Value *r = create_string(s); free(s); return r; }
Value* value_to_json(Value *v) { if (!v) return create_string("null"); switch (v->type) { case TYPE_NULL: return create_string("null"); case TYPE_NUMBER: { char b[64]; snprintf(b, 64, "%g", v->data.number); return create_string(b); } case TYPE_BOOL: return create_string(v->data.boolean ? "true" : "false"); case TYPE_STRING: { char *b = malloc(strlen(v->data.string) + 3); sprintf(b, "\"%s\"", v->data.string); Value *r = create_string(b); free(b); return r; } case TYPE_ARRAY: { char **ps = malloc(sizeof(char*) * v->data.array.count); int tl = 2; for (int i = 0; i < v->data.array.count; i++) { Value *j = value_to_json(v->data.array.items[i]); ps[i] = j->data.string; tl += strlen(ps[i]); if (i < v->data.array.count - 1) tl++; j->data.string = NULL; } char *b = malloc(tl + 1); int p = 0; b[p++] = '['; for (int i = 0; i < v->data.array.count; i++) { p += sprintf(b + p, "%s", ps[i]); if (i < v->data.array.count - 1) b[p++] = ','; free(ps[i]); } b[p++] = ']'; b[p] = '\0'; free(ps); Value *r = create_string(b); free(b); return r; } case TYPE_OBJECT: { char **ks = malloc(sizeof(char*) * v->data.object.count); char **vs = malloc(sizeof(char*) * v->data.object.count); int tl = 2; for (int i = 0; i < v->data.object.count; i++) { Value *j = value_to_json(v->data.object.values[i]); ks[i] = v->data.object.keys[i]; vs[i] = j->data.string; tl += strlen(ks[i]) + 3 + strlen(vs[i]); if (i < v->data.object.count - 1) tl++; j->data.string = NULL; } char *b = malloc(tl + 1); int p = 0; b[p++] = '{'; for (int i = 0; i < v->data.object.count; i++) { p += sprintf(b + p, "\"%s\":%s", ks[i], vs[i]); if (i < v->data.object.count - 1) b[p++] = ','; free(vs[i]); } b[p++] = '}'; b[p] = '\0'; free(ks); free(vs); Value *r = create_string(b); free(b); return r; } default: return create_string("null"); } }

// JSON PARSER
Value* json_parse_value(const char **p) {
    while (**p == ' ' || **p == '\n' || **p == '\t' || **p == '\r') (*p)++;
    if (**p == '{') {
        (*p)++; Value *o = create_object();
        while (**p && **p != '}') {
            while (**p == ' ' || **p == ',' || **p == '\n' || **p == '\t' || **p == '\r') (*p)++;
            if (**p == '}') break;
            if (**p == '"') {
                (*p)++; char key_buf[MAX_STRING]; int kl = 0;
                while (**p && **p != '"' && kl < MAX_STRING - 1) {
                    if (**p == '\\' && *(*p + 1)) { (*p)++;
                        switch (**p) { case '"': key_buf[kl++]='"'; break; case '\\': key_buf[kl++]='\\'; break; case '/': key_buf[kl++]='/'; break; case 'n': key_buf[kl++]='\n'; break; case 't': key_buf[kl++]='\t'; break; case 'r': key_buf[kl++]='\r'; break; case 'f': key_buf[kl++]='\f'; break; case 'b': key_buf[kl++]='\b'; break; default: key_buf[kl++]='\\'; key_buf[kl++]=**p; }
                    } else key_buf[kl++] = **p;
                    (*p)++;
                }
                key_buf[kl] = '\0'; if (**p == '"') (*p)++;
                while (**p == ' ' || **p == ':') (*p)++;
                Value *v = json_parse_value(p);
                object_set(o, key_buf, v);
            }
        }
        if (**p == '}') (*p)++; return o;
    }
    if (**p == '[') {
        (*p)++; Value *a = create_array();
        while (**p && **p != ']') {
            while (**p == ' ' || **p == ',' || **p == '\n' || **p == '\t' || **p == '\r') (*p)++;
            if (**p == ']') break;
            Value *v = json_parse_value(p); array_push(a, v);
        }
        if (**p == ']') (*p)++; return a;
    }
    if (**p == '"') {
        (*p)++; char str_buf[MAX_STRING]; int sl = 0;
        while (**p && **p != '"' && sl < MAX_STRING - 1) {
            if (**p == '\\' && *(*p + 1)) { (*p)++;
                switch (**p) { case '"': str_buf[sl++]='"'; break; case '\\': str_buf[sl++]='\\'; break; case '/': str_buf[sl++]='/'; break; case 'n': str_buf[sl++]='\n'; break; case 't': str_buf[sl++]='\t'; break; case 'r': str_buf[sl++]='\r'; break; case 'f': str_buf[sl++]='\f'; break; case 'b': str_buf[sl++]='\b'; break; default: str_buf[sl++]='\\'; str_buf[sl++]=**p; }
            } else str_buf[sl++] = **p;
            (*p)++;
        }
        str_buf[sl] = '\0'; if (**p == '"') (*p)++; return create_string(str_buf);
    }
    if (**p == 't' || **p == 'f') { bool b = (**p == 't'); while (**p && isalpha(**p)) (*p)++; return create_bool(b); }
    if (**p == 'n') { while (**p && isalpha(**p)) (*p)++; return create_value(TYPE_NULL); }
    char *ep; double n = strtod(*p, &ep); if (ep != *p) { *p = ep; return create_number(n); }
    return create_value(TYPE_NULL);
}

// Scopes
void push_scope() { if (scope_depth >= MAX_SCOPE_DEPTH) { just_error("Maximum scope depth exceeded"); exit(1); } Scope *s = calloc(1, sizeof(Scope)); s->var_capacity = 16; s->vars = malloc(sizeof(Variable) * 16); scopes[scope_depth++] = s; }
void pop_scope() { if (scope_depth <= 0) return; scope_depth--; Scope *s = scopes[scope_depth]; for (int i = 0; i < s->var_count; i++) { free(s->vars[i].name); } free(s->vars); free(s); }
Variable* find_var(const char *name) { for (int s = scope_depth - 1; s >= 0; s--) for (int i = 0; i < scopes[s]->var_count; i++) if (strcmp(scopes[s]->vars[i].name, name) == 0) return &scopes[s]->vars[i]; return NULL; }
void set_var(const char *name, Value *value, bool constant) { 
    Scope *cur = scopes[scope_depth - 1]; 
    for (int i = 0; i < cur->var_count; i++) 
        if (strcmp(cur->vars[i].name, name) == 0) { 
            if (cur->vars[i].constant) { just_error("Cannot reassign constant"); return; } 
            cur->vars[i].value = value; 
            return; 
        } 
    if (cur->var_count >= cur->var_capacity) { 
        cur->var_capacity *= 2; 
        cur->vars = realloc(cur->vars, sizeof(Variable) * cur->var_capacity); 
    } 
    cur->vars[cur->var_count].name = str_dup(name); 
    cur->vars[cur->var_count].value = value; 
    cur->vars[cur->var_count].constant = constant; 
    cur->var_count++; 
}
void add_func(const char *name, char **params, int param_count, int start, int end) { if (func_count >= func_capacity) { func_capacity = func_capacity ? func_capacity * 2 : 16; funcs = realloc(funcs, sizeof(Function) * func_capacity); } funcs[func_count].name = str_dup(name); funcs[func_count].params = malloc(sizeof(char*) * param_count); for (int i = 0; i < param_count; i++) funcs[func_count].params[i] = str_dup(params[i]); funcs[func_count].param_count = param_count; funcs[func_count].body_start = start; funcs[func_count].body_end = end; funcs[func_count].is_native = false; funcs[func_count].native_func = NULL; func_count++; }
Function* find_func(const char *name) { for (int i = 0; i < func_count; i++) if (strcmp(funcs[i].name, name) == 0) return &funcs[i]; return NULL; }

// Builtins
Value* builtin_print(Value **args, int count) { for (int i = 0; i < count; i++) { char *s = value_to_string_raw(args[i]); printf("%s", s); free(s); if (i < count - 1) printf(" "); } printf("\n"); return create_value(TYPE_NULL); }
Value* builtin_type(Value **args, int count) { if (count < 1) return create_string("null"); char *t[] = {"null","number","string","bool","object","array","function","native_function"}; return create_string(t[args[0]->type]); }
Value* builtin_len(Value **args, int count) { if (count < 1) return create_number(0); Value *v = args[0]; if (v->type == TYPE_STRING) return create_number(strlen(v->data.string)); if (v->type == TYPE_ARRAY) return create_number(v->data.array.count); if (v->type == TYPE_OBJECT) return create_number(v->data.object.count); return create_number(0); }
Value* builtin_input(Value **args, int count) { char b[MAX_STRING]; if (fgets(b, MAX_STRING, stdin)) { b[strcspn(b, "\n")] = 0; return create_string(b); } return create_string(""); }
Value* builtin_int(Value **args, int count) { if (count < 1) return create_number(0); return create_number((int)value_to_number(args[0])); }
Value* builtin_str(Value **args, int count) { if (count < 1) return create_string(""); return value_to_string(args[0]); }
Value* builtin_bool(Value **args, int count) { if (count < 1) return create_bool(false); return create_bool(value_to_bool(args[0])); }
Value* builtin_range(Value **args, int count) { Value *a = create_array(); if (count >= 1) { int s = 0, e = (int)value_to_number(args[0]), st = 1; if (count >= 2) { s = e; e = (int)value_to_number(args[1]); } if (count >= 3) st = (int)value_to_number(args[2]); if (st > 0) for (int i = s; i < e; i += st) array_push(a, create_number(i)); else if (st < 0) for (int i = s; i > e; i += st) array_push(a, create_number(i)); } return a; }
Value* builtin_keys(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_OBJECT) return create_array(); Value *a = create_array(); for (int i = 0; i < args[0]->data.object.count; i++) array_push(a, create_string(args[0]->data.object.keys[i])); return a; }
Value* builtin_values(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_OBJECT) return create_array(); Value *a = create_array(); for (int i = 0; i < args[0]->data.object.count; i++) array_push(a, clone_value(args[0]->data.object.values[i])); return a; }
Value* builtin_has(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_OBJECT || args[1]->type != TYPE_STRING) return create_bool(false); return create_bool(object_has(args[0], args[1]->data.string)); }
Value* builtin_read_file(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_string(""); FILE *f = fopen(args[0]->data.string, "r"); if (!f) return create_string(""); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc(sz + 1); fread(b, 1, sz, f); b[sz] = '\0'; fclose(f); Value *v = create_string(b); free(b); return v; }
Value* builtin_write_file(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING) return create_bool(false); FILE *f = fopen(args[0]->data.string, "w"); if (!f) return create_bool(false); fprintf(f, "%s", args[1]->data.string); fclose(f); return create_bool(true); }
Value* builtin_json_export(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_STRING) return create_bool(false); FILE *f = fopen(args[0]->data.string, "w"); if (!f) { return create_bool(false); } Value *j = value_to_json(args[1]); fprintf(f, "%s", j->data.string); fclose(f); return create_bool(true); }
Value* builtin_json(Value **args, int count) { if (count < 1) return create_string("null"); return value_to_json(args[0]); }
Value* builtin_json_parse(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_value(TYPE_NULL); const char *p = args[0]->data.string; return json_parse_value(&p); }

Value* builtin_http_get(Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string("");
#ifdef _WIN32
    if (!winsock_initialized) { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); winsock_initialized = true; }
#endif
    char *url_orig = args[0]->data.string;
    char *url = str_dup(url_orig);
    char host[256] = {0}, path[1024] = "/"; int port = 80;
    Value *result = create_string("");
    
    char *p = strstr(url, "://"); if (p) p += 3; else p = url;
    char *ps = strchr(p, '/'); if (ps) { strncpy(path, ps, 1023); *ps = '\0'; }
    char *pp = strchr(p, ':'); if (pp) { *pp = '\0'; port = atoi(pp + 1); }
    strncpy(host, p, 255);
    
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { free(url); return result; }
    
    struct sockaddr_in addr; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    struct hostent *he = gethostbyname(host);
    if (!he) { closesocket(sock); free(url); return result; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { closesocket(sock); free(url); return result; }
    
    char req[2048]; snprintf(req, 2048, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    send(sock, req, strlen(req), 0);
    
    char *buf = malloc(MAX_STRING); int total = 0, n;
    while ((n = recv(sock, buf + total, MAX_STRING - total - 1, 0)) > 0) total += n;
    buf[total] = '\0'; closesocket(sock);
    
    char *body = strstr(buf, "\r\n\r\n");
    result = body ? create_string(body + 4) : create_string(buf);
    free(buf);
    free(url);
    return result;
}

Value* builtin_import_json(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_value(TYPE_NULL); FILE *f = fopen(args[0]->data.string, "r"); if (!f) return create_value(TYPE_NULL); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc(sz + 1); fread(b, 1, sz, f); b[sz] = '\0'; fclose(f); const char *p = b; Value *v = json_parse_value(&p); free(b); return v; }
Value* builtin_split(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING) return create_array(); char *s = str_dup(args[0]->data.string), *d = args[1]->data.string; Value *a = create_array(); char *saveptr, *token = strtok_r(s, d, &saveptr); while (token) { array_push(a, create_string(token)); token = strtok_r(NULL, d, &saveptr); } free(s); return a; }
Value* builtin_upper(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_string(""); char *s = str_dup(args[0]->data.string); for (int i = 0; s[i]; i++) s[i] = toupper(s[i]); Value *v = create_string(s); free(s); return v; }
Value* builtin_lower(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_string(""); char *s = str_dup(args[0]->data.string); for (int i = 0; s[i]; i++) s[i] = tolower(s[i]); Value *v = create_string(s); free(s); return v; }
Value* builtin_replace(Value **args, int count) { if (count < 3 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING || args[2]->type != TYPE_STRING) return create_string(""); char *s = str_dup(args[0]->data.string), *f = args[1]->data.string, *t = args[2]->data.string; char *p; while ((p = strstr(s, f))) { int pl = p - s; int nl = pl + strlen(t) + strlen(p + strlen(f)) + 1; char *n = malloc(nl); strncpy(n, s, pl); n[pl] = '\0'; strcat(n, t); strcat(n, p + strlen(f)); free(s); s = n; } Value *v = create_string(s); free(s); return v; }
Value* builtin_sqrt(Value **args, int count) { if (count < 1) return create_number(0); return create_number(sqrt(value_to_number(args[0]))); }
Value* builtin_random(Value **args, int count) { double max = count >= 1 ? value_to_number(args[0]) : 1.0; double r = (double)(rand() % 10000) / 10000.0; return create_number(r * max); }
Value* builtin_filter(Value **args, int count) { 
    if (count < 1 || args[0]->type != TYPE_ARRAY) return create_array(); 
    Value *result = create_array(); 
    for (int i = 0; i < args[0]->data.array.count; i++) { 
        Value *item = args[0]->data.array.items[i]; 
        if (value_to_bool(item)) array_push(result, item); 
    } 
    return result; 
}
Value* builtin_map(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_ARRAY || args[1]->type != TYPE_NATIVE_FUNC) return create_array(); Value *result = create_array(); for (int i = 0; i < args[0]->data.array.count; i++) { Value *item = args[0]->data.array.items[i]; item->marked = true; Value *mapped = args[1]->data.native_func(&item, 1); array_push(result, mapped); } return result; }
Value* builtin_floor(Value **args, int count) { if (count < 1) return create_number(0); return create_number(floor(value_to_number(args[0]))); }
Value* builtin_ceil(Value **args, int count) { if (count < 1) return create_number(0); return create_number(ceil(value_to_number(args[0]))); }
Value* builtin_round(Value **args, int count) { if (count < 1) return create_number(0); return create_number(round(value_to_number(args[0]))); }
Value* builtin_now(Value **args, int count) { time_t t = time(NULL); struct tm *tm = localtime(&t); char buf[64]; strftime(buf, 64, "%Y-%m-%d %H:%M:%S", tm); return create_string(buf); }

Value* builtin_http_post(Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING) return create_string("");
#ifdef _WIN32
    if (!winsock_initialized) { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); winsock_initialized = true; }
#endif
    char *url_orig = args[0]->data.string;
    char *url = str_dup(url_orig);
    char *body = args[1]->data.string;
    char host[256] = {0}, path[1024] = "/"; int port = 80;
    Value *result = create_string("");
    
    char *p = strstr(url, "://"); if (p) p += 3; else p = url;
    char *ps = strchr(p, '/'); if (ps) { strncpy(path, ps, 1023); *ps = '\0'; }
    char *pp = strchr(p, ':'); if (pp) { *pp = '\0'; port = atoi(pp + 1); }
    strncpy(host, p, 255);
    
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { free(url); return result; }
    
    struct sockaddr_in addr; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    struct hostent *he = gethostbyname(host);
    if (!he) { closesocket(sock); free(url); return result; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { closesocket(sock); free(url); return result; }
    
    char req[4096]; 
    snprintf(req, 4096, "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", 
             path, host, (int)strlen(body), body);
    send(sock, req, strlen(req), 0);
    
    char *buf = malloc(MAX_STRING); int total = 0, n;
    while ((n = recv(sock, buf + total, MAX_STRING - total - 1, 0)) > 0) total += n;
    buf[total] = '\0'; closesocket(sock);
    
    char *resp = strstr(buf, "\r\n\r\n");
    result = resp ? create_string(resp + 4) : create_string(buf);
    free(buf);
    free(url);
    return result;
}

Value* builtin_exec(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_string(""); char *cmd = args[0]->data.string; char buf[MAX_STRING]; FILE *fp = popen(cmd, "r"); if (!fp) return create_string(""); int total = 0; while (fgets(buf + total, MAX_STRING - total - 1, fp)) total = strlen(buf); pclose(fp); return create_string(buf); }

#ifdef _WIN32
void just_sleep_ms(int ms) { Sleep(ms); }
#else
void just_sleep_ms(int ms) { usleep(ms * 1000); }
#endif
Value* builtin_sleep(Value **args, int count) { if (count < 1) return create_value(TYPE_NULL); int ms = (int)value_to_number(args[0]); if (ms < 0) ms = 0; if (ms > 60000) ms = 60000; just_sleep_ms(ms); return create_value(TYPE_NULL); }
Value* builtin_env(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_string(""); char *val = getenv(args[0]->data.string); return val ? create_string(val) : create_string(""); }

Value* builtin_color(Value **args, int count, const char *ansi_code, int win_color) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string("");
    char *text = args[0]->data.string;
#ifdef _WIN32
    if (win10_ansi_supported) {
        char buf[MAX_STRING]; snprintf(buf, MAX_STRING, "\033[%sm%s\033[0m", ansi_code, text);
        return create_string(buf);
    } else { return create_string(text); }
#else
    char buf[MAX_STRING]; snprintf(buf, MAX_STRING, "\033[%sm%s\033[0m", ansi_code, text);
    return create_string(buf);
#endif
}
Value* builtin_red(Value **args, int count)     { return builtin_color(args, count, "31", 12); }
Value* builtin_green(Value **args, int count)   { return builtin_color(args, count, "32", 10); }
Value* builtin_yellow(Value **args, int count)  { return builtin_color(args, count, "33", 14); }
Value* builtin_blue(Value **args, int count)    { return builtin_color(args, count, "34", 9); }
Value* builtin_magenta(Value **args, int count) { return builtin_color(args, count, "35", 13); }
Value* builtin_cyan(Value **args, int count)    { return builtin_color(args, count, "36", 11); }
Value* builtin_bold(Value **args, int count)    { return builtin_color(args, count, "1", 15); }

typedef struct { char *name; int func_index; } Task;
Task *tasks = NULL; int task_count = 0;
Value* builtin_task(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_value(TYPE_NULL); Function *f = find_func(args[0]->data.string); if (!f) { just_error("Task not found"); return create_value(TYPE_NULL); } push_scope(); ControlFlow of = current_flow; current_flow = FLOW_NORMAL; return_value = NULL; execute_block(f->body_start, f->body_end); current_flow = of; while (scope_depth > 0) pop_scope(); return create_value(TYPE_NULL); }
Value* builtin_watch(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_STRING) return create_value(TYPE_NULL); char *filepath = args[0]->data.string; struct stat st; if (stat(filepath, &st) != 0) { just_error("watch: file not found"); return create_value(TYPE_NULL); } time_t last_modified = st.st_mtime; printf("Watching: %s\n", filepath); while (1) { just_sleep_ms(500); if (stat(filepath, &st) == 0 && st.st_mtime != last_modified) { last_modified = st.st_mtime; if (args[1]->type == TYPE_STRING) { Value *ta[1] = {args[1]}; builtin_task(ta, 1); } } } return create_value(TYPE_NULL); }
Value* builtin_error(Value **args, int count) { if (count >= 1 && args[0]->type == TYPE_STRING) just_error(args[0]->data.string); return create_value(TYPE_NULL); }

Value* builtin_load_plugin(Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_bool(false);
    char *name = args[0]->data.string;
    char fn[MAX_STRING];
#ifdef _WIN32
    snprintf(fn, MAX_STRING, "%s.dll", name);
    HMODULE h = LoadLibraryA(fn);
    if (!h) return create_bool(false);
    void (*init)(RegisterFunc) = (void(*)(RegisterFunc))GetProcAddress(h, "init_plugin");
    if (!init) { FreeLibrary(h); return create_bool(false); }
#else
    snprintf(fn, MAX_STRING, "./%s.so", name);
    void *h = dlopen(fn, RTLD_NOW);
    if (!h) return create_bool(false);
    void (*init)(RegisterFunc) = (void(*)(RegisterFunc))dlsym(h, "init_plugin");
    if (!init) { dlclose(h); return create_bool(false); }
#endif
    init(add_native_func);
    loaded_plugins = realloc(loaded_plugins, sizeof(void*) * (loaded_plugin_count + 1));
    loaded_plugins[loaded_plugin_count++] = h;
    return create_bool(true);
}

Value* builtin_pow(Value **args, int count) { if (count < 2) return create_number(0); return create_number(pow(value_to_number(args[0]), value_to_number(args[1]))); }
Value* builtin_min(Value **args, int count) { if (count < 2) return create_number(0); double a = value_to_number(args[0]), b = value_to_number(args[1]); return create_number(a < b ? a : b); }
Value* builtin_max(Value **args, int count) { if (count < 2) return create_number(0); double a = value_to_number(args[0]), b = value_to_number(args[1]); return create_number(a > b ? a : b); }
Value* builtin_abs(Value **args, int count) { if (count < 1) return create_number(0); double v = value_to_number(args[0]); return create_number(v < 0 ? -v : v); }
Value* builtin_join(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_ARRAY || args[1]->type != TYPE_STRING) return create_string(""); Value *a = args[0]; char *d = args[1]->data.string; char buf[MAX_STRING] = ""; for (int i = 0; i < a->data.array.count; i++) { if (i > 0) strcat(buf, d); char *s = value_to_string_raw(a->data.array.items[i]); strcat(buf, s); free(s); } return create_string(buf); }
Value* builtin_trim(Value **args, int count) { 
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(""); 
    char *s = str_dup(args[0]->data.string); 
    char *start = s;
    while (*start == ' ') start++;
    if (*start == '\0') { free(s); return create_string(""); }
    char *end = start + strlen(start) - 1; 
    while (end > start && *end == ' ') end--; 
    *(end+1) = '\0'; 
    Value *v = create_string(start); 
    free(s);
    return v; 
}
Value* builtin_read_json(Value **args, int count) { return builtin_import_json(args, count); }
Value* builtin_write_json(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_STRING) return create_bool(false); return builtin_json_export(args, count); }
Value* builtin_exists(Value **args, int count) { if (count < 1 || args[0]->type != TYPE_STRING) return create_bool(false); FILE *f = fopen(args[0]->data.string, "r"); if (f) { fclose(f); return create_bool(true); } return create_bool(false); }
Value* builtin_contains(Value **args, int count) { if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING) return create_bool(false); return create_bool(strstr(args[0]->data.string, args[1]->data.string) != NULL); }

Value* builtin_array_push(Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_ARRAY) return create_value(TYPE_NULL);
    array_push(args[0], args[1]);
    return args[0];
}

Value* builtin_array_pop(Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_ARRAY || args[0]->data.array.count == 0) 
        return create_value(TYPE_NULL);
    Value *last = args[0]->data.array.items[args[0]->data.array.count - 1];
    args[0]->data.array.count--;
    return last;
}

void add_native_func(const char *name, NativeFunc func) { for (int i = 0; i < func_count; i++) if (strcmp(funcs[i].name, name) == 0) return; if (func_count >= func_capacity) { func_capacity = func_capacity ? func_capacity * 2 : 16; funcs = realloc(funcs, sizeof(Function) * func_capacity); } funcs[func_count].name = str_dup(name); funcs[func_count].params = NULL; funcs[func_count].param_count = 0; funcs[func_count].body_start = 0; funcs[func_count].body_end = 0; funcs[func_count].is_native = true; funcs[func_count].native_func = func; func_count++; }
void register_builtins() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    
    add_native_func("print", builtin_print); add_native_func("type", builtin_type); add_native_func("len", builtin_len);
    add_native_func("input", builtin_input); add_native_func("int", builtin_int); add_native_func("str", builtin_str);
    add_native_func("bool", builtin_bool); add_native_func("range", builtin_range); add_native_func("keys", builtin_keys);
    add_native_func("values", builtin_values); add_native_func("has", builtin_has);
    add_native_func("read", builtin_read_file); add_native_func("write", builtin_write_file);
    add_native_func("json", builtin_json); add_native_func("export", builtin_json_export);
    add_native_func("http_get", builtin_http_get); add_native_func("import_json", builtin_import_json);
    add_native_func("split", builtin_split); add_native_func("upper", builtin_upper);
    add_native_func("lower", builtin_lower); add_native_func("replace", builtin_replace);
    add_native_func("sqrt", builtin_sqrt); add_native_func("random", builtin_random);
    add_native_func("floor", builtin_floor); add_native_func("ceil", builtin_ceil); add_native_func("round", builtin_round);
    add_native_func("now", builtin_now); add_native_func("http_post", builtin_http_post); add_native_func("exec", builtin_exec);
    add_native_func("sleep", builtin_sleep); add_native_func("env", builtin_env);
    add_native_func("red", builtin_red); add_native_func("green", builtin_green); add_native_func("yellow", builtin_yellow);
    add_native_func("blue", builtin_blue); add_native_func("magenta", builtin_magenta); add_native_func("cyan", builtin_cyan);
    add_native_func("bold", builtin_bold); add_native_func("json_parse", builtin_json_parse);
    add_native_func("load_plugin", builtin_load_plugin);
    add_native_func("filter", builtin_filter); add_native_func("map", builtin_map);
    add_native_func("task", builtin_task); add_native_func("watch", builtin_watch);
    add_native_func("error", builtin_error);
    add_native_func("pow", builtin_pow);
    add_native_func("min", builtin_min); add_native_func("max", builtin_max);
    add_native_func("abs", builtin_abs); add_native_func("trim", builtin_trim);
    add_native_func("contains", builtin_contains); add_native_func("join", builtin_join);
    add_native_func("read_json", builtin_read_json);
    add_native_func("write_json", builtin_write_json);
    add_native_func("exists", builtin_exists);
    add_native_func("array_push", builtin_array_push);
    add_native_func("array_pop", builtin_array_pop);
}

// Tokenizer
void tokenize(const char *src) {
    current_line = 1;
    if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) src += 3;
    if (tokens) { for (int i = 0; i < token_count; i++) free(tokens[i]); free(tokens); }
    tokens = NULL; token_count = 0; token_capacity = 256;
    tokens = malloc(sizeof(char*) * token_capacity);
    const char *p = src;
    while (*p) {
        while (*p && isspace(*p)) { if (*p == '\n') current_line++; p++; }
        if (!*p) break;
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) p++; if (*p) p += 2; continue; }
        
        if (*p == '"' || *p == '\'') {
            char q = *p++; char *s = malloc(MAX_STRING); int l = 0;
            while (*p && *p != q && l < MAX_STRING - 1) { 
                if (*p == '\\' && *(p+1)) { 
                    p++; 
                    switch (*p) { 
                        case 'n': s[l++]='\n'; break; 
                        case 't': s[l++]='\t'; break; 
                        case 'r': s[l++]='\r'; break; 
                        case '\\': s[l++]='\\'; break; 
                        case '"': s[l++]='"'; break; 
                        case '\'': s[l++]='\''; break; 
                        default: s[l++]='\\'; s[l++]=*p; 
                    } 
                } else s[l++] = *p; 
                p++; 
            }
            s[l] = '\0'; if (*p == q) p++;
            char *t = malloc(l + 3); t[0] = '"'; strcpy(t + 1, s); t[l + 1] = '"'; t[l + 2] = '\0'; free(s);
            if (token_count >= token_capacity) { token_capacity *= 2; tokens = realloc(tokens, sizeof(char*) * token_capacity); }
            if (token_count >= MAX_TOKENS) { just_error("Too many tokens"); exit(1); }
            tokens[token_count++] = t; continue;
        }
        
        if (p[0]=='=' && p[1]=='=' && p[2]=='=') { 
            char *t = malloc(4); t[0]='='; t[1]='='; t[2]='='; t[3]='\0'; 
            if (token_count >= token_capacity) { token_capacity *= 2; tokens = realloc(tokens, sizeof(char*) * token_capacity); }
            if (token_count >= MAX_TOKENS) { just_error("Too many tokens"); exit(1); }
            tokens[token_count++] = t; p += 3; continue; 
        }
        if (p[0]=='!' && p[1]=='=' && p[2]=='=') { 
            char *t = malloc(4); t[0]='!'; t[1]='='; t[2]='='; t[3]='\0'; 
            if (token_count >= token_capacity) { token_capacity *= 2; tokens = realloc(tokens, sizeof(char*) * token_capacity); }
            if (token_count >= MAX_TOKENS) { just_error("Too many tokens"); exit(1); }
            tokens[token_count++] = t; p += 3; continue; 
        }
        
        if (p[0]=='*' && p[1]=='*') { 
            char *t = malloc(3); t[0]='*'; t[1]='*'; t[2]='\0'; 
            if (token_count >= token_capacity) { token_capacity *= 2; tokens = realloc(tokens, sizeof(char*) * token_capacity); }
            if (token_count >= MAX_TOKENS) { just_error("Too many tokens"); exit(1); }
            tokens[token_count++] = t; p += 2; continue; 
        }
        
        if ((p[0]=='='&&p[1]=='=')||(p[0]=='!'&&p[1]=='=')||(p[0]=='<'&&p[1]=='=')||(p[0]=='>'&&p[1]=='=')||(p[0]=='+'&&p[1]=='=')||(p[0]=='-'&&p[1]=='=')) {
            char *t = malloc(3); t[0]=p[0]; t[1]=p[1]; t[2]='\0';
            if (token_count >= token_capacity) { token_capacity *= 2; tokens = realloc(tokens, sizeof(char*) * token_capacity); }
            if (token_count >= MAX_TOKENS) { just_error("Too many tokens"); exit(1); }
            tokens[token_count++] = t; p += 2; continue;
        }
        
        if (strchr("(){}[]:,;=+*/<>.!@#$%^&|~?-", *p)) {
            char *t = malloc(2); t[0]=*p; t[1]='\0';
            if (token_count >= token_capacity) { token_capacity *= 2; tokens = realloc(tokens, sizeof(char*) * token_capacity); }
            if (token_count >= MAX_TOKENS) { just_error("Too many tokens"); exit(1); }
            tokens[token_count++] = t; p++; continue;
        }
        
        if (isalpha(*p) || *p == '_' || isdigit(*p)) {
            const char *st = p;
            if (isdigit(*p)) { 
                while (*p && isdigit(*p)) p++; 
                if (*p == '.' && isdigit(*(p+1))) { p++; while (*p && isdigit(*p)) p++; } 
            }
            else while (*p && (isalnum(*p) || *p == '_')) p++;
            int l = p - st; char *t = malloc(l + 1); strncpy(t, st, l); t[l] = '\0';
            if (token_count >= token_capacity) { token_capacity *= 2; tokens = realloc(tokens, sizeof(char*) * token_capacity); }
            if (token_count >= MAX_TOKENS) { just_error("Too many tokens"); exit(1); }
            tokens[token_count++] = t; continue;
        }
        p++;
    }
}

// Expression parser
Value* eval_primary(int *pos) { 
    if (*pos >= token_count) return create_value(TYPE_NULL); 
    char *t = tokens[*pos]; 
    
    if (strcmp(t, "null") == 0) { (*pos)++; return create_value(TYPE_NULL); } 
    if (strcmp(t, "true") == 0) { (*pos)++; return create_bool(true); } 
    if (strcmp(t, "false") == 0) { (*pos)++; return create_bool(false); } 
    
    if (t[0] == '"' || t[0] == '\'') { 
        (*pos)++; 
        int l = strlen(t); 
        char *s = malloc(l); 
        int j = 0; 
        for (int i = 1; i < l - 1; i++) { 
            if (t[i] == '\\' && i + 1 < l - 1) { 
                i++; 
                switch (t[i]) { 
                    case 'n': s[j++]='\n'; break; 
                    case 't': s[j++]='\t'; break; 
                    case 'r': s[j++]='\r'; break; 
                    case '\\': s[j++]='\\'; break; 
                    case '"': s[j++]='"'; break; 
                    default: s[j++]='\\'; s[j++]=t[i]; 
                } 
            } else s[j++] = t[i]; 
        } 
        s[j] = '\0'; 
        Value *v = create_string(s); 
        free(s); 
        return v; 
    } 
    
    if (isdigit(t[0])) { 
        (*pos)++; 
        return create_number(atof(t)); 
    } 
    
    if (strcmp(t, "-") == 0) {
        (*pos)++;
        Value *v = eval_primary(pos);
        if (v->type == TYPE_NUMBER) {
            v->data.number = -v->data.number;
            return v;
        }
        Value *r = create_number(-value_to_number(v));
        return r;
    }
    
    if (strcmp(t, "[") == 0) { 
        (*pos)++; 
        Value *a = create_array(); 
        while (*pos < token_count && strcmp(tokens[*pos], "]") != 0) { 
            if (a->data.array.count > 0 && strcmp(tokens[*pos], ",") == 0) (*pos)++; 
            if (*pos < token_count && strcmp(tokens[*pos], "]") != 0) { 
                Value *i = eval_expression(pos); 
                array_push(a, i); 
            } 
        } 
        if (*pos < token_count) (*pos)++; 
        return a; 
    } 
    
    if (strcmp(t, "{") == 0) { 
        (*pos)++; 
        Value *o = create_object(); 
        while (*pos < token_count && strcmp(tokens[*pos], "}") != 0) { 
            if (o->data.object.count > 0 && strcmp(tokens[*pos], ",") == 0) (*pos)++; 
            if (*pos >= token_count || strcmp(tokens[*pos], "}") == 0) break; 
            char *k = tokens[(*pos)++]; 
            if (*pos < token_count && strcmp(tokens[*pos], ":") == 0) (*pos)++; 
            if (*pos >= token_count) break; 
            Value *v = eval_expression(pos); 
            object_set(o, k, v); 
        } 
        if (*pos < token_count) (*pos)++; 
        return o; 
    } 
    
    if (strcmp(t, "(") == 0) { 
        (*pos)++; 
        Value *v = eval_expression(pos); 
        if (*pos < token_count && strcmp(tokens[*pos], ")") == 0) (*pos)++; 
        return v; 
    } 
    
    if (strcmp(t, "!") == 0 || strcmp(t, "not") == 0) { 
        (*pos)++; 
        Value *v = eval_primary(pos); 
        bool b = value_to_bool(v);  
        return create_bool(!b); 
    } 
    
    if (isalpha(t[0]) || t[0] == '_') { 
        if (*pos + 1 < token_count && strcmp(tokens[*pos + 1], "(") == 0) { 
            char *fn = t; 
            *pos += 2; 
            Value *args[MAX_ARGS]; 
            int ac = 0; 
            while (*pos < token_count && strcmp(tokens[*pos], ")") != 0) { 
                if (ac > 0 && strcmp(tokens[*pos], ",") == 0) (*pos)++; 
                if (*pos >= token_count || strcmp(tokens[*pos], ")") == 0) break; 
                args[ac++] = eval_expression(pos); 
            } 
            if (*pos < token_count) (*pos)++; 
            Function *f = find_func(fn); 
            if (f) { 
                if (f->is_native) { 
                    Value *r = f->native_func(args, ac); 
                    return r; 
                } 
                push_scope(); 
                for (int i = 0; i < f->param_count && i < ac; i++) set_var(f->params[i], args[i], false); 
                int od = scope_depth; 
                ControlFlow of = current_flow; 
                current_flow = FLOW_NORMAL; 
                return_value = NULL; 
                execute_block(f->body_start, f->body_end); 
                current_flow = of; 
                while (scope_depth > od) pop_scope(); 
                Value *r = return_value ? return_value : create_value(TYPE_NULL); 
                if (return_value) { return_value = NULL; } 
                return r; 
            } 
            return create_value(TYPE_NULL); 
        } 
        (*pos)++; 
        Variable *v = find_var(t); 
        if (v) return v->value; 
        return create_value(TYPE_NULL); 
    } 
    (*pos)++; 
    return create_value(TYPE_NULL); 
}
Value* eval_postfix(int *pos) { Value *v = eval_primary(pos); while (*pos < token_count) { if (strcmp(tokens[*pos], ".") == 0) { (*pos)++; if (*pos < token_count) { char *f = tokens[*pos]; (*pos)++; if (v->type == TYPE_OBJECT) { Value *fv = object_get(v, f); v = fv ? fv : create_value(TYPE_NULL); } else { v = create_value(TYPE_NULL); } } continue; } if (strcmp(tokens[*pos], "[") == 0) { (*pos)++; Value *idx = eval_expression(pos); if (*pos < token_count && strcmp(tokens[*pos], "]") == 0) (*pos)++; if (v->type == TYPE_ARRAY) { Value *item = array_get(v, (int)value_to_number(idx)); v = item; } else { v = create_value(TYPE_NULL); } continue; } break; } return v; }
Value* eval_multiplicative(int *pos) { Value *l = eval_postfix(pos); while (*pos < token_count) { char *op = tokens[*pos]; if (strcmp(op,"*") && strcmp(op,"/") && strcmp(op,"%")) break; (*pos)++; Value *r = eval_postfix(pos); double a = value_to_number(l), b = value_to_number(r), res = 0; if (strcmp(op,"*")==0) res=a*b; else if (strcmp(op,"/")==0) res=b!=0?a/b:0; else res=b!=0?fmod(a,b):0; l = create_number(res); } return l; }
Value* eval_additive(int *pos) { Value *l = eval_multiplicative(pos); while (*pos < token_count) { char *op = tokens[*pos]; if (strcmp(op,"+") && strcmp(op,"-")) break; (*pos)++; Value *r = eval_multiplicative(pos); if (l->type == TYPE_STRING || r->type == TYPE_STRING) { char *ls = value_to_string_raw(l), *rs = value_to_string_raw(r); char *res = malloc(strlen(ls)+strlen(rs)+1); strcpy(res, ls); if (strcmp(op,"+")==0) strcat(res, rs); free(ls); free(rs); l = create_string(res); free(res); } else { double a = value_to_number(l), b = value_to_number(r); l = create_number(strcmp(op,"+")==0?a+b:a-b); } } return l; }
Value* eval_comparison(int *pos) {
    Value *l = eval_additive(pos);
    while (*pos < token_count) {
        char *op = tokens[*pos];
        if (strcmp(op,"==") && strcmp(op,"!=") && strcmp(op,"<") && strcmp(op,">") && strcmp(op,"<=") && strcmp(op,">=") && strcmp(op,"===") && strcmp(op,"!==")) break;
        (*pos)++;
        Value *r = eval_additive(pos);
        bool res = false;
        if (strcmp(op,"===")==0) {
            res = (l->type == r->type && value_to_number(l) == value_to_number(r));
        } else if (strcmp(op,"!==")==0) {
            res = !(l->type == r->type && value_to_number(l) == value_to_number(r));
        } else {
            double a = value_to_number(l), b = value_to_number(r);
            if (strcmp(op,"==")==0) res=a==b;
            else if (strcmp(op,"!=")==0) res=a!=b;
            else if (strcmp(op,"<")==0) res=a<b;
            else if (strcmp(op,">")==0) res=a>b;
            else if (strcmp(op,"<=")==0) res=a<=b;
            else res=a>=b;
        }
        l = create_bool(res);
    }
    return l;
}
Value* eval_logical_and(int *pos) { Value *l = eval_comparison(pos); while (*pos < token_count && strcmp(tokens[*pos],"and")==0) { (*pos)++; Value *r = eval_comparison(pos); bool b = value_to_bool(l) && value_to_bool(r); l = create_bool(b); } return l; }
Value* eval_logical_or(int *pos) { Value *l = eval_logical_and(pos); while (*pos < token_count && strcmp(tokens[*pos],"or")==0) { (*pos)++; Value *r = eval_logical_and(pos); bool b = value_to_bool(l) || value_to_bool(r); l = create_bool(b); } return l; }
Value* eval_expression(int *pos) { return eval_logical_or(pos); }

// Executor
int execute_block(int start, int end) { int pos = start, od = scope_depth; while (pos < end && pos < token_count && current_flow == FLOW_NORMAL) pos = execute_statement(pos); while (scope_depth > od) pop_scope(); return pos; }
int execute_statement(int pos) { 
    if (pos >= token_count) return pos; 
    char *cmd = tokens[pos]; 
    
    if (strcmp(cmd, ";") == 0) return pos + 1; 
    
    if (strcmp(cmd, "return") == 0) { 
        pos++; 
        if (pos < token_count && strcmp(tokens[pos], ";") != 0) { 
            return_value = eval_expression(&pos); 
        } 
        current_flow = FLOW_RETURN; 
        return pos; 
    } 
    
    if (strcmp(cmd, "break") == 0) { 
        current_flow = FLOW_BREAK; 
        return pos + 1; 
    } 
    
    if (strcmp(cmd, "continue") == 0) { 
        current_flow = FLOW_CONTINUE; 
        return pos + 1; 
    } 
    
    if (strcmp(cmd, "import") == 0) {
        pos++; 
        if (pos >= token_count) return pos;
        char *fn = tokens[pos++];
        if (fn[0] == '"' || fn[0] == '\'') { 
            int l = strlen(fn); 
            fn[l-1] = '\0'; 
            fn++; 
        }
        
        int saved_line = current_line;
        ControlFlow saved_flow = current_flow;
        
        static char *imported_files[100] = {NULL};
        static int import_count = 0;
        for (int i = 0; i < import_count; i++) {
            if (strcmp(imported_files[i], fn) == 0) return pos;
        }
        if (import_count < 100) imported_files[import_count++] = str_dup(fn);
        
        FILE *f = fopen(fn, "r");
        if (!f) { 
            just_error("Cannot import file"); 
            return pos; 
        }
        fseek(f, 0, SEEK_END); 
        long sz = ftell(f); 
        fseek(f, 0, SEEK_SET);
        char *code = malloc(sz + 1); 
        fread(code, 1, sz, f); 
        code[sz] = '\0'; 
        fclose(f);
        
        char **saved_tokens = tokens;
        int saved_count = token_count;
        int saved_cap = token_capacity;
        tokens = NULL; 
        token_count = 0; 
        token_capacity = 0;
        
        tokenize(code);
        current_flow = FLOW_NORMAL; 
        return_value = NULL;
        execute_block(0, token_count);
        
        current_line = saved_line;
        current_flow = saved_flow;
        
        free(code);
        
        for (int i = 0; i < token_count; i++) free(tokens[i]); 
        free(tokens);
        tokens = saved_tokens;
        token_count = saved_count;
        token_capacity = saved_cap;
        
        return pos;
    } 
    
    if (strcmp(cmd, "const") == 0 || strcmp(cmd, "let") == 0) { 
        bool ic = (strcmp(cmd, "const") == 0); 
        pos++; 
        if (pos >= token_count) return pos; 
        char *vn = tokens[pos++]; 
        if (pos < token_count && strcmp(tokens[pos], "=") == 0) { 
            pos++; 
            Value *v = eval_expression(&pos); 
            set_var(vn, v, ic); 
            return pos; 
        } 
        return pos; 
    } 
    
    if (strcmp(cmd, "if") == 0) { 
        pos++; 
        Value *c = eval_expression(&pos); 
        bool cond = value_to_bool(c); 
        while (pos < token_count && strcmp(tokens[pos], "{") != 0) pos++; 
        if (pos >= token_count) return pos; 
        pos++; 
        int bs = pos, d = 1; 
        while (pos < token_count && d > 0) { 
            if (strcmp(tokens[pos], "{") == 0) d++; 
            else if (strcmp(tokens[pos], "}") == 0) d--; 
            pos++; 
        } 
        int be = pos - 1; 
        bool eh = false; 
        ControlFlow sf = current_flow; 
        if (cond) { 
            push_scope(); 
            current_flow = FLOW_NORMAL; 
            execute_block(bs, be); 
            if (current_flow == FLOW_NORMAL) current_flow = sf; 
            if (scope_depth > 0) pop_scope(); 
            eh = true; 
        } 
        while (pos < token_count && strcmp(tokens[pos], "else") == 0) { 
            pos++; 
            if (pos < token_count && strcmp(tokens[pos], "if") == 0) { 
                pos++; 
                Value *c2 = eval_expression(&pos); 
                bool c2b = value_to_bool(c2); 
                while (pos < token_count && strcmp(tokens[pos], "{") != 0) pos++; 
                if (pos >= token_count) return pos; 
                pos++; 
                int b2s = pos, d2 = 1; 
                while (pos < token_count && d2 > 0) { 
                    if (strcmp(tokens[pos], "{") == 0) d2++; 
                    else if (strcmp(tokens[pos], "}") == 0) d2--; 
                    pos++; 
                } 
                int b2e = pos - 1; 
                if (!eh && c2b) { 
                    push_scope(); 
                    current_flow = FLOW_NORMAL; 
                    execute_block(b2s, b2e); 
                    if (current_flow == FLOW_NORMAL) current_flow = sf; 
                    if (scope_depth > 0) pop_scope(); 
                    eh = true; 
                } 
                if (c2b) eh = true; 
            } else if (pos < token_count && strcmp(tokens[pos], "{") == 0) { 
                pos++; 
                int es = pos, d2 = 1; 
                while (pos < token_count && d2 > 0) { 
                    if (strcmp(tokens[pos], "{") == 0) d2++; 
                    else if (strcmp(tokens[pos], "}") == 0) d2--; 
                    pos++; 
                } 
                int ee = pos - 1; 
                if (!eh) { 
                    push_scope(); 
                    current_flow = FLOW_NORMAL; 
                    execute_block(es, ee); 
                    if (current_flow == FLOW_NORMAL) current_flow = sf; 
                    if (scope_depth > 0) pop_scope(); 
                } 
                break; 
            } 
        } 
        return pos; 
    } 
    
    if (strcmp(cmd, "while") == 0) { 
        pos++; 
        int cp = pos; 
        while (pos < token_count && strcmp(tokens[pos], "{") != 0) pos++; 
        if (pos >= token_count) return pos; 
        pos++; 
        int bs = pos, d = 1; 
        while (pos < token_count && d > 0) { 
            if (strcmp(tokens[pos], "{") == 0) d++; 
            else if (strcmp(tokens[pos], "}") == 0) d--; 
            pos++; 
        } 
        int be = pos - 1; 
        for (int lc = 0; lc < MAX_ITERATIONS; lc++) { 
            int chp = cp; 
            Value *c = eval_expression(&chp); 
            bool cond = value_to_bool(c); 
            if (!cond) break; 
            ControlFlow of = current_flow; 
            current_flow = FLOW_NORMAL; 
            execute_block(bs, be); 
            if (current_flow == FLOW_BREAK) { 
                current_flow = of; 
                break; 
            } 
            if (current_flow == FLOW_CONTINUE) { 
                current_flow = FLOW_NORMAL; 
                continue; 
            } 
            current_flow = of; 
        } 
        return pos; 
    } 
    
    if (strcmp(cmd, "for") == 0) { 
        pos++; 
        if (pos + 4 >= token_count) return pos; 
        if (strcmp(tokens[pos], "(") == 0) pos++; 
        int is = pos; 
        while (pos < token_count && strcmp(tokens[pos], ";") != 0) pos++; 
        if (pos >= token_count) return pos; 
        int ie = pos; 
        pos++; 
        int cp = pos; 
        while (pos < token_count && strcmp(tokens[pos], ";") != 0) pos++; 
        if (pos >= token_count) return pos; 
        int ce = pos; 
        pos++; 
        int xs = pos; 
        while (pos < token_count && strcmp(tokens[pos], ")") != 0) pos++; 
        if (pos >= token_count) return pos; 
        int xe = pos; 
        pos++; 
        if (strcmp(tokens[pos], "{") != 0) return pos; 
        pos++; 
        int bs = pos, d = 1; 
        while (pos < token_count && d > 0) { 
            if (strcmp(tokens[pos], "{") == 0) d++; 
            else if (strcmp(tokens[pos], "}") == 0) d--; 
            pos++; 
        } 
        int be = pos - 1; 
        push_scope(); 
        ControlFlow fi = current_flow; 
        current_flow = FLOW_NORMAL; 
        execute_block(is, ie); 
        current_flow = fi; 
        for (int lc = 0; lc < MAX_ITERATIONS; lc++) { 
            int chp = cp; 
            Value *c = eval_expression(&chp); 
            bool cond = value_to_bool(c); 
            if (!cond) break; 
            ControlFlow of = current_flow; 
            current_flow = FLOW_NORMAL; 
            execute_block(bs, be); 
            if (current_flow == FLOW_BREAK) { 
                current_flow = of; 
                break; 
            } 
            if (current_flow == FLOW_CONTINUE) { 
                current_flow = FLOW_NORMAL; 
                ControlFlow fi2 = current_flow; 
                current_flow = FLOW_NORMAL; 
                execute_block(xs, xe); 
                current_flow = fi2; 
                continue; 
            } 
            current_flow = of; 
            ControlFlow fi2 = current_flow; 
            current_flow = FLOW_NORMAL; 
            execute_block(xs, xe); 
            current_flow = fi2; 
        } 
        if (scope_depth > 0) pop_scope(); 
        return pos; 
    } 
    
    if (strcmp(cmd, "func") == 0) { 
        pos++; 
        if (pos >= token_count) return pos; 
        char *fn = tokens[pos++]; 
        char *params[MAX_ARGS]; 
        int pc = 0; 
        if (pos < token_count && strcmp(tokens[pos], "(") == 0) { 
            pos++; 
            while (pos < token_count && strcmp(tokens[pos], ")") != 0) { 
                if (pc > 0 && strcmp(tokens[pos], ",") == 0) pos++; 
                if (pos < token_count && strcmp(tokens[pos], ")") != 0) 
                    params[pc++] = tokens[pos++]; 
            } 
            if (pos < token_count) pos++; 
        } 
        while (pos < token_count && strcmp(tokens[pos], "{") != 0) pos++; 
        if (pos >= token_count) return pos; 
        pos++; 
        int bs = pos, d = 1; 
        while (pos < token_count && d > 0) { 
            if (strcmp(tokens[pos], "{") == 0) d++; 
            else if (strcmp(tokens[pos], "}") == 0) d--; 
            pos++; 
        } 
        int be = pos - 1; 
        add_func(fn, params, pc, bs, be); 
        return pos; 
    } 
    
    if (isalpha(cmd[0]) || cmd[0] == '_') { 
        Variable *var = find_var(cmd);
        
        if (var && var->value && var->value->type == TYPE_ARRAY && 
            pos + 1 < token_count && strcmp(tokens[pos + 1], "[") == 0) {
            pos += 2;
            Value *idx_val = eval_expression(&pos);
            int idx = (int)value_to_number(idx_val);
            
            if (pos < token_count && strcmp(tokens[pos], "]") == 0) {
                pos++;
                
                if (pos < token_count && strcmp(tokens[pos], ".") == 0) {
                    pos++;
                    char *prop = tokens[pos++];
                    
                    if (pos < token_count && strcmp(tokens[pos], "=") == 0) {
                        pos++;
                        Value *v = eval_expression(&pos);
                        Value *item = array_get(var->value, idx);
                        if (item && item->type == TYPE_OBJECT) {
                            object_set(item, prop, v);
                        }
                        return pos;
                    }
                    else if (pos < token_count && strcmp(tokens[pos], "+=") == 0) {
                        pos++;
                        Value *v = eval_expression(&pos);
                        Value *item = array_get(var->value, idx);
                        if (item && item->type == TYPE_OBJECT) {
                            Value *cur = object_get(item, prop);
                            Value *nv = create_number(value_to_number(cur) + value_to_number(v));
                            object_set(item, prop, nv);
                        }
                        return pos;
                    }
                    else if (pos < token_count && strcmp(tokens[pos], "-=") == 0) {
                        pos++;
                        Value *v = eval_expression(&pos);
                        Value *item = array_get(var->value, idx);
                        if (item && item->type == TYPE_OBJECT) {
                            Value *cur = object_get(item, prop);
                            Value *nv = create_number(value_to_number(cur) - value_to_number(v));
                            object_set(item, prop, nv);
                        }
                        return pos;
                    }
                }
                else if (pos < token_count && strcmp(tokens[pos], "=") == 0) {
                    pos++;
                    Value *v = eval_expression(&pos);
                    array_set(var->value, idx, v);
                    return pos;
                }
            }
            return pos;
        }
        
        if (var && var->value && var->value->type == TYPE_OBJECT && 
            pos + 1 < token_count && strcmp(tokens[pos + 1], ".") == 0) {
            pos += 2;
            char *f = tokens[pos++];
            if (pos < token_count) {
                if (strcmp(tokens[pos], "=") == 0) {
                    pos++;
                    Value *v = eval_expression(&pos);
                    object_set(var->value, f, v);
                    return pos;
                } else if (strcmp(tokens[pos], "+=") == 0) {
                    pos++;
                    Value *v = eval_expression(&pos);
                    Value *cur = object_get(var->value, f);
                    Value *nv = create_number(value_to_number(cur) + value_to_number(v));
                    object_set(var->value, f, nv);
                    return pos;
                } else if (strcmp(tokens[pos], "-=") == 0) {
                    pos++;
                    Value *v = eval_expression(&pos);
                    Value *cur = object_get(var->value, f);
                    Value *nv = create_number(value_to_number(cur) - value_to_number(v));
                    object_set(var->value, f, nv);
                    return pos;
                }
            }
            return pos;
        }
        
        if (pos + 1 < token_count && strcmp(tokens[pos + 1], "=") == 0) {
            pos += 2;
            Value *v = eval_expression(&pos);
            set_var(cmd, v, false);
            return pos;
        }
        
        if (pos + 1 < token_count && (strcmp(tokens[pos + 1], "+=") == 0 || strcmp(tokens[pos + 1], "-=") == 0)) {
            char *op = tokens[pos + 1];
            pos += 2;
            Value *v = eval_expression(&pos);
            double cur = (var && var->value) ? value_to_number(var->value) : 0;
            double r = (strcmp(op, "+=") == 0) ? cur + value_to_number(v) : cur - value_to_number(v);
            Value *nv = create_number(r);
            set_var(cmd, nv, false);
            return pos;
        }
        
        Function *f = find_func(cmd);
        if (f && pos + 1 < token_count && strcmp(tokens[pos + 1], "(") == 0) {
            pos += 2;
            Value *args[MAX_ARGS];
            int ac = 0;
            while (pos < token_count && strcmp(tokens[pos], ")") != 0) {
                if (ac > 0 && strcmp(tokens[pos], ",") == 0) pos++;
                if (pos >= token_count || strcmp(tokens[pos], ")") == 0) break;
                args[ac++] = eval_expression(&pos);
            }
            if (pos < token_count) pos++;
            if (f->is_native) {
                Value *r = f->native_func(args, ac);
            } else {
                push_scope();
                for (int i = 0; i < f->param_count && i < ac; i++) 
                    set_var(f->params[i], args[i], false);
                int od = scope_depth;
                ControlFlow of = current_flow;
                current_flow = FLOW_NORMAL;
                return_value = NULL;
                execute_block(f->body_start, f->body_end);
                current_flow = of;
                while (scope_depth > od) pop_scope();
                Value *r = return_value ? return_value : create_value(TYPE_NULL);
                if (return_value) { return_value = NULL; }
            }
            return pos;
        }
    }
    pos++;
    return pos;
}

void run_code(const char *src) { current_line = 1; tokenize(src); push_scope(); register_builtins(); current_flow = FLOW_NORMAL; return_value = NULL; execute_block(0, token_count); while (scope_depth > 0) pop_scope(); for (int i = 0; i < token_count; i++) free(tokens[i]); free(tokens); tokens = NULL; token_count = 0; gc_collect(); }

void shutdown_cleanup() {
    for (int i = 0; i < func_count; i++) { free(funcs[i].name); if (!funcs[i].is_native) { for (int j = 0; j < funcs[i].param_count; j++) free(funcs[i].params[j]); free(funcs[i].params); } }
    free(funcs); 
#ifdef _WIN32
    if (winsock_initialized) WSACleanup();
#endif
    for (int i = 0; i < loaded_plugin_count; i++) {
#ifdef _WIN32
        FreeLibrary((HMODULE)loaded_plugins[i]);
#else
        dlclose(loaded_plugins[i]);
#endif
    }
    for (int i = 0; i < task_count; i++) free(tasks[i].name);
    free(tasks); free(loaded_plugins);
    GCNode *node = gc_head;
    while (node) { GCNode *next = node->next; free(node); node = next; }
    gc_head = gc_tail = NULL; gc_count = 0;
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) { dwMode |= 0x0004; if (SetConsoleMode(hOut, dwMode)) win10_ansi_supported = true; }
#endif
    srand((unsigned)time(NULL));
    if (argc < 2) {
        printf("Just Language v2.0 REPL (type 'exit' to quit)\n");
        char *line = malloc(4096);
        while (1) { printf("> "); if (!fgets(line, 4096, stdin)) break; line[strcspn(line, "\n")] = 0; if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break; if (strlen(line) > 0) run_code(line); }
        free(line); printf("Goodbye!\n"); return 0;
    }
    if (strcmp(argv[1], "run") == 0) { if (argc < 3) { fprintf(stderr, "Usage: just run \"code\"\n"); return 1; } run_code(argv[2]); shutdown_cleanup(); return 0; }
    if (strcmp(argv[1], "-c") == 0) { if (argc < 3) { fprintf(stderr, "Error: No code\n"); return 1; } run_code(argv[2]); shutdown_cleanup(); return 0; }
    if (strcmp(argv[1], "init") == 0) { char *name = argc > 2 ? argv[2] : "myproject"; char cmd[MAX_STRING]; snprintf(cmd, MAX_STRING, "mkdir %s && echo 'print(\"Hello from %s!\")' > %s/main.just", name, name, name); system(cmd); printf("Created project: %s/\n  main.just\n", name); return 0; }
    FILE *f = fopen(argv[1], "r"); if (!f) { fprintf(stderr, "Error: Cannot open '%s'\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *code = malloc(sz + 1); fread(code, 1, sz, f); code[sz] = '\0'; fclose(f);
    if (strncmp(code, "#!/", 3) == 0) { char *p = strchr(code, '\n'); if (p) memmove(code, p + 1, strlen(p + 1) + 1); }
    run_code(code); free(code); shutdown_cleanup(); return 0;
}
