#include "just.h"
#include <stdatomic.h>

#include "re.c"

#ifndef JUST_NO_TLS
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#endif

#ifdef _WIN32
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#include <windows.h>
#include <wincrypt.h>

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    HCRYPTPROV hProv;
    (void)data;
    
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return -1;
    }
    
    if (!CryptGenRandom(hProv, (DWORD)len, output)) {
        CryptReleaseContext(hProv, 0);
        return -1;
    }
    
    CryptReleaseContext(hProv, 0);
    *olen = len;
    return 0;
}
#endif

#define SQLITE_OMIT_LOAD_EXTENSION
#define SQLITE_THREADSAFE 0
#define SQLITE_OMIT_DEPRECATED
#ifndef JUST_NO_SQLITE
#include "sqlite3.c"
#endif

typedef struct {
    JustState *j;
    Value *arr;
} DbCallbackCtx;

static void add_native_func(JustState *j, const char *name, NativeFunc func);

static JustState* plugin_state = NULL;

static atomic_flag plugin_lock = ATOMIC_FLAG_INIT;
static void plugin_lock_acquire(void) {
    while (atomic_flag_test_and_set_explicit(&plugin_lock, memory_order_acquire)) {

    }
}
static void plugin_lock_release(void) {
    atomic_flag_clear_explicit(&plugin_lock, memory_order_release);
}

static void plugin_adapter(const char *name, NativeFunc func) {
    if (plugin_state) {
        add_native_func(plugin_state, name, func);
    }
}

char* str_dup(const char *s) {
    if (!s) return NULL;
    char *d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

void just_error(JustState *j, const char *msg) {

    if (!msg) msg = "(no message)";
    if (!j) {
        fprintf(stderr, "\nERROR: %s\n", msg);
        return;
    }
    fprintf(stderr, "\nERROR (line %d): %s\n", j->current_line, msg);

    if (j->call_depth > 0) {
        fprintf(stderr, "  Call stack (innermost first):\n");
        int shown = 0;
        for (int s = j->call_depth - 1; s >= 0 && shown < 15; s--, shown++)
            fprintf(stderr, "    at %s()\n", j->call_stack[s] ? j->call_stack[s] : "?");
        if (j->call_depth - shown > 0)
            fprintf(stderr, "    ... (%d more frame%s omitted)\n", j->call_depth - shown, (j->call_depth - shown) == 1 ? "" : "s");
    }
}

#ifdef _WIN32
void just_sleep_ms(int ms) { Sleep(ms); }
#else
void just_sleep_ms(int ms) { usleep(ms * 1000); }
#endif

static void gc_add_node(JustState *j, Value *v);
static void adopt_into_gc(JustState *j, Value *v);
static void gc_mark_value(JustState *j, Value *v);
static void gc_mark_roots(JustState *j);
static void gc_sweep(JustState *j);
static void gc_collect(JustState *j);
static Value* create_value(JustState *j, ValueType type);
static Value* create_number(JustState *j, double n);
static Value* create_string(JustState *j, const char *s);
static Value* create_bool(JustState *j, bool b);
static Value* create_object(JustState *j);
static Value* capture_scope(JustState *j);
static Value* create_array(JustState *j);
static Value* clone_value_internal(JustState *j, Value *v, int depth);
static Value* clone_value(JustState *j, Value *v);
static void object_set(Value *obj, const char *key, Value *val);
static Value* object_get(Value *obj, const char *key);
static bool object_has(Value *obj, const char *key);
static void array_push(Value *arr, Value *val);
static Value* array_get(Value *arr, int index);
static int normalize_index(int index, int len);
#ifndef JUST_NO_SQLITE
static int handle_alloc(JustState *j, void *ptr);
static void* handle_get(JustState *j, int id);
static void handle_free(JustState *j, int id);
#endif
static uint32_t xorshift32(uint32_t *state);
static void array_set(Value *arr, int index, Value *val);
static double value_to_number(Value *v);
static bool values_equal(Value *l, Value *r);
static bool value_to_bool(Value *v);
static char* value_to_string_raw(Value *v);
static Value* value_to_string(JustState *j, Value *v);
static Value* value_to_json(JustState *j, Value *v);
static Value* json_parse_value(JustState *j, const char **p);
static void push_scope(JustState *j);
static void pop_scope(JustState *j);
static Variable* find_var(JustState *j, const char *name);
static void set_var(JustState *j, const char *name, Value *value, bool constant);
static void declare_var(JustState *j, const char *name, Value *value, bool constant);
static void add_func(JustState *j, const char *name, char **params, int param_count, int start, int end);
static Function* find_func(JustState *j, const char *name);
static void register_builtins(JustState *j);
static void tokenize(JustState *j, const char *src);
static Value* eval_expression(JustState *j, int *pos);
static int execute_statement(JustState *j, int pos);
static int execute_block(JustState *j, int start, int end);
static void run_code(JustState *j, const char *src);
static void shutdown_cleanup(JustState *j);
static void add_native_func(JustState *j, const char *name, NativeFunc func);

void gc_add_node(JustState *j, Value *v) {
    if (!v) return;
    GCNode *node = malloc(sizeof(GCNode));
    if (!node) { just_error(j, "GC: Out of memory"); exit(1); }
    node->value = v;
    node->next = NULL;
    if (j->gc_tail) {
        j->gc_tail->next = node;
        j->gc_tail = node;
    } else {
        j->gc_head = j->gc_tail = node;
    }
    v->gc_node = node;
    j->gc_count++;
    j->total_allocations++;
}

void adopt_into_gc(JustState *j, Value *v) {
    if (!v) return;
    if (!v->gc_node) gc_add_node(j, v);

    switch (v->type) {
        case TYPE_OBJECT:
            for (int i = 0; i < v->data.object.count; i++) {
                Value *child = v->data.object.values[i];
                if (child && !child->gc_node) adopt_into_gc(j, child);
            }
            break;
        case TYPE_ARRAY:
            for (int i = 0; i < v->data.array.count; i++) {
                Value *child = v->data.array.items[i];
                if (child && !child->gc_node) adopt_into_gc(j, child);
            }
            break;
        default: break;
    }
}

void gc_mark_value(JustState *j, Value *v) {
    (void)j;
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
            case TYPE_FUNCTION:
                if (cur->is_lambda && cur->data.lambda.captured) {
                    Value *child = cur->data.lambda.captured;
                    if (!child->marked) {
                        child->marked = true;
                        if (sp < 10000) stack[sp++] = child;
                    }
                }
                break;
            default: break;
        }
    }
}

void gc_mark_roots(JustState *j) {
    for (int s = 0; s < j->scope_depth; s++)
        for (int i = 0; i < j->scopes[s]->var_count; i++)
            gc_mark_value(j, j->scopes[s]->vars[i].value);
    if (j->return_value) gc_mark_value(j, j->return_value);

    if (j->error_value) gc_mark_value(j, j->error_value);
}

void gc_sweep(JustState *j) {
    GCNode *prev = NULL, *curr = j->gc_head;
    while (curr) {

        if (curr->value && !curr->value->marked) {
            Value *v = curr->value;
            switch (v->type) {
                case TYPE_STRING: free(v->data.string); break;
                case TYPE_FUNCTION:
                    if (v->is_lambda) {
                        for (int i = 0; i < v->data.lambda.param_count; i++) free(v->data.lambda.params[i]);
                        free(v->data.lambda.params);

                    } else {
                        free(v->data.string);
                    }
                    break;
                case TYPE_OBJECT:
                    for (int i = 0; i < v->data.object.count; i++) free(v->data.object.keys[i]);
                    free(v->data.object.keys); free(v->data.object.values); break;
                case TYPE_ARRAY: free(v->data.array.items); break;
                default: break;
            }
            v->gc_node = NULL;
            free(v);
            curr->value = NULL;
            j->gc_count--;
        }
        GCNode *next = curr->next;
        if (!curr->value) {
            if (prev) prev->next = next;
            else j->gc_head = next;
            if (curr == j->gc_tail) j->gc_tail = prev;
            free(curr);
        } else prev = curr;
        curr = next;
    }
}

void gc_collect(JustState *j) {
    GCNode *node = j->gc_head;
    while (node) {
        if (node->value) node->value->marked = false;
        node = node->next;
    }
    gc_mark_roots(j);
    gc_sweep(j);
    j->total_allocations = 0;
}

Value* create_value(JustState *j, ValueType type) {
    Value *v = calloc(1, sizeof(Value));
    if (!v) { just_error(j, "Out of memory"); exit(1); }
    v->type = type;
    v->marked = false;
    gc_add_node(j, v);
    return v;
}

Value* create_number(JustState *j, double n) {
    Value *v = create_value(j, TYPE_NUMBER);
    v->data.number = n;
    return v;
}

Value* create_string(JustState *j, const char *s) {
    Value *v = create_value(j, TYPE_STRING);
    v->data.string = s ? str_dup(s) : str_dup("");
    return v;
}

Value* create_bool(JustState *j, bool b) {
    Value *v = create_value(j, TYPE_BOOL);
    v->data.boolean = b;
    return v;
}

Value* create_object(JustState *j) {
    Value *v = create_value(j, TYPE_OBJECT);
    v->data.object.capacity = 8;
    v->data.object.keys = malloc(sizeof(char*) * 8);
    v->data.object.values = malloc(sizeof(Value*) * 8);
    v->data.object.count = 0;
    return v;
}

Value* create_array(JustState *j) {
    Value *v = create_value(j, TYPE_ARRAY);
    v->data.array.capacity = 8;
    v->data.array.items = malloc(sizeof(Value*) * 8);
    v->data.array.count = 0;
    return v;
}

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

bool object_has(Value *obj, const char *key) {
    if (!obj || obj->type != TYPE_OBJECT) return false;
    for (int i = 0; i < obj->data.object.count; i++)
        if (strcmp(obj->data.object.keys[i], key) == 0) return true;
    return false;
}

Value* clone_value_internal(JustState *j, Value *v, int depth) {
    if (depth > 1000) return create_value(j, TYPE_NULL);
    if (!v) return create_value(j, TYPE_NULL);
    switch (v->type) {
        case TYPE_NULL: return create_value(j, TYPE_NULL);
        case TYPE_NUMBER: return create_number(j, v->data.number);
        case TYPE_BOOL: return create_bool(j, v->data.boolean);
        case TYPE_STRING: return create_string(j, v->data.string);
        case TYPE_OBJECT: {
            Value *n = create_object(j);
            for (int i = 0; i < v->data.object.count; i++)
                object_set(n, v->data.object.keys[i], clone_value_internal(j, v->data.object.values[i], depth + 1));
            return n;
        }
        case TYPE_ARRAY: {
            Value *n = create_array(j);
            for (int i = 0; i < v->data.array.count; i++)
                array_push(n, clone_value_internal(j, v->data.array.items[i], depth + 1));
            return n;
        }
        default: return v;
    }
}

Value* clone_value(JustState *j, Value *v) {
    return clone_value_internal(j, v, 0);
}

uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#ifndef JUST_NO_SQLITE

int handle_alloc(JustState *j, void *ptr) {
    for (int i = 0; i < j->handle_count; i++) {
        if (j->handles[i] == NULL) { j->handles[i] = ptr; return i; }
    }
    if (j->handle_count >= j->handle_capacity) {
        j->handle_capacity = j->handle_capacity ? j->handle_capacity * 2 : 16;
        j->handles = realloc(j->handles, sizeof(void*) * j->handle_capacity);
    }
    j->handles[j->handle_count] = ptr;
    return j->handle_count++;
}
void* handle_get(JustState *j, int id) {
    if (id < 0 || id >= j->handle_count) return NULL;
    return j->handles[id];
}
void handle_free(JustState *j, int id) {
    if (id >= 0 && id < j->handle_count) j->handles[id] = NULL;
}
#endif

int normalize_index(int index, int len) {
    if (index < 0) index += len;
    return index;
}

static GCNode out_of_range_gc_sentinel;

Value* array_get(Value *arr, int index) {
    if (!arr || arr->type != TYPE_ARRAY) {
        static Value out_of_range_null = { .type = TYPE_NULL, .gc_node = &out_of_range_gc_sentinel, .marked = true };
        return &out_of_range_null;
    }
    index = normalize_index(index, arr->data.array.count);
    if (index < 0 || index >= arr->data.array.count) {

        static Value out_of_range_null = { .type = TYPE_NULL, .gc_node = &out_of_range_gc_sentinel, .marked = true };
        return &out_of_range_null;
    }
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

double value_to_number(Value *v) {
    if (!v) return 0;
    switch (v->type) {
        case TYPE_NUMBER: return v->data.number;
        case TYPE_BOOL: return v->data.boolean ? 1.0 : 0.0;
        case TYPE_STRING: return atof(v->data.string);
        default: return 0;
    }
}

bool values_equal(Value *l, Value *r) {
    if (!l || !r) return l == r;
    if (l->type == TYPE_STRING && r->type == TYPE_STRING)
        return strcmp(l->data.string, r->data.string) == 0;
    if (l->type == TYPE_NULL || r->type == TYPE_NULL)
        return l->type == r->type;
    if (l->type == TYPE_BOOL && r->type == TYPE_BOOL)
        return l->data.boolean == r->data.boolean;
    if (l->type == TYPE_ARRAY || l->type == TYPE_OBJECT || r->type == TYPE_ARRAY || r->type == TYPE_OBJECT)
        return l == r;
    return value_to_number(l) == value_to_number(r);
}

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

static void format_double(double d, char *buf, size_t bufsize) {
    if (isnan(d)) { snprintf(buf, bufsize, "nan"); return; }
    if (isinf(d)) { snprintf(buf, bufsize, d > 0 ? "inf" : "-inf"); return; }
    if (d == trunc(d) && fabs(d) <= 9007199254740992.0 ) {
        snprintf(buf, bufsize, "%lld", (long long)d);
        return;
    }
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, bufsize, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) return;
    }
}

char* value_to_string_raw(Value *v) {
    if (!v) return str_dup("null");
    switch (v->type) {
        case TYPE_NULL: return str_dup("null");
        case TYPE_NUMBER: {
            char b[64];
            format_double(v->data.number, b, sizeof(b));
            return str_dup(b);
        }
        case TYPE_STRING: return str_dup(v->data.string);
        case TYPE_BOOL: return str_dup(v->data.boolean ? "true" : "false");
        case TYPE_OBJECT: {
            char **fs = malloc(sizeof(char*) * v->data.object.count);
            int tl = 2;
            for (int i = 0; i < v->data.object.count; i++) {
                char *vs = value_to_string_raw(v->data.object.values[i]);
                if (!vs) vs = str_dup("null");
                int n = strlen(v->data.object.keys[i]) + 2 + strlen(vs);
                fs[i] = malloc(n + 1);
                sprintf(fs[i], "%s: %s", v->data.object.keys[i], vs);
                tl += strlen(fs[i]);
                if (i < v->data.object.count - 1) tl += 2;
                free(vs);
            }
            char *r = malloc(tl + 1);
            int p = 0;
            p += sprintf(r + p, "{");
            for (int i = 0; i < v->data.object.count; i++) {
                p += sprintf(r + p, "%s", fs[i]);
                if (i < v->data.object.count - 1) p += sprintf(r + p, ", ");
                free(fs[i]);
            }
            p += sprintf(r + p, "}");
            free(fs);
            return r;
        }
        case TYPE_ARRAY: {
            char **is = malloc(sizeof(char*) * v->data.array.count);
            int tl = 2;
            for (int i = 0; i < v->data.array.count; i++) {
                is[i] = value_to_string_raw(v->data.array.items[i]);
                tl += strlen(is[i]);
                if (i < v->data.array.count - 1) tl += 2;
            }
            char *r = malloc(tl + 1);
            int p = 0;
            p += sprintf(r + p, "[");
            for (int i = 0; i < v->data.array.count; i++) {
                p += sprintf(r + p, "%s", is[i]);
                if (i < v->data.array.count - 1) p += sprintf(r + p, ", ");
                free(is[i]);
            }
            p += sprintf(r + p, "]");
            free(is);
            return r;
        }
        default: return str_dup("<function>");
    }
}

Value* value_to_string(JustState *j, Value *v) {
    if (!v) return create_string(j, "null");
    if (v->type == TYPE_STRING) return v;
    char *s = value_to_string_raw(v);
    Value *r = create_string(j, s);
    free(s);
    return r;
}

Value* value_to_json(JustState *j, Value *v) {
    if (!v) return create_string(j, "null");
    switch (v->type) {
        case TYPE_NULL: return create_string(j, "null");
        case TYPE_NUMBER: {
            char b[64];

            if (isnan(v->data.number) || isinf(v->data.number)) return create_string(j, "null");
            format_double(v->data.number, b, sizeof(b));
            return create_string(j, b);
        }
        case TYPE_BOOL: return create_string(j, v->data.boolean ? "true" : "false");
        case TYPE_STRING: {
            char *b = malloc(strlen(v->data.string) + 3);
            sprintf(b, "\"%s\"", v->data.string);
            Value *r = create_string(j, b);
            free(b);
            return r;
        }
        case TYPE_ARRAY: {
            char **ps = malloc(sizeof(char*) * v->data.array.count);
            int tl = 2;
            for (int i = 0; i < v->data.array.count; i++) {
                Value *jv = value_to_json(j, v->data.array.items[i]);
                ps[i] = jv->data.string;
                tl += strlen(ps[i]);
                if (i < v->data.array.count - 1) tl++;
                jv->data.string = NULL;
            }
            char *b = malloc(tl + 1);
            int p = 0;
            b[p++] = '[';
            for (int i = 0; i < v->data.array.count; i++) {
                p += sprintf(b + p, "%s", ps[i]);
                if (i < v->data.array.count - 1) b[p++] = ',';
                free(ps[i]);
            }
            b[p++] = ']';
            b[p] = '\0';
            free(ps);
            Value *r = create_string(j, b);
            free(b);
            return r;
        }
        case TYPE_OBJECT: {
            char **ks = malloc(sizeof(char*) * v->data.object.count);
            char **vs = malloc(sizeof(char*) * v->data.object.count);
            int tl = 2;
            for (int i = 0; i < v->data.object.count; i++) {
                Value *jv = value_to_json(j, v->data.object.values[i]);
                ks[i] = v->data.object.keys[i];
                vs[i] = jv->data.string;
                tl += strlen(ks[i]) + 3 + strlen(vs[i]);
                if (i < v->data.object.count - 1) tl++;
                jv->data.string = NULL;
            }
            char *b = malloc(tl + 1);
            int p = 0;
            b[p++] = '{';
            for (int i = 0; i < v->data.object.count; i++) {
                p += sprintf(b + p, "\"%s\":%s", ks[i], vs[i]);
                if (i < v->data.object.count - 1) b[p++] = ',';
                free(vs[i]);
            }
            b[p++] = '}';
            b[p] = '\0';
            free(ks);
            free(vs);
            Value *r = create_string(j, b);
            free(b);
            return r;
        }
        default: return create_string(j, "null");
    }
}

Value* json_parse_value(JustState *j, const char **p) {
    while (**p == ' ' || **p == '\n' || **p == '\t' || **p == '\r') (*p)++;
    if (**p == '{') {
        (*p)++;
        Value *o = create_object(j);
        while (**p && **p != '}') {
            while (**p == ' ' || **p == ',' || **p == '\n' || **p == '\t' || **p == '\r') (*p)++;
            if (**p == '}') break;
            if (**p == '"') {
                (*p)++;
                char key_buf[MAX_STRING];
                int kl = 0;
                while (**p && **p != '"' && kl < MAX_STRING - 1) {
                    if (**p == '\\' && *(*p + 1)) {
                        (*p)++;
                        switch (**p) {
                            case '"': key_buf[kl++]='"'; break;
                            case '\\': key_buf[kl++]='\\'; break;
                            case '/': key_buf[kl++]='/'; break;
                            case 'n': key_buf[kl++]='\n'; break;
                            case 't': key_buf[kl++]='\t'; break;
                            case 'r': key_buf[kl++]='\r'; break;
                            case 'f': key_buf[kl++]='\f'; break;
                            case 'b': key_buf[kl++]='\b'; break;
                            default: key_buf[kl++]='\\'; key_buf[kl++]=**p;
                        }
                    } else key_buf[kl++] = **p;
                    (*p)++;
                }
                key_buf[kl] = '\0';
                if (**p == '"') (*p)++;
                while (**p == ' ' || **p == ':') (*p)++;
                Value *v = json_parse_value(j, p);
                object_set(o, key_buf, v);
            }
        }
        if (**p == '}') (*p)++;
        return o;
    }
    if (**p == '[') {
        (*p)++;
        Value *a = create_array(j);
        while (**p && **p != ']') {
            while (**p == ' ' || **p == ',' || **p == '\n' || **p == '\t' || **p == '\r') (*p)++;
            if (**p == ']') break;
            Value *v = json_parse_value(j, p);
            array_push(a, v);
        }
        if (**p == ']') (*p)++;
        return a;
    }
    if (**p == '"') {
        (*p)++;
        char str_buf[MAX_STRING];
        int sl = 0;
        while (**p && **p != '"' && sl < MAX_STRING - 1) {
            if (**p == '\\' && *(*p + 1)) {
                (*p)++;
                switch (**p) {
                    case '"': str_buf[sl++]='"'; break;
                    case '\\': str_buf[sl++]='\\'; break;
                    case '/': str_buf[sl++]='/'; break;
                    case 'n': str_buf[sl++]='\n'; break;
                    case 't': str_buf[sl++]='\t'; break;
                    case 'r': str_buf[sl++]='\r'; break;
                    case 'f': str_buf[sl++]='\f'; break;
                    case 'b': str_buf[sl++]='\b'; break;
                    default: str_buf[sl++]='\\'; str_buf[sl++]=**p;
                }
            } else str_buf[sl++] = **p;
            (*p)++;
        }
        str_buf[sl] = '\0';
        if (**p == '"') (*p)++;
        return create_string(j, str_buf);
    }
    if (**p == 't' || **p == 'f') {
        bool b = (**p == 't');
        while (**p && isalpha(**p)) (*p)++;
        return create_bool(j, b);
    }
    if (**p == 'n') {
        while (**p && isalpha(**p)) (*p)++;
        return create_value(j, TYPE_NULL);
    }
    char *ep;
    double n = strtod(*p, &ep);
    if (ep != *p) {
        *p = ep;
        return create_number(j, n);
    }
    return create_value(j, TYPE_NULL);
}

void push_scope(JustState *j) {
    if (j->scope_depth >= MAX_SCOPE_DEPTH) {
        just_error(j, "Maximum scope depth exceeded");
        exit(1);
    }
    if (j->scope_depth >= j->scope_capacity) {
        int new_cap = j->scope_capacity ? j->scope_capacity * 2 : 32;
        if (new_cap > MAX_SCOPE_DEPTH) new_cap = MAX_SCOPE_DEPTH;
        j->scopes = realloc(j->scopes, sizeof(Scope*) * new_cap);
        j->scope_capacity = new_cap;
    }
    Scope *s = calloc(1, sizeof(Scope));
    s->var_capacity = 16;
    s->vars = malloc(sizeof(Variable) * 16);
    j->scopes[j->scope_depth++] = s;
}

void pop_scope(JustState *j) {
    if (j->scope_depth <= 0) return;
    j->scope_depth--;
    Scope *s = j->scopes[j->scope_depth];
    for (int i = 0; i < s->var_count; i++) {
        free(s->vars[i].name);
    }
    free(s->vars);
    free(s);
}

Variable* find_var(JustState *j, const char *name) {
    for (int s = j->scope_depth - 1; s >= 0; s--)
        for (int i = 0; i < j->scopes[s]->var_count; i++)
            if (strcmp(j->scopes[s]->vars[i].name, name) == 0)
                return &j->scopes[s]->vars[i];
    return NULL;
}

void declare_var(JustState *j, const char *name, Value *value, bool constant) {

    adopt_into_gc(j, value);
    Scope *cur = j->scopes[j->scope_depth - 1];
    for (int i = 0; i < cur->var_count; i++) {
        if (strcmp(cur->vars[i].name, name) == 0) {
            cur->vars[i].value = value;
            cur->vars[i].constant = constant;
            return;
        }
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

void set_var(JustState *j, const char *name, Value *value, bool constant) {
    adopt_into_gc(j, value);
    for (int s = j->scope_depth - 1; s >= 0; s--) {
        for (int i = 0; i < j->scopes[s]->var_count; i++) {
            if (strcmp(j->scopes[s]->vars[i].name, name) == 0) {
                if (j->scopes[s]->vars[i].constant) {
                    just_error(j, "Cannot reassign constant");
                    return;
                }
                j->scopes[s]->vars[i].value = value;
                return;
            }
        }
    }
    Scope *cur = j->scopes[j->scope_depth - 1];
    if (cur->var_count >= cur->var_capacity) {
        cur->var_capacity *= 2;
        cur->vars = realloc(cur->vars, sizeof(Variable) * cur->var_capacity);
    }
    cur->vars[cur->var_count].name = str_dup(name);
    cur->vars[cur->var_count].value = value;
    cur->vars[cur->var_count].constant = constant;
    cur->var_count++;
}

void add_func(JustState *j, const char *name, char **params, int param_count, int start, int end) {
    if (j->func_count >= j->func_capacity) {
        j->func_capacity = j->func_capacity ? j->func_capacity * 2 : 16;
        j->funcs = realloc(j->funcs, sizeof(Function) * j->func_capacity);
    }
    j->funcs[j->func_count].name = str_dup(name);
    j->funcs[j->func_count].params = malloc(sizeof(char*) * param_count);
    for (int i = 0; i < param_count; i++)
        j->funcs[j->func_count].params[i] = str_dup(params[i]);
    j->funcs[j->func_count].param_count = param_count;
    j->funcs[j->func_count].body_start = start;
    j->funcs[j->func_count].body_end = end;
    j->funcs[j->func_count].is_native = false;
    j->funcs[j->func_count].native_func = NULL;
    j->func_count++;
}

Function* find_func(JustState *j, const char *name) {
    for (int i = 0; i < j->func_count; i++)
        if (strcmp(j->funcs[i].name, name) == 0)
            return &j->funcs[i];
    return NULL;
}

Value* capture_scope(JustState *j) {

    Value *env = create_object(j);
    for (int s = 0; s < j->scope_depth; s++) {
        Scope *sc = j->scopes[s];
        for (int i = 0; i < sc->var_count; i++)
            object_set(env, sc->vars[i].name, sc->vars[i].value);
    }
    return env;
}

Value* call_function_value(JustState *j, Value *fn, Value **args, int argc) {
    if (!fn) return create_value(j, TYPE_NULL);
    if (fn->type == TYPE_NATIVE_FUNC) {
        Value *r = fn->data.native_func(j, args, argc);
        adopt_into_gc(j, r);
        return r;
    }
    if (fn->type == TYPE_FUNCTION) {
        if (fn->is_lambda) {

            if (j->call_depth >= j->max_call_depth) {
                just_error(j, "Maximum call stack size exceeded");
                free(j->error_message);
                j->error_message = str_dup("Maximum call stack size exceeded");
                j->error_value = create_string(j, "Maximum call stack size exceeded");
                j->current_flow = FLOW_ERROR;
                return create_value(j, TYPE_NULL);
            }
            j->call_stack[j->call_depth] = "<lambda>";
            j->call_depth++;
            int od = j->scope_depth;
            push_scope(j);
            if (fn->data.lambda.captured) {
                Value *cap = fn->data.lambda.captured;
                for (int i = 0; i < cap->data.object.count; i++)
                    declare_var(j, cap->data.object.keys[i], cap->data.object.values[i], false);
            }
            for (int i = 0; i < fn->data.lambda.param_count && i < argc; i++)
                declare_var(j, fn->data.lambda.params[i], args[i], false);
            ControlFlow of = j->current_flow;
            j->current_flow = FLOW_NORMAL;
            j->return_value = NULL;
            execute_block(j, fn->data.lambda.body_start, fn->data.lambda.body_end);
            if (j->current_flow != FLOW_ERROR) j->current_flow = of;
            Value *saved_return = j->return_value;
            while (j->scope_depth > od) pop_scope(j);
            j->call_depth--;
            Value *r = saved_return ? saved_return : create_value(j, TYPE_NULL);
            j->return_value = NULL;
            return r;
        }
        Function *f = find_func(j, fn->data.string);
        if (!f) return create_value(j, TYPE_NULL);
        if (f->is_native) {
            Value *r = f->native_func(j, args, argc);
            adopt_into_gc(j, r);
            return r;
        }

        if (j->call_depth >= j->max_call_depth) {
            just_error(j, "Maximum call stack size exceeded");
            free(j->error_message);
            j->error_message = str_dup("Maximum call stack size exceeded");
            j->error_value = create_string(j, "Maximum call stack size exceeded");
            j->current_flow = FLOW_ERROR;
            return create_value(j, TYPE_NULL);
        }
        j->call_stack[j->call_depth] = f->name;
        j->call_depth++;

        int od = j->scope_depth;
        push_scope(j);
        for (int i = 0; i < f->param_count && i < argc; i++)
            declare_var(j, f->params[i], args[i], false);
        ControlFlow of = j->current_flow;
        j->current_flow = FLOW_NORMAL;
        j->return_value = NULL;
        execute_block(j, f->body_start, f->body_end);

        if (j->current_flow != FLOW_ERROR) j->current_flow = of;
        Value *saved_return = j->return_value;
        while (j->scope_depth > od) pop_scope(j);
        j->call_depth--;
        Value *r = saved_return ? saved_return : create_value(j, TYPE_NULL);
        j->return_value = NULL;
        return r;
    }
    return create_value(j, TYPE_NULL);
}

Value* builtin_print(JustState *j, Value **args, int count) {
    for (int i = 0; i < count; i++) {
        char *s = value_to_string_raw(args[i]);
        printf("%s", s);
        free(s);
        if (i < count - 1) printf(" ");
    }
    printf("\n");
    return create_value(j, TYPE_NULL);
}

Value* builtin_type(JustState *j, Value **args, int count) {
    if (count < 1) return create_string(j, "null");
    char *t[] = {"null","number","string","bool","object","array","function","native_function"};
    return create_string(j, t[args[0]->type]);
}

Value* builtin_len(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    Value *v = args[0];
    if (v->type == TYPE_STRING) return create_number(j, strlen(v->data.string));
    if (v->type == TYPE_ARRAY) return create_number(j, v->data.array.count);
    if (v->type == TYPE_OBJECT) return create_number(j, v->data.object.count);
    return create_number(j, 0);
}

Value* builtin_input(JustState *j, Value **args, int count) {
    (void)args; (void)count;
    char b[MAX_STRING];
    if (fgets(b, MAX_STRING, stdin)) {
        b[strcspn(b, "\n")] = 0;
        return create_string(j, b);
    }
    return create_string(j, "");
}

Value* builtin_int(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, (int)value_to_number(args[0]));
}

Value* builtin_str(JustState *j, Value **args, int count) {
    if (count < 1) return create_string(j, "");
    return value_to_string(j, args[0]);
}

Value* builtin_bool(JustState *j, Value **args, int count) {
    if (count < 1) return create_bool(j, false);
    return create_bool(j, value_to_bool(args[0]));
}

Value* builtin_range(JustState *j, Value **args, int count) {
    Value *a = create_array(j);
    if (count >= 1) {
        int s = 0, e = (int)value_to_number(args[0]), st = 1;
        if (count >= 2) {
            s = e;
            e = (int)value_to_number(args[1]);
        }
        if (count >= 3) st = (int)value_to_number(args[2]);
        if (st > 0)
            for (int i = s; i < e; i += st) array_push(a, create_number(j, i));
        else if (st < 0)
            for (int i = s; i > e; i += st) array_push(a, create_number(j, i));
    }
    return a;
}

Value* builtin_keys(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_OBJECT) return create_array(j);
    Value *a = create_array(j);
    for (int i = 0; i < args[0]->data.object.count; i++)
        array_push(a, create_string(j, args[0]->data.object.keys[i]));
    return a;
}

Value* builtin_values(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_OBJECT) return create_array(j);
    Value *a = create_array(j);
    for (int i = 0; i < args[0]->data.object.count; i++)
        array_push(a, clone_value(j, args[0]->data.object.values[i]));
    return a;
}

Value* builtin_has(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_OBJECT || args[1]->type != TYPE_STRING)
        return create_bool(j, false);
    return create_bool(j, object_has(args[0], args[1]->data.string));
}

Value* builtin_read_file(JustState *j, Value **args, int count) {
    if (!(j->capabilities & JUST_CAP_FILES)) { just_error(j, "File access disabled for this interpreter"); return create_value(j, TYPE_NULL); }
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
    FILE *f = fopen(args[0]->data.string, "r");
    if (!f) return create_string(j, "");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(sz + 1);
    size_t _rd = fread(b, 1, sz, f); (void)_rd;
    b[sz] = '\0';
    fclose(f);
    Value *v = create_string(j, b);
    free(b);
    return v;
}

Value* builtin_write_file(JustState *j, Value **args, int count) {
    if (!(j->capabilities & JUST_CAP_FILES)) { just_error(j, "File access disabled for this interpreter"); return create_bool(j, false); }
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_bool(j, false);
    FILE *f = fopen(args[0]->data.string, "w");
    if (!f) return create_bool(j, false);
    fprintf(f, "%s", args[1]->data.string);
    fclose(f);
    return create_bool(j, true);
}

Value* builtin_json_export(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING) return create_bool(j, false);
    FILE *f = fopen(args[0]->data.string, "w");
    if (!f) { return create_bool(j, false); }
    Value *jv = value_to_json(j, args[1]);
    fprintf(f, "%s", jv->data.string);
    fclose(f);
    return create_bool(j, true);
}

Value* builtin_json(JustState *j, Value **args, int count) {
    if (count < 1) return create_string(j, "null");
    return value_to_json(j, args[0]);
}

Value* builtin_json_parse(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_value(j, TYPE_NULL);
    const char *p = args[0]->data.string;
    return json_parse_value(j, &p);
}

static char* net_send_recv(const char *host, int port, bool use_tls, const char *request, size_t request_len) {
#ifndef JUST_NO_TLS
    if (use_tls) {
        mbedtls_net_context server_fd;
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context ctr_drbg;
        mbedtls_ssl_context ssl;
        mbedtls_ssl_config conf;
        char port_str[16];
        char *result = NULL;
        snprintf(port_str, sizeof(port_str), "%d", port);

        mbedtls_net_init(&server_fd);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_entropy_init(&entropy);

        const char *pers = "just_https_client";
        int ret;
        if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)pers, strlen(pers)) != 0) goto tls_cleanup;
        if (mbedtls_net_connect(&server_fd, host, port_str, MBEDTLS_NET_PROTO_TCP) != 0) goto tls_cleanup;
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto tls_cleanup;

        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto tls_cleanup;
        mbedtls_ssl_set_hostname(&ssl, host);
        mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) goto tls_cleanup;
        }

        size_t written = 0;
        while (written < request_len) {
            ret = mbedtls_ssl_write(&ssl, (const unsigned char*)request + written, request_len - written);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (ret < 0) goto tls_cleanup;
            written += (size_t)ret;
        }

        {
            size_t cap = MAX_STRING, total = 0;
            char *buf = malloc(cap);
            for (;;) {
                if (total + 4096 > cap) { cap *= 2; buf = realloc(buf, cap); }
                ret = mbedtls_ssl_read(&ssl, (unsigned char*)buf + total, cap - total - 1);
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                if (ret <= 0) break;
                total += (size_t)ret;
            }
            buf[total] = '\0';
            result = buf;
        }
        mbedtls_ssl_close_notify(&ssl);

    tls_cleanup:
        mbedtls_net_free(&server_fd);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return result;
    }
#else
    if (use_tls) return NULL; 
#endif

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return NULL;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    struct hostent *he = gethostbyname(host);
    if (!he) { closesocket(sock); return NULL; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { closesocket(sock); return NULL; }
    send(sock, request, (int)request_len, 0);

    size_t cap = MAX_STRING, total = 0;
    char *buf = malloc(cap);
    int n;
    while ((n = recv(sock, buf + total, (int)(cap - total - 1), 0)) > 0) {
        total += (size_t)n;
        if (total + 4096 > cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[total] = '\0';
    closesocket(sock);
    return buf;
}

static void parse_url(const char *url_in, char *host, size_t host_cap,
                       char *path, size_t path_cap, int *port, bool *use_tls) {
    char *url = str_dup(url_in);
    *use_tls = false;
    *port = 80;
    strncpy(path, "/", path_cap);

    char *p = url;
    char *scheme_end = strstr(url, "://");
    if (scheme_end) {
        if (strncmp(url, "https", 5) == 0) { *use_tls = true; *port = 443; }
        p = scheme_end + 3;
    }
    char *ps = strchr(p, '/');
    if (ps) {
        strncpy(path, ps, path_cap - 1);
        path[path_cap - 1] = '\0';
        *ps = '\0';
    }
    char *pp = strchr(p, ':');
    if (pp) {
        *pp = '\0';
        *port = atoi(pp + 1);
    }
    strncpy(host, p, host_cap - 1);
    host[host_cap - 1] = '\0';
    free(url);
}

Value* builtin_http_get(JustState *j, Value **args, int count) {
    if (!(j->capabilities & JUST_CAP_NET)) { just_error(j, "Network access disabled for this interpreter"); return create_value(j, TYPE_NULL); }
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
#ifdef _WIN32
    if (!j->winsock_initialized) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
        j->winsock_initialized = true;
    }
#endif
    char host[256], path[1024];
    int port; bool use_tls;
    parse_url(args[0]->data.string, host, sizeof(host), path, sizeof(path), &port, &use_tls);

    char req[2048];
    int req_len = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);

    char *buf = net_send_recv(host, port, use_tls, req, (size_t)req_len);
    if (!buf) {
#ifndef JUST_NO_TLS
        return create_string(j, "");
#else
        if (use_tls) { just_error(j, "HTTPS requested but this build has JUST_NO_TLS -- rebuild with mbedTLS to use https:// URLs"); }
        return create_string(j, "");
#endif
    }
    char *body = strstr(buf, "\r\n\r\n");
    Value *result = body ? create_string(j, body + 4) : create_string(j, buf);
    free(buf);
    return result;
}

Value* builtin_import_json(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_value(j, TYPE_NULL);
    FILE *f = fopen(args[0]->data.string, "r");
    if (!f) return create_value(j, TYPE_NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(sz + 1);
    size_t _rd = fread(b, 1, sz, f); (void)_rd;
    b[sz] = '\0';
    fclose(f);
    const char *p = b;
    Value *v = json_parse_value(j, &p);
    free(b);
    return v;
}

Value* builtin_split(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_array(j);
    char *s = str_dup(args[0]->data.string), *d = args[1]->data.string;
    Value *a = create_array(j);
    char *saveptr, *token = strtok_r(s, d, &saveptr);
    while (token) {
        array_push(a, create_string(j, token));
        token = strtok_r(NULL, d, &saveptr);
    }
    free(s);
    return a;
}

Value* builtin_upper(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
    char *s = str_dup(args[0]->data.string);
    for (int i = 0; s[i]; i++) s[i] = toupper(s[i]);
    Value *v = create_string(j, s);
    free(s);
    return v;
}

Value* builtin_lower(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
    char *s = str_dup(args[0]->data.string);
    for (int i = 0; s[i]; i++) s[i] = tolower(s[i]);
    Value *v = create_string(j, s);
    free(s);
    return v;
}

Value* builtin_replace(JustState *j, Value **args, int count) {
    if (count < 3 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING || args[2]->type != TYPE_STRING)
        return create_string(j, "");
    char *s = str_dup(args[0]->data.string), *f = args[1]->data.string, *t = args[2]->data.string;
    char *p;
    while ((p = strstr(s, f))) {
        int pl = p - s;
        int nl = pl + strlen(t) + strlen(p + strlen(f)) + 1;
        char *n = malloc(nl);
        strncpy(n, s, pl);
        n[pl] = '\0';
        strcat(n, t);
        strcat(n, p + strlen(f));
        free(s);
        s = n;
    }
    Value *v = create_string(j, s);
    free(s);
    return v;
}

Value* builtin_regex_match(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_bool(j, false);
    int matchlength;
    int idx = re_match(args[1]->data.string, args[0]->data.string, &matchlength);
    return create_bool(j, idx >= 0);
}

Value* builtin_regex_find(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_value(j, TYPE_NULL);
    int matchlength;
    int idx = re_match(args[1]->data.string, args[0]->data.string, &matchlength);
    if (idx < 0) return create_value(j, TYPE_NULL);
    char *out = malloc((size_t)matchlength + 1);
    memcpy(out, args[0]->data.string + idx, (size_t)matchlength);
    out[matchlength] = '\0';
    Value *v = create_string(j, out);
    free(out);
    return v;
}

Value* builtin_regex_find_all(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_array(j);
    Value *result = create_array(j);
    const char *s = args[0]->data.string;
    int offset = 0;
    int total = (int)strlen(s);
    while (offset <= total) {
        int matchlength;
        int idx = re_match(args[1]->data.string, s + offset, &matchlength);
        if (idx < 0) break;
        if (matchlength == 0) {
            offset += idx + 1;
            continue;
        }
        char *out = malloc((size_t)matchlength + 1);
        memcpy(out, s + offset + idx, (size_t)matchlength);
        out[matchlength] = '\0';
        array_push(result, create_string(j, out));
        free(out);
        offset += idx + matchlength;
    }
    return result;
}

Value* builtin_regex_replace(JustState *j, Value **args, int count) {
    if (count < 3 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING || args[2]->type != TYPE_STRING)
        return create_string(j, args[0]->type == TYPE_STRING ? args[0]->data.string : "");
    const char *s = args[0]->data.string;
    const char *pattern = args[1]->data.string;
    const char *repl = args[2]->data.string;
    size_t cap = strlen(s) + 1;
    char *out = malloc(cap);
    out[0] = '\0';
    size_t out_len = 0;
    int offset = 0;
    int total = (int)strlen(s);

    while (offset <= total) {
        int matchlength;
        int idx = re_match(pattern, s + offset, &matchlength);
        if (idx < 0) break;
        size_t prefix_len = (size_t)idx;
        size_t need = out_len + prefix_len + strlen(repl) + 1;
        if (need > cap) { cap = need * 2; out = realloc(out, cap); }
        memcpy(out + out_len, s + offset, prefix_len);
        out_len += prefix_len;
        memcpy(out + out_len, repl, strlen(repl));
        out_len += strlen(repl);
        out[out_len] = '\0';

        if (matchlength == 0) {
            size_t need2 = out_len + 2;
            if (need2 > cap) { cap = need2 * 2; out = realloc(out, cap); }
            if (offset + idx < total) out[out_len++] = s[offset + idx];
            out[out_len] = '\0';
            offset += idx + 1;
        } else {
            offset += idx + matchlength;
        }
    }

    if (offset < total) {
        size_t rest = (size_t)(total - offset);
        size_t need = out_len + rest + 1;
        if (need > cap) { cap = need; out = realloc(out, cap); }
        memcpy(out + out_len, s + offset, rest);
        out_len += rest;
        out[out_len] = '\0';
    }
    Value *v = create_string(j, out);
    free(out);
    return v;
}

Value* builtin_sqrt(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, sqrt(value_to_number(args[0])));
}

Value* builtin_random(JustState *j, Value **args, int count) {

    double max = count >= 1 ? value_to_number(args[0]) : 1.0;
    double span = 4294967296.0;
    double hi = (double)xorshift32(&j->rand_seed) / span;
    double lo = (double)xorshift32(&j->rand_seed) / span;
    double r = hi + lo / span;
    if (r >= 1.0) r = 0.999999999999;
    return create_number(j, r * max);
}

Value* builtin_substr(JustState *j, Value **args, int count) {

    if (count < 2 || args[0]->type != TYPE_STRING) return create_string(j, "");
    const char *s = args[0]->data.string;
    int len = (int)strlen(s);
    int start = (int)value_to_number(args[1]);
    if (start < 0) start += len;
    if (start < 0) start = 0;
    if (start > len) start = len;
    int take = len - start;
    if (count >= 3) {
        take = (int)value_to_number(args[2]);
        if (take < 0) take = 0;
    }
    if (start + take > len) take = len - start;
    char *out = malloc((size_t)take + 1);
    memcpy(out, s + start, (size_t)take);
    out[take] = '\0';
    Value *v = create_string(j, out);
    free(out);
    return v;
}

Value* builtin_sin(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, sin(value_to_number(args[0])));
}

Value* builtin_cos(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, cos(value_to_number(args[0])));
}

Value* builtin_tan(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, tan(value_to_number(args[0])));
}

Value* builtin_log(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, log(value_to_number(args[0])));
}

Value* builtin_exp(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, exp(value_to_number(args[0])));
}

Value* builtin_filter(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_ARRAY ||
        (args[1]->type != TYPE_NATIVE_FUNC && args[1]->type != TYPE_FUNCTION))
        return create_array(j);

    Value *result = create_array(j);
    for (int i = 0; i < args[0]->data.array.count; i++) {
        Value *item = args[0]->data.array.items[i];
        item->marked = true;
        Value *cond = call_function_value(j, args[1], &item, 1);
        if (value_to_bool(cond)) array_push(result, item);
    }
    return result;
}

Value* builtin_map(JustState *j, Value **args, int count) {

    if (count < 2 || args[0]->type != TYPE_ARRAY ||
        (args[1]->type != TYPE_NATIVE_FUNC && args[1]->type != TYPE_FUNCTION))
        return create_array(j);
    Value *result = create_array(j);
    for (int i = 0; i < args[0]->data.array.count; i++) {
        Value *item = args[0]->data.array.items[i];
        item->marked = true;
        Value *mapped = call_function_value(j, args[1], &item, 1);
        array_push(result, mapped);
    }
    return result;
}

Value* builtin_reduce(JustState *j, Value **args, int count) {

    if (count < 2 || args[0]->type != TYPE_ARRAY ||
        (args[1]->type != TYPE_NATIVE_FUNC && args[1]->type != TYPE_FUNCTION))
        return create_value(j, TYPE_NULL);
    Value *acc;
    int start = 0;
    if (count >= 3) {
        acc = args[2];
    } else if (args[0]->data.array.count > 0) {
        acc = args[0]->data.array.items[0];
        start = 1;
    } else {
        return create_value(j, TYPE_NULL);
    }
    for (int i = start; i < args[0]->data.array.count; i++) {
        Value *pair[2] = { acc, args[0]->data.array.items[i] };
        acc = call_function_value(j, args[1], pair, 2);
    }
    return acc;
}

Value* builtin_find(JustState *j, Value **args, int count) {

    if (count < 2 || args[0]->type != TYPE_ARRAY ||
        (args[1]->type != TYPE_NATIVE_FUNC && args[1]->type != TYPE_FUNCTION))
        return create_value(j, TYPE_NULL);
    for (int i = 0; i < args[0]->data.array.count; i++) {
        Value *item = args[0]->data.array.items[i];
        Value *cond = call_function_value(j, args[1], &item, 1);
        if (value_to_bool(cond)) return item;
    }
    return create_value(j, TYPE_NULL);
}

Value* builtin_index_of(JustState *j, Value **args, int count) {

    if (count < 2) return create_number(j, -1);
    if (args[0]->type == TYPE_ARRAY) {
        for (int i = 0; i < args[0]->data.array.count; i++)
            if (values_equal(args[0]->data.array.items[i], args[1])) return create_number(j, i);
        return create_number(j, -1);
    }
    if (args[0]->type == TYPE_STRING && args[1]->type == TYPE_STRING) {
        char *p = strstr(args[0]->data.string, args[1]->data.string);
        return create_number(j, p ? (double)(p - args[0]->data.string) : -1);
    }
    return create_number(j, -1);
}

Value* builtin_includes(JustState *j, Value **args, int count) {
    if (count < 2) return create_bool(j, false);
    Value *idx = builtin_index_of(j, args, count);
    return create_bool(j, value_to_number(idx) >= 0);
}

Value* builtin_slice(JustState *j, Value **args, int count) {

    if (count < 2) return create_value(j, TYPE_NULL);
    if (args[0]->type == TYPE_ARRAY) {
        int len = args[0]->data.array.count;
        int start = normalize_index((int)value_to_number(args[1]), len);
        int end = count >= 3 ? normalize_index((int)value_to_number(args[2]), len) : len;
        if (start < 0) start = 0;
        if (end > len) end = len;
        Value *result = create_array(j);
        for (int i = start; i < end; i++) array_push(result, args[0]->data.array.items[i]);
        return result;
    }
    if (args[0]->type == TYPE_STRING) {
        Value *sargs[3];
        sargs[0] = args[0];
        sargs[1] = args[1];
        int n = 2;
        if (count >= 3) {
            int len = (int)strlen(args[0]->data.string);
            int start = normalize_index((int)value_to_number(args[1]), len);
            int end = normalize_index((int)value_to_number(args[2]), len);
            sargs[2] = create_number(j, end - start > 0 ? end - start : 0);
            n = 3;
        }
        return builtin_substr(j, sargs, n);
    }
    return create_value(j, TYPE_NULL);
}

Value* builtin_concat(JustState *j, Value **args, int count) {

    Value *result = create_array(j);
    for (int a = 0; a < count; a++) {
        if (args[a]->type != TYPE_ARRAY) continue;
        for (int i = 0; i < args[a]->data.array.count; i++)
            array_push(result, args[a]->data.array.items[i]);
    }
    return result;
}

Value* builtin_unique(JustState *j, Value **args, int count) {

    if (count < 1 || args[0]->type != TYPE_ARRAY) return create_array(j);
    Value *result = create_array(j);
    for (int i = 0; i < args[0]->data.array.count; i++) {
        Value *item = args[0]->data.array.items[i];
        bool seen = false;
        for (int k = 0; k < result->data.array.count; k++) {
            if (values_equal(result->data.array.items[k], item)) { seen = true; break; }
        }
        if (!seen) array_push(result, item);
    }
    return result;
}

Value* builtin_sum(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_ARRAY) return create_number(j, 0);
    double total = 0;
    for (int i = 0; i < args[0]->data.array.count; i++)
        total += value_to_number(args[0]->data.array.items[i]);
    return create_number(j, total);
}

Value* builtin_clamp(JustState *j, Value **args, int count) {
    if (count < 3) return create_number(j, count >= 1 ? value_to_number(args[0]) : 0);
    double v = value_to_number(args[0]), lo = value_to_number(args[1]), hi = value_to_number(args[2]);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return create_number(j, v);
}

Value* builtin_pad_start(JustState *j, Value **args, int count) {

    if (count < 2 || args[0]->type != TYPE_STRING) return create_string(j, "");
    const char *s = args[0]->data.string;
    int target = (int)value_to_number(args[1]);
    char pad = (count >= 3 && args[2]->type == TYPE_STRING && args[2]->data.string[0]) ? args[2]->data.string[0] : ' ';
    int len = (int)strlen(s);
    if (len >= target) return create_string(j, s);
    char *out = malloc((size_t)target + 1);
    int padlen = target - len;
    memset(out, pad, (size_t)padlen);
    memcpy(out + padlen, s, (size_t)len + 1);
    Value *v = create_string(j, out);
    free(out);
    return v;
}

Value* builtin_pad_end(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING) return create_string(j, "");
    const char *s = args[0]->data.string;
    int target = (int)value_to_number(args[1]);
    char pad = (count >= 3 && args[2]->type == TYPE_STRING && args[2]->data.string[0]) ? args[2]->data.string[0] : ' ';
    int len = (int)strlen(s);
    if (len >= target) return create_string(j, s);
    char *out = malloc((size_t)target + 1);
    memcpy(out, s, (size_t)len);
    memset(out + len, pad, (size_t)(target - len));
    out[target] = '\0';
    Value *v = create_string(j, out);
    free(out);
    return v;
}

Value* builtin_merge(JustState *j, Value **args, int count) {

    Value *result = create_object(j);
    for (int a = 0; a < count; a++) {
        if (args[a]->type != TYPE_OBJECT) continue;
        for (int i = 0; i < args[a]->data.object.count; i++)
            object_set(result, args[a]->data.object.keys[i], args[a]->data.object.values[i]);
    }
    return result;
}

Value* builtin_entries(JustState *j, Value **args, int count) {

    if (count < 1 || args[0]->type != TYPE_OBJECT) return create_array(j);
    Value *result = create_array(j);
    for (int i = 0; i < args[0]->data.object.count; i++) {
        Value *pair = create_array(j);
        array_push(pair, create_string(j, args[0]->data.object.keys[i]));
        array_push(pair, args[0]->data.object.values[i]);
        array_push(result, pair);
    }
    return result;
}

Value* builtin_floor(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, floor(value_to_number(args[0])));
}

Value* builtin_ceil(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, ceil(value_to_number(args[0])));
}

Value* builtin_round(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    return create_number(j, round(value_to_number(args[0])));
}

Value* builtin_now(JustState *j, Value **args, int count) {
    (void)args; (void)count;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[128];

    if (count >= 1 && args[0]->type == TYPE_STRING) {

        strftime(buf, sizeof(buf), args[0]->data.string, tm);
    } else {

        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    }
    return create_string(j, buf);
}

Value* builtin_http_post(JustState *j, Value **args, int count) {
    if (!(j->capabilities & JUST_CAP_NET)) { just_error(j, "Network access disabled for this interpreter"); return create_value(j, TYPE_NULL); }
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_string(j, "");
#ifdef _WIN32
    if (!j->winsock_initialized) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
        j->winsock_initialized = true;
    }
#endif
    char *body = args[1]->data.string;
    char host[256], path[1024];
    int port; bool use_tls;
    parse_url(args[0]->data.string, host, sizeof(host), path, sizeof(path), &port, &use_tls);

    char *req = malloc(4096 + strlen(body));
    int req_len = sprintf(req, "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
             path, host, (int)strlen(body), body);

    char *buf = net_send_recv(host, port, use_tls, req, (size_t)req_len);
    free(req);
    if (!buf) {
#ifdef JUST_NO_TLS
        if (use_tls) { just_error(j, "HTTPS requested but this build has JUST_NO_TLS -- rebuild with mbedTLS to use https:// URLs"); }
#endif
        return create_string(j, "");
    }
    char *resp = strstr(buf, "\r\n\r\n");
    Value *result = resp ? create_string(j, resp + 4) : create_string(j, buf);
    free(buf);
    return result;
}

Value* builtin_exec(JustState *j, Value **args, int count) {
    if (!(j->capabilities & JUST_CAP_EXEC)) { just_error(j, "Shell exec disabled for this interpreter"); return create_string(j, ""); }
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
    char *cmd = args[0]->data.string;
    char buf[MAX_STRING];
    FILE *fp = popen(cmd, "r");
    if (!fp) return create_string(j, "");
    int total = 0;
    while (fgets(buf + total, MAX_STRING - total - 1, fp))
        total = strlen(buf);
    pclose(fp);
    return create_string(j, buf);
}

Value* builtin_sleep(JustState *j, Value **args, int count) {
    if (count < 1) return create_value(j, TYPE_NULL);
    int ms = (int)value_to_number(args[0]);
    if (ms < 0) ms = 0;
    if (ms > 60000) ms = 60000;
    just_sleep_ms(ms);
    return create_value(j, TYPE_NULL);
}

Value* builtin_env(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
    char *val = getenv(args[0]->data.string);
    return val ? create_string(j, val) : create_string(j, "");
}

Value* builtin_color(JustState *j, Value **args, int count, const char *ansi_code, int win_color) {
    (void)win_color;
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
    char *text = args[0]->data.string;
#ifdef _WIN32
    if (j->win10_ansi_supported) {
        char buf[MAX_STRING];
        snprintf(buf, MAX_STRING, "\033[%sm%s\033[0m", ansi_code, text);
        return create_string(j, buf);
    } else {
        return create_string(j, text);
    }
#else
    char buf[MAX_STRING];
    snprintf(buf, MAX_STRING, "\033[%sm%s\033[0m", ansi_code, text);
    return create_string(j, buf);
#endif
}

Value* builtin_red(JustState *j, Value **args, int count) {
    return builtin_color(j, args, count, "31", 12);
}
Value* builtin_green(JustState *j, Value **args, int count) {
    return builtin_color(j, args, count, "32", 10);
}
Value* builtin_yellow(JustState *j, Value **args, int count) {
    return builtin_color(j, args, count, "33", 14);
}
Value* builtin_blue(JustState *j, Value **args, int count) {
    return builtin_color(j, args, count, "34", 9);
}
Value* builtin_magenta(JustState *j, Value **args, int count) {
    return builtin_color(j, args, count, "35", 13);
}
Value* builtin_cyan(JustState *j, Value **args, int count) {
    return builtin_color(j, args, count, "36", 11);
}
Value* builtin_bold(JustState *j, Value **args, int count) {
    return builtin_color(j, args, count, "1", 15);
}

Value* builtin_task(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_value(j, TYPE_NULL);
    Function *f = find_func(j, args[0]->data.string);
    if (!f) {
        just_error(j, "Task not found");
        return create_value(j, TYPE_NULL);
    }

    int od = j->scope_depth;
    push_scope(j);
    ControlFlow of = j->current_flow;
    j->current_flow = FLOW_NORMAL;
    j->return_value = NULL;
    execute_block(j, f->body_start, f->body_end);
    if (j->current_flow != FLOW_ERROR) j->current_flow = of;
    while (j->scope_depth > od) pop_scope(j);
    return create_value(j, TYPE_NULL);
}

Value* builtin_watch(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING) return create_value(j, TYPE_NULL);
    char *filepath = args[0]->data.string;
    struct stat st;
    if (stat(filepath, &st) != 0) {
        just_error(j, "watch: file not found");
        return create_value(j, TYPE_NULL);
    }
    time_t last_modified = st.st_mtime;
    printf("Watching: %s\n", filepath);

    long max_checks = j->max_iterations;
    if (count >= 3) {
        double mc = value_to_number(args[2]);
        if (mc > 0) max_checks = (long)mc;
    }
    for (long iter = 0; iter < max_checks; iter++) {
        just_sleep_ms(500);
        if (stat(filepath, &st) == 0 && st.st_mtime != last_modified) {
            last_modified = st.st_mtime;
            if (args[1]->type == TYPE_STRING) {
                Value *ta[1] = {args[1]};
                builtin_task(j, ta, 1);
            }
        }
    }
    return create_value(j, TYPE_NULL);
}

Value* builtin_error(JustState *j, Value **args, int count) {

    const char *msg = (count >= 1 && args[0]->type == TYPE_STRING) ? args[0]->data.string : "error";
    just_error(j, msg);
    free(j->error_message);
    j->error_message = str_dup(msg);
    j->error_value = create_string(j, msg);
    j->current_flow = FLOW_ERROR;
    return create_value(j, TYPE_NULL);
}

Value* builtin_load_plugin(JustState *j, Value **args, int count) {
    if (!(j->capabilities & JUST_CAP_PLUGIN)) { just_error(j, "Plugin loading disabled for this interpreter"); return create_bool(j, false); }
    if (count < 1 || args[0]->type != TYPE_STRING) return create_bool(j, false);
    char *name = args[0]->data.string;
    char fn[MAX_STRING];

    plugin_lock_acquire();

    plugin_state = j;

#ifdef _WIN32
    snprintf(fn, MAX_STRING, "%s.dll", name);
    HMODULE h = LoadLibraryA(fn);
    if (!h) {
        plugin_state = NULL;
        plugin_lock_release();
        return create_bool(j, false);
    }
    void (*init)(RegisterFunc) = (void(*)(RegisterFunc))GetProcAddress(h, "init_plugin");
    if (!init) {
        FreeLibrary(h);
        plugin_state = NULL;
        plugin_lock_release();
        return create_bool(j, false);
    }
    init(plugin_adapter);
#else
    snprintf(fn, MAX_STRING, "./%s.so", name);
    void *h = dlopen(fn, RTLD_NOW);
    if (!h) {
        plugin_state = NULL;
        plugin_lock_release();
        return create_bool(j, false);
    }
    void (*init)(RegisterFunc) = (void(*)(RegisterFunc))dlsym(h, "init_plugin");
    if (!init) {
        dlclose(h);
        plugin_state = NULL;
        plugin_lock_release();
        return create_bool(j, false);
    }
    init(plugin_adapter);
#endif

    j->loaded_plugins = realloc(j->loaded_plugins, sizeof(void*) * (j->loaded_plugin_count + 1));
    j->loaded_plugins[j->loaded_plugin_count++] = h;

    plugin_state = NULL;
    plugin_lock_release();
    return create_bool(j, true);
}

Value* builtin_pow(JustState *j, Value **args, int count) {
    if (count < 2) return create_number(j, 0);
    return create_number(j, pow(value_to_number(args[0]), value_to_number(args[1])));
}

Value* builtin_min(JustState *j, Value **args, int count) {
    if (count < 2) return create_number(j, 0);
    double a = value_to_number(args[0]), b = value_to_number(args[1]);
    return create_number(j, a < b ? a : b);
}

Value* builtin_max(JustState *j, Value **args, int count) {
    if (count < 2) return create_number(j, 0);
    double a = value_to_number(args[0]), b = value_to_number(args[1]);
    return create_number(j, a > b ? a : b);
}

Value* builtin_abs(JustState *j, Value **args, int count) {
    if (count < 1) return create_number(j, 0);
    double v = value_to_number(args[0]);
    return create_number(j, v < 0 ? -v : v);
}

Value* builtin_join(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_ARRAY || args[1]->type != TYPE_STRING)
        return create_string(j, "");
    Value *a = args[0];
    char *d = args[1]->data.string;
    char buf[MAX_STRING] = "";
    for (int i = 0; i < a->data.array.count; i++) {
        if (i > 0) strcat(buf, d);
        char *s = value_to_string_raw(a->data.array.items[i]);
        strcat(buf, s);
        free(s);
    }
    return create_string(j, buf);
}

Value* builtin_trim(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
    char *s = str_dup(args[0]->data.string);
    char *start = s;
    while (*start == ' ') start++;
    if (*start == '\0') {
        free(s);
        return create_string(j, "");
    }
    char *end = start + strlen(start) - 1;
    while (end > start && *end == ' ') end--;
    *(end+1) = '\0';
    Value *v = create_string(j, start);
    free(s);
    return v;
}

Value* builtin_read_json(JustState *j, Value **args, int count) {
    if (!(j->capabilities & JUST_CAP_FILES)) { just_error(j, "File access disabled for this interpreter"); return create_value(j, TYPE_NULL); }
    return builtin_import_json(j, args, count);
}

Value* builtin_write_json(JustState *j, Value **args, int count) {
    if (!(j->capabilities & JUST_CAP_FILES)) { just_error(j, "File access disabled for this interpreter"); return create_bool(j, false); }
    if (count < 2 || args[0]->type != TYPE_STRING) return create_bool(j, false);
    return builtin_json_export(j, args, count);
}

Value* builtin_exists(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_bool(j, false);
    FILE *f = fopen(args[0]->data.string, "r");
    if (f) {
        fclose(f);
        return create_bool(j, true);
    }
    return create_bool(j, false);
}

Value* builtin_contains(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_bool(j, false);
    return create_bool(j, strstr(args[0]->data.string, args[1]->data.string) != NULL);
}

Value* builtin_starts_with(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_bool(j, false);

    char *str = args[0]->data.string;
    char *prefix = args[1]->data.string;
    int len = strlen(prefix);

    return create_bool(j, strncmp(str, prefix, len) == 0);
}

Value* builtin_ends_with(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING)
        return create_bool(j, false);

    char *str = args[0]->data.string;
    char *suffix = args[1]->data.string;
    int str_len = strlen(str);
    int suffix_len = strlen(suffix);

    if (suffix_len > str_len) return create_bool(j, false);
    return create_bool(j, strcmp(str + str_len - suffix_len, suffix) == 0);
}

Value* builtin_repeat(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_STRING)
        return create_string(j, "");

    char *str = args[0]->data.string;
    int n = (int)value_to_number(args[1]);
    if (n < 0) n = 0;
    if (n > 10000) n = 10000;

    int len = strlen(str);
    char *result = malloc(len * n + 1);
    if (!result) return create_string(j, "");

    result[0] = '\0';
    for (int i = 0; i < n; i++) {
        strcat(result, str);
    }

    Value *v = create_string(j, result);
    free(result);
    return v;
}

Value* builtin_is_number(JustState *j, Value **args, int count) {
    if (count < 1) return create_bool(j, false);
    return create_bool(j, args[0]->type == TYPE_NUMBER);
}

Value* builtin_is_string(JustState *j, Value **args, int count) {
    if (count < 1) return create_bool(j, false);
    return create_bool(j, args[0]->type == TYPE_STRING);
}

Value* builtin_is_bool(JustState *j, Value **args, int count) {
    if (count < 1) return create_bool(j, false);
    return create_bool(j, args[0]->type == TYPE_BOOL);
}

Value* builtin_is_array(JustState *j, Value **args, int count) {
    if (count < 1) return create_bool(j, false);
    return create_bool(j, args[0]->type == TYPE_ARRAY);
}

Value* builtin_is_object(JustState *j, Value **args, int count) {
    if (count < 1) return create_bool(j, false);
    return create_bool(j, args[0]->type == TYPE_OBJECT);
}

Value* builtin_is_function(JustState *j, Value **args, int count) {
    if (count < 1) return create_bool(j, false);
    return create_bool(j, args[0]->type == TYPE_FUNCTION || args[0]->type == TYPE_NATIVE_FUNC);
}

Value* builtin_is_null(JustState *j, Value **args, int count) {
    if (count < 1) return create_bool(j, false);
    return create_bool(j, args[0]->type == TYPE_NULL);
}

Value* builtin_array_push(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_ARRAY) return create_value(j, TYPE_NULL);
    array_push(args[0], args[1]);
    return args[0];
}

Value* builtin_array_pop(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_ARRAY || args[0]->data.array.count == 0)
        return create_value(j, TYPE_NULL);
    Value *last = args[0]->data.array.items[args[0]->data.array.count - 1];
    args[0]->data.array.count--;
    return last;
}

Value* builtin_first(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_ARRAY || args[0]->data.array.count == 0)
        return create_value(j, TYPE_NULL);

    args[0]->data.array.items[0]->marked = true;
    return args[0]->data.array.items[0];
}

Value* builtin_last(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_ARRAY || args[0]->data.array.count == 0)
        return create_value(j, TYPE_NULL);

    int last = args[0]->data.array.count - 1;
    args[0]->data.array.items[last]->marked = true;
    return args[0]->data.array.items[last];
}

Value* builtin_reverse(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_ARRAY)
        return create_array(j);

    Value *arr = args[0];
    Value *result = create_array(j);

    for (int i = arr->data.array.count - 1; i >= 0; i--) {
        array_push(result, arr->data.array.items[i]);
    }

    return result;
}

Value* builtin_sort(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_ARRAY)
        return create_array(j);

    Value *arr = args[0];
    Value *result = create_array(j);

    for (int i = 0; i < arr->data.array.count; i++) {
        array_push(result, arr->data.array.items[i]);
    }

    bool all_numbers = true;
    for (int i = 0; i < result->data.array.count; i++) {
        if (result->data.array.items[i]->type != TYPE_NUMBER) {
            all_numbers = false;
            break;
        }
    }

    if (all_numbers && result->data.array.count > 1) {
        for (int i = 0; i < result->data.array.count - 1; i++) {
            for (int j = 0; j < result->data.array.count - i - 1; j++) {
                double a = value_to_number(result->data.array.items[j]);
                double b = value_to_number(result->data.array.items[j + 1]);
                if (a > b) {
                    Value *temp = result->data.array.items[j];
                    result->data.array.items[j] = result->data.array.items[j + 1];
                    result->data.array.items[j + 1] = temp;
                }
            }
        }
    }

    return result;
}

Value* builtin_dirname(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING)
        return create_string(j, "");

    char *path = args[0]->data.string;
    char *last_slash = strrchr(path, '/');
#ifdef _WIN32
    char *last_backslash = strrchr(path, '\\');
    if (last_backslash > last_slash) last_slash = last_backslash;
#endif

    if (!last_slash) return create_string(j, ".");

    int len = last_slash - path;
    char *result = malloc(len + 1);
    strncpy(result, path, len);
    result[len] = '\0';

    Value *v = create_string(j, result);
    free(result);
    return v;
}

Value* builtin_basename(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING)
        return create_string(j, "");

    char *path = args[0]->data.string;
    char *last_slash = strrchr(path, '/');
#ifdef _WIN32
    char *last_backslash = strrchr(path, '\\');
    if (last_backslash > last_slash) last_slash = last_backslash;
#endif

    if (!last_slash) return create_string(j, path);
    return create_string(j, last_slash + 1);
}

Value* builtin_extname(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING)
        return create_string(j, "");

    char *path = args[0]->data.string;
    char *last_slash = strrchr(path, '/');
#ifdef _WIN32
    char *last_backslash = strrchr(path, '\\');
    if (last_backslash > last_slash) last_slash = last_backslash;
#endif

    char *start = last_slash ? last_slash + 1 : path;
    char *dot = strrchr(start, '.');

    if (!dot || dot == start) return create_string(j, "");
    return create_string(j, dot);
}

Value* builtin_join_path(JustState *j, Value **args, int count) {
    if (count < 1) return create_string(j, "");

    char result[MAX_STRING] = "";

    for (int i = 0; i < count; i++) {
        if (args[i]->type != TYPE_STRING) continue;
        char *part = args[i]->data.string;

        if (i > 0 && result[0] != '\0') {
            int len = strlen(result);
#ifdef _WIN32
            if (result[len-1] != '\\' && result[len-1] != '/') {
                strcat(result, "\\");
            }
#else
            if (result[len-1] != '/') {
                strcat(result, "/");
            }
#endif
        }
        strcat(result, part);
    }

    return create_string(j, result);
}

Value* builtin_help(JustState *j, Value **args, int count) {
    if (count < 1) {
        printf("Available functions:\n");
        printf("  ");
        int printed = 0;
        for (int i = 0; i < j->func_count; i++) {
            printf("%s", j->funcs[i].name);
            printed++;
            if (i < j->func_count - 1) {
                if (printed % 8 == 0) {
                    printf("\n  ");
                } else {
                    printf(", ");
                }
            }
        }
        printf("\n\nTotal: %d functions\n", j->func_count);
    } else if (args[0]->type == TYPE_STRING) {
        Function *f = find_func(j, args[0]->data.string);
        if (f) {
            printf("Function: %s\n", f->name);
            if (f->is_native) {
                printf("  Type: native (built-in)\n");
            } else {
                printf("  Type: user-defined\n");
                printf("  Params: %d\n", f->param_count);
            }
        } else {
            printf("Function '%s' not found\n", args[0]->data.string);
        }
    }
    return create_value(j, TYPE_NULL);
}

Value* builtin_debug(JustState *j, Value **args, int count) {
    printf("DEBUG: ");
    for (int i = 0; i < count; i++) {
        char *s = value_to_string_raw(args[i]);
        printf("%s", s);
        free(s);
        if (i < count - 1) printf(" ");
    }
    printf("\n");
    return create_value(j, TYPE_NULL);
}

Value* builtin_dump(JustState *j, Value **args, int count) {
    if (count < 1) return create_value(j, TYPE_NULL);
    char *s = value_to_string_raw(args[0]);
    printf("DUMP: %s\n", s);
    free(s);
    return create_value(j, TYPE_NULL);
}

#ifndef JUST_NO_SQLITE
Value* builtin_db_open(JustState *j, Value **args, int count) {

    if (!(j->capabilities & JUST_CAP_DB)) {
        just_error(j, "Database access disabled for this interpreter");
        return create_value(j, TYPE_NULL);
    }
    if (count < 1 || args[0]->type != TYPE_STRING) {
        return create_value(j, TYPE_NULL);
    }

    sqlite3 *db;
    const char *filename = args[0]->data.string;

    int rc = sqlite3_open(filename, &db);
    if (rc != SQLITE_OK) {
        just_error(j, "Cannot open database");
        return create_value(j, TYPE_NULL);
    }

    return create_number(j, handle_alloc(j, db));
}

Value* builtin_db_close(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_value(j, TYPE_NULL);
    }

    int hid = (int)value_to_number(args[0]);
    sqlite3 *db = (sqlite3*)handle_get(j, hid);
    if (db) sqlite3_close(db);
    handle_free(j, hid);
    return create_value(j, TYPE_NULL);
}

static int db_callback(void *data, int argc, char **argv, char **azColName) {
    DbCallbackCtx *ctx = (DbCallbackCtx*)data;
    Value *row = create_object(ctx->j);

    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            object_set(row, azColName[i], create_string(ctx->j, argv[i]));
        } else {
            object_set(row, azColName[i], create_value(ctx->j, TYPE_NULL));
        }
    }

    array_push(ctx->arr, row);
    return 0;
}

Value* builtin_db_query(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_NUMBER || args[1]->type != TYPE_STRING) {
        return create_value(j, TYPE_NULL);
    }

    sqlite3 *db = (sqlite3*)handle_get(j, (int)value_to_number(args[0]));
    const char *sql = args[1]->data.string;
    char *err = NULL;

    if (strstr(sql, "SELECT") || strstr(sql, "select")) {
        Value *result = create_array(j);

        struct {
            JustState *j;
            Value *arr;
        } ctx = {j, result};

        int rc = sqlite3_exec(db, sql, db_callback, &ctx, &err);

        if (rc != SQLITE_OK) {
            just_error(j, err);
            sqlite3_free(err);
            return create_value(j, TYPE_NULL);
        }

        return result;
    }

    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        just_error(j, err);
        sqlite3_free(err);
        return create_bool(j, false);
    }

    return create_bool(j, true);
}

Value* builtin_db_exec(JustState *j, Value **args, int count) {
    return builtin_db_query(j, args, count);
}

Value* builtin_db_prepare(JustState *j, Value **args, int count) {
    if (count < 2 || args[0]->type != TYPE_NUMBER || args[1]->type != TYPE_STRING) {
        return create_value(j, TYPE_NULL);
    }

    sqlite3 *db = (sqlite3*)handle_get(j, (int)value_to_number(args[0]));
    sqlite3_stmt *stmt;
    const char *sql = args[1]->data.string;

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        just_error(j, sqlite3_errmsg(db));
        return create_value(j, TYPE_NULL);
    }

    return create_number(j, handle_alloc(j, stmt));
}

Value* builtin_db_bind(JustState *j, Value **args, int count) {
    if (count < 3 || args[0]->type != TYPE_NUMBER) {
        return create_bool(j, false);
    }

    sqlite3_stmt *stmt = (sqlite3_stmt*)handle_get(j, (int)value_to_number(args[0]));
    int idx = (int)value_to_number(args[1]);
    Value *val = args[2];

    int rc;
    switch (val->type) {
        case TYPE_NULL:
            rc = sqlite3_bind_null(stmt, idx);
            break;
        case TYPE_NUMBER:
            rc = sqlite3_bind_double(stmt, idx, val->data.number);
            break;
        case TYPE_STRING:
            rc = sqlite3_bind_text(stmt, idx, val->data.string, -1, SQLITE_STATIC);
            break;
        case TYPE_BOOL:
            rc = sqlite3_bind_int(stmt, idx, val->data.boolean ? 1 : 0);
            break;
        default:
            return create_bool(j, false);
    }

    return create_bool(j, rc == SQLITE_OK);
}

Value* builtin_db_step(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_value(j, TYPE_NULL);
    }

    sqlite3_stmt *stmt = (sqlite3_stmt*)handle_get(j, (int)value_to_number(args[0]));
    int rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
        int cols = sqlite3_column_count(stmt);
        Value *row = create_object(j);

        for (int i = 0; i < cols; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            int type = sqlite3_column_type(stmt, i);

            switch (type) {
                case SQLITE_INTEGER:
                    object_set(row, name, create_number(j, sqlite3_column_int(stmt, i)));
                    break;
                case SQLITE_FLOAT:
                    object_set(row, name, create_number(j, sqlite3_column_double(stmt, i)));
                    break;
                case SQLITE_TEXT:
                    object_set(row, name, create_string(j, (const char*)sqlite3_column_text(stmt, i)));
                    break;
                case SQLITE_NULL:
                    object_set(row, name, create_value(j, TYPE_NULL));
                    break;
                default:
                    object_set(row, name, create_value(j, TYPE_NULL));
            }
        }
        return row;
    }

    if (rc == SQLITE_DONE) {
        return create_value(j, TYPE_NULL);
    }

    return create_value(j, TYPE_NULL);
}

Value* builtin_db_finalize(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_value(j, TYPE_NULL);
    }

    int hid = (int)value_to_number(args[0]);
    sqlite3_stmt *stmt = (sqlite3_stmt*)handle_get(j, hid);
    if (stmt) sqlite3_finalize(stmt);
    handle_free(j, hid);
    return create_value(j, TYPE_NULL);
}

Value* builtin_db_last_insert_id(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_number(j, 0);
    }

    sqlite3 *db = (sqlite3*)handle_get(j, (int)value_to_number(args[0]));
    return create_number(j, sqlite3_last_insert_rowid(db));
}

Value* builtin_db_changes(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_number(j, 0);
    }

    sqlite3 *db = (sqlite3*)handle_get(j, (int)value_to_number(args[0]));
    return create_number(j, sqlite3_changes(db));
}

Value* builtin_db_begin(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_bool(j, false);
    }

    sqlite3 *db = (sqlite3*)handle_get(j, (int)value_to_number(args[0]));
    char *err = NULL;
    int rc = sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, &err);

    if (rc != SQLITE_OK) {
        just_error(j, err);
        sqlite3_free(err);
        return create_bool(j, false);
    }

    return create_bool(j, true);
}

Value* builtin_db_commit(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_bool(j, false);
    }

    sqlite3 *db = (sqlite3*)handle_get(j, (int)value_to_number(args[0]));
    char *err = NULL;
    int rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &err);

    if (rc != SQLITE_OK) {
        just_error(j, err);
        sqlite3_free(err);
        return create_bool(j, false);
    }

    return create_bool(j, true);
}

Value* builtin_db_rollback(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_bool(j, false);
    }

    sqlite3 *db = (sqlite3*)handle_get(j, (int)value_to_number(args[0]));
    char *err = NULL;
    int rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, &err);

    if (rc != SQLITE_OK) {
        just_error(j, err);
        sqlite3_free(err);
        return create_bool(j, false);
    }

    return create_bool(j, true);
}

Value* builtin_db_error(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_string(j, "");
    }

    sqlite3 *db = (sqlite3*)handle_get(j, (int)value_to_number(args[0]));
    const char *err = sqlite3_errmsg(db);
    return create_string(j, err ? err : "");
}
#endif

Value* builtin_gc_get_count(JustState *j, Value **args, int count) {
    (void)args; (void)count;
    return create_number(j, j->gc_count);
}

Value* builtin_gc_get_allocations(JustState *j, Value **args, int count) {
    (void)args; (void)count;
    return create_number(j, j->total_allocations);
}

void add_native_func(JustState *j, const char *name, NativeFunc func) {
    if (!j) return;
    for (int i = 0; i < j->func_count; i++)
        if (strcmp(j->funcs[i].name, name) == 0) return;
    if (j->func_count >= j->func_capacity) {
        j->func_capacity = j->func_capacity ? j->func_capacity * 2 : 16;
        j->funcs = realloc(j->funcs, sizeof(Function) * j->func_capacity);
    }
    j->funcs[j->func_count].name = str_dup(name);
    j->funcs[j->func_count].params = NULL;
    j->funcs[j->func_count].param_count = 0;
    j->funcs[j->func_count].body_start = 0;
    j->funcs[j->func_count].body_end = 0;
    j->funcs[j->func_count].is_native = true;
    j->funcs[j->func_count].native_func = func;
    j->func_count++;
}

void register_builtins(JustState *j) {
	if (j->builtins_registered) return;
	j->builtins_registered = true;

    add_native_func(j, "print", builtin_print);
    add_native_func(j, "type", builtin_type);
    add_native_func(j, "len", builtin_len);
    add_native_func(j, "input", builtin_input);
    add_native_func(j, "int", builtin_int);
    add_native_func(j, "str", builtin_str);
    add_native_func(j, "bool", builtin_bool);
    add_native_func(j, "range", builtin_range);
    add_native_func(j, "keys", builtin_keys);
    add_native_func(j, "values", builtin_values);
    add_native_func(j, "has", builtin_has);
    add_native_func(j, "read", builtin_read_file);
    add_native_func(j, "write", builtin_write_file);
    add_native_func(j, "json", builtin_json);
    add_native_func(j, "export", builtin_json_export);
    add_native_func(j, "http_get", builtin_http_get);
    add_native_func(j, "import_json", builtin_import_json);
    add_native_func(j, "split", builtin_split);
    add_native_func(j, "upper", builtin_upper);
    add_native_func(j, "lower", builtin_lower);
    add_native_func(j, "replace", builtin_replace);
    add_native_func(j, "regex_match", builtin_regex_match);
    add_native_func(j, "regex_find", builtin_regex_find);
    add_native_func(j, "regex_find_all", builtin_regex_find_all);
    add_native_func(j, "regex_replace", builtin_regex_replace);
    add_native_func(j, "sqrt", builtin_sqrt);
    add_native_func(j, "random", builtin_random);
	add_native_func(j, "sin", builtin_sin);
	add_native_func(j, "cos", builtin_cos);
	add_native_func(j, "tan", builtin_tan);
	add_native_func(j, "log", builtin_log);
	add_native_func(j, "exp", builtin_exp);
	declare_var(j, "PI", create_number(j, 3.141592653589793), true);
    declare_var(j, "E", create_number(j, 2.718281828459045), true);
    add_native_func(j, "floor", builtin_floor);
    add_native_func(j, "ceil", builtin_ceil);
    add_native_func(j, "round", builtin_round);
    add_native_func(j, "now", builtin_now);
    add_native_func(j, "http_post", builtin_http_post);
    add_native_func(j, "exec", builtin_exec);
    add_native_func(j, "sleep", builtin_sleep);
    add_native_func(j, "env", builtin_env);
    add_native_func(j, "red", builtin_red);
    add_native_func(j, "green", builtin_green);
    add_native_func(j, "yellow", builtin_yellow);
    add_native_func(j, "blue", builtin_blue);
    add_native_func(j, "magenta", builtin_magenta);
    add_native_func(j, "cyan", builtin_cyan);
    add_native_func(j, "bold", builtin_bold);
    add_native_func(j, "json_parse", builtin_json_parse);
    add_native_func(j, "load_plugin", builtin_load_plugin);
    add_native_func(j, "filter", builtin_filter);
    add_native_func(j, "map", builtin_map);
    add_native_func(j, "reduce", builtin_reduce);
    add_native_func(j, "find", builtin_find);
    add_native_func(j, "index_of", builtin_index_of);
    add_native_func(j, "includes", builtin_includes);
    add_native_func(j, "slice", builtin_slice);
    add_native_func(j, "concat", builtin_concat);
    add_native_func(j, "unique", builtin_unique);
    add_native_func(j, "sum", builtin_sum);
    add_native_func(j, "clamp", builtin_clamp);
    add_native_func(j, "pad_start", builtin_pad_start);
    add_native_func(j, "pad_end", builtin_pad_end);
    add_native_func(j, "merge", builtin_merge);
    add_native_func(j, "entries", builtin_entries);
    add_native_func(j, "task", builtin_task);
    add_native_func(j, "watch", builtin_watch);
    add_native_func(j, "error", builtin_error);
    add_native_func(j, "pow", builtin_pow);
    add_native_func(j, "min", builtin_min);
    add_native_func(j, "max", builtin_max);
    add_native_func(j, "abs", builtin_abs);
    add_native_func(j, "trim", builtin_trim);
    add_native_func(j, "contains", builtin_contains);
    add_native_func(j, "join", builtin_join);
    add_native_func(j, "read_json", builtin_read_json);
    add_native_func(j, "write_json", builtin_write_json);
    add_native_func(j, "exists", builtin_exists);
    add_native_func(j, "array_push", builtin_array_push);
    add_native_func(j, "array_pop", builtin_array_pop);
	add_native_func(j, "is_number", builtin_is_number);
	add_native_func(j, "is_string", builtin_is_string);
	add_native_func(j, "is_bool", builtin_is_bool);
	add_native_func(j, "is_array", builtin_is_array);
	add_native_func(j, "is_object", builtin_is_object);
	add_native_func(j, "is_function", builtin_is_function);
	add_native_func(j, "is_null", builtin_is_null);
	add_native_func(j, "debug", builtin_debug);
	add_native_func(j, "help", builtin_help);
	add_native_func(j, "dump", builtin_dump);
	add_native_func(j, "dirname", builtin_dirname);
	add_native_func(j, "basename", builtin_basename);
	add_native_func(j, "extname", builtin_extname);
	add_native_func(j, "join_path", builtin_join_path);
	add_native_func(j, "starts_with", builtin_starts_with);
	add_native_func(j, "ends_with", builtin_ends_with);
	add_native_func(j, "repeat", builtin_repeat);
	add_native_func(j, "first", builtin_first);
	add_native_func(j, "last", builtin_last);
	add_native_func(j, "reverse", builtin_reverse);
	add_native_func(j, "sort", builtin_sort);
#ifndef JUST_NO_SQLITE
	add_native_func(j, "db_open", builtin_db_open);
    add_native_func(j, "db_close", builtin_db_close);
    add_native_func(j, "db_query", builtin_db_query);
    add_native_func(j, "db_exec", builtin_db_exec);
	add_native_func(j, "db_prepare", builtin_db_prepare);
	add_native_func(j, "db_bind", builtin_db_bind);
	add_native_func(j, "db_step", builtin_db_step);
	add_native_func(j, "db_finalize", builtin_db_finalize);
	add_native_func(j, "db_last_insert_id", builtin_db_last_insert_id);
	add_native_func(j, "db_changes", builtin_db_changes);
	add_native_func(j, "db_begin", builtin_db_begin);
	add_native_func(j, "db_commit", builtin_db_commit);
	add_native_func(j, "db_rollback", builtin_db_rollback);
	add_native_func(j, "db_error", builtin_db_error);
#endif
	add_native_func(j, "substr", builtin_substr);
	add_native_func(j, "gc_get_count", builtin_gc_get_count);
	add_native_func(j, "gc_get_allocations", builtin_gc_get_allocations);
}

void tokenize(JustState *j, const char *src) {
    j->current_line = 1;
    if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
        src += 3;
    if (j->tokens) {
        for (int i = 0; i < j->token_count; i++) free(j->tokens[i]);
        free(j->tokens);
    }
    free(j->token_lines);
    j->tokens = NULL;
    j->token_count = 0;
    j->token_capacity = 256;
    j->tokens = malloc(sizeof(char*) * j->token_capacity);
    j->token_lines = malloc(sizeof(int) * j->token_capacity);
    const char *p = src;
    while (*p) {
        while (*p && isspace(*p)) {
            if (*p == '\n') j->current_line++;
            p++;
        }
        if (!*p) break;
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        if (*p == '"' || *p == '\'') {
            char q = *p++;
            char *s = malloc(MAX_STRING);
            int l = 0;
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
            s[l] = '\0';
            if (*p == q) p++;
            char *t = malloc(l + 3);
            t[0] = '"';
            strcpy(t + 1, s);
            t[l + 1] = '"';
            t[l + 2] = '\0';
            free(s);
            if (j->token_count >= j->token_capacity) {
                j->token_capacity *= 2;
                j->token_lines = realloc(j->token_lines, sizeof(int) * j->token_capacity);
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity);
            }
            if (j->token_count >= j->max_tokens) {

                j->token_limit_hit = true;
                return;
            }
            j->token_lines[j->token_count] = j->current_line;
            j->tokens[j->token_count++] = t;
            continue;
        }

        if (p[0]=='=' && p[1]=='=' && p[2]=='=') {
            char *t = malloc(4);
            t[0]='='; t[1]='='; t[2]='='; t[3]='\0';
            if (j->token_count >= j->token_capacity) {
                j->token_capacity *= 2;
                j->token_lines = realloc(j->token_lines, sizeof(int) * j->token_capacity);
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity);
            }
            if (j->token_count >= j->max_tokens) {

                j->token_limit_hit = true;
                return;
            }
            j->token_lines[j->token_count] = j->current_line;
            j->tokens[j->token_count++] = t;
            p += 3;
            continue;
        }
        if (p[0]=='!' && p[1]=='=' && p[2]=='=') {
            char *t = malloc(4);
            t[0]='!'; t[1]='='; t[2]='='; t[3]='\0';
            if (j->token_count >= j->token_capacity) {
                j->token_capacity *= 2;
                j->token_lines = realloc(j->token_lines, sizeof(int) * j->token_capacity);
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity);
            }
            if (j->token_count >= j->max_tokens) {

                j->token_limit_hit = true;
                return;
            }
            j->token_lines[j->token_count] = j->current_line;
            j->tokens[j->token_count++] = t;
            p += 3;
            continue;
        }

        if (p[0]=='*' && p[1]=='*') {
            char *t = malloc(3);
            t[0]='*'; t[1]='*'; t[2]='\0';
            if (j->token_count >= j->token_capacity) {
                j->token_capacity *= 2;
                j->token_lines = realloc(j->token_lines, sizeof(int) * j->token_capacity);
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity);
            }
            if (j->token_count >= j->max_tokens) {

                j->token_limit_hit = true;
                return;
            }
            j->token_lines[j->token_count] = j->current_line;
            j->tokens[j->token_count++] = t;
            p += 2;
            continue;
        }

        if ((p[0]=='&'&&p[1]=='&')||(p[0]=='|'&&p[1]=='|')) {
            char *t = malloc(3);
            t[0]=p[0]; t[1]=p[1]; t[2]='\0';
            if (j->token_count >= j->token_capacity) {
                j->token_capacity *= 2;
                j->token_lines = realloc(j->token_lines, sizeof(int) * j->token_capacity);
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity);
            }
            if (j->token_count >= j->max_tokens) {

                j->token_limit_hit = true;
                return;
            }
            j->token_lines[j->token_count] = j->current_line;
            j->tokens[j->token_count++] = t;
            p += 2;
            continue;
        }

        if ((p[0]=='='&&p[1]=='=')||(p[0]=='!'&&p[1]=='=')||(p[0]=='<'&&p[1]=='=')||(p[0]=='>'&&p[1]=='=')||(p[0]=='+'&&p[1]=='=')||(p[0]=='-'&&p[1]=='=')) {
            char *t = malloc(3);
            t[0]=p[0]; t[1]=p[1]; t[2]='\0';
            if (j->token_count >= j->token_capacity) {
                j->token_capacity *= 2;
                j->token_lines = realloc(j->token_lines, sizeof(int) * j->token_capacity);
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity);
            }
            if (j->token_count >= j->max_tokens) {

                j->token_limit_hit = true;
                return;
            }
            j->token_lines[j->token_count] = j->current_line;
            j->tokens[j->token_count++] = t;
            p += 2;
            continue;
        }

        if (strchr("(){}[]:,;=+*/<>.!@#$%^&|~?-", *p)) {
            char *t = malloc(2);
            t[0]=*p; t[1]='\0';
            if (j->token_count >= j->token_capacity) {
                j->token_capacity *= 2;
                j->token_lines = realloc(j->token_lines, sizeof(int) * j->token_capacity);
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity);
            }
            if (j->token_count >= j->max_tokens) {

                j->token_limit_hit = true;
                return;
            }
            j->token_lines[j->token_count] = j->current_line;
            j->tokens[j->token_count++] = t;
            p++;
            continue;
        }

        if (isalpha(*p) || *p == '_' || isdigit(*p)) {
            const char *st = p;
            if (isdigit(*p)) {
                while (*p && isdigit(*p)) p++;
                if (*p == '.' && isdigit(*(p+1))) {
                    p++;
                    while (*p && isdigit(*p)) p++;
                }
            } else while (*p && (isalnum(*p) || *p == '_')) p++;
            int l = p - st;
            char *t = malloc(l + 1);
            strncpy(t, st, l);
            t[l] = '\0';
            if (j->token_count >= j->token_capacity) {
                j->token_capacity *= 2;
                j->token_lines = realloc(j->token_lines, sizeof(int) * j->token_capacity);
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity);
            }
            if (j->token_count >= j->max_tokens) {

                j->token_limit_hit = true;
                return;
            }
            j->token_lines[j->token_count] = j->current_line;
            j->tokens[j->token_count++] = t;
            continue;
        }
        p++;
    }
}

Value* eval_primary(JustState *j, int *pos) {
    if (*pos >= j->token_count) return create_value(j, TYPE_NULL);
    char *t = j->tokens[*pos];

    if (strcmp(t, "null") == 0) {
        (*pos)++;
        return create_value(j, TYPE_NULL);
    }
    if (strcmp(t, "true") == 0) {
        (*pos)++;
        return create_bool(j, true);
    }
    if (strcmp(t, "false") == 0) {
        (*pos)++;
        return create_bool(j, false);
    }

    if (t[0] == '"' || t[0] == '\'') {
        (*pos)++;
        int l = strlen(t);
        char *s = malloc(l);
        int jj = 0;
        for (int i = 1; i < l - 1; i++) {
            if (t[i] == '\\' && i + 1 < l - 1) {
                i++;
                switch (t[i]) {
                    case 'n': s[jj++]='\n'; break;
                    case 't': s[jj++]='\t'; break;
                    case 'r': s[jj++]='\r'; break;
                    case '\\': s[jj++]='\\'; break;
                    case '"': s[jj++]='"'; break;
                    default: s[jj++]='\\'; s[jj++]=t[i];
                }
            } else s[jj++] = t[i];
        }
        s[jj] = '\0';
        Value *v = create_string(j, s);
        free(s);
        return v;
    }

    if (isdigit(t[0])) {
        (*pos)++;
        return create_number(j, atof(t));
    }

    if (strcmp(t, "-") == 0) {
        (*pos)++;
        Value *v = eval_primary(j, pos);
        if (v->type == TYPE_NUMBER) {
            v->data.number = -v->data.number;
            return v;
        }
        Value *r = create_number(j, -value_to_number(v));
        return r;
    }

    if (strcmp(t, "[") == 0) {
        (*pos)++;
        Value *a = create_array(j);
        while (*pos < j->token_count && strcmp(j->tokens[*pos], "]") != 0) {
            if (a->data.array.count > 0 && strcmp(j->tokens[*pos], ",") == 0) (*pos)++;
            if (*pos < j->token_count && strcmp(j->tokens[*pos], "]") != 0) {
                Value *i = eval_expression(j, pos);
                array_push(a, i);
            }
        }
        if (*pos < j->token_count) (*pos)++;
        return a;
    }

    if (strcmp(t, "{") == 0) {
        (*pos)++;
        Value *o = create_object(j);
        while (*pos < j->token_count && strcmp(j->tokens[*pos], "}") != 0) {
            if (o->data.object.count > 0 && strcmp(j->tokens[*pos], ",") == 0) (*pos)++;
            if (*pos >= j->token_count || strcmp(j->tokens[*pos], "}") == 0) break;
            char *k = j->tokens[(*pos)++];
            if (*pos < j->token_count && strcmp(j->tokens[*pos], ":") == 0) (*pos)++;
            if (*pos >= j->token_count) break;
            Value *v = eval_expression(j, pos);
            object_set(o, k, v);
        }
        if (*pos < j->token_count) (*pos)++;
        return o;
    }

    if (strcmp(t, "(") == 0) {
        (*pos)++;
        Value *v = eval_expression(j, pos);
        if (*pos < j->token_count && strcmp(j->tokens[*pos], ")") == 0) (*pos)++;
        return v;
    }

    if (strcmp(t, "!") == 0 || strcmp(t, "not") == 0) {
        (*pos)++;
        Value *v = eval_primary(j, pos);
        bool b = value_to_bool(v);
        return create_bool(j, !b);
    }

    if (strcmp(t, "func") == 0 && *pos + 1 < j->token_count && strcmp(j->tokens[*pos + 1], "(") == 0) {

        (*pos)++;
        (*pos)++;
        char *params[MAX_ARGS];
        int pc = 0;
        while (*pos < j->token_count && strcmp(j->tokens[*pos], ")") != 0) {
            if (pc > 0 && strcmp(j->tokens[*pos], ",") == 0) (*pos)++;
            if (*pos < j->token_count && strcmp(j->tokens[*pos], ")") != 0)
                params[pc++] = j->tokens[(*pos)++];
        }
        if (*pos < j->token_count) (*pos)++;
        while (*pos < j->token_count && strcmp(j->tokens[*pos], "{") != 0) (*pos)++;
        if (*pos >= j->token_count) return create_value(j, TYPE_NULL);
        (*pos)++;
        int bs = *pos, d = 1;
        while (*pos < j->token_count && d > 0) {
            if (strcmp(j->tokens[*pos], "{") == 0) d++;
            else if (strcmp(j->tokens[*pos], "}") == 0) d--;
            (*pos)++;
        }
        int be = *pos - 1;

        Value *fv = create_value(j, TYPE_FUNCTION);
        fv->is_lambda = true;
        fv->data.lambda.body_start = bs;
        fv->data.lambda.body_end = be;
        fv->data.lambda.param_count = pc;
        fv->data.lambda.params = malloc(sizeof(char*) * (pc > 0 ? pc : 1));
        for (int i = 0; i < pc; i++) fv->data.lambda.params[i] = str_dup(params[i]);
        fv->data.lambda.captured = capture_scope(j);
        return fv;
    }

    if (isalpha(t[0]) || t[0] == '_') {
        if (*pos + 1 < j->token_count && strcmp(j->tokens[*pos + 1], "(") == 0) {
            char *fn = t;
            *pos += 2;
            Value *args[MAX_ARGS];
            int ac = 0;
            while (*pos < j->token_count && strcmp(j->tokens[*pos], ")") != 0) {
                if (ac > 0 && strcmp(j->tokens[*pos], ",") == 0) (*pos)++;
                if (*pos >= j->token_count || strcmp(j->tokens[*pos], ")") == 0) break;
                args[ac++] = eval_expression(j, pos);
            }
            if (*pos < j->token_count) (*pos)++;
            Function *f = find_func(j, fn);
            if (f) {
				if (f->is_native) {
					Value *r = f->native_func(j, args, ac);
					adopt_into_gc(j, r); 
					return r;
				}
                if (j->call_depth >= j->max_call_depth) {
                    just_error(j, "Maximum call stack size exceeded");
                    free(j->error_message);
                    j->error_message = str_dup("Maximum call stack size exceeded");
                    j->error_value = create_string(j, "Maximum call stack size exceeded");
                    j->current_flow = FLOW_ERROR;
                    return create_value(j, TYPE_NULL);
                }
                j->call_stack[j->call_depth] = f->name;
                j->call_depth++;

                int od = j->scope_depth;
                push_scope(j);
                for (int i = 0; i < f->param_count && i < ac; i++)
                    declare_var(j, f->params[i], args[i], false);
                ControlFlow of = j->current_flow;
                j->current_flow = FLOW_NORMAL;
                j->return_value = NULL;
                execute_block(j, f->body_start, f->body_end);
                if (j->current_flow != FLOW_ERROR) j->current_flow = of;
                Value *saved_return = j->return_value;
                while (j->scope_depth > od) pop_scope(j);
                j->call_depth--;
                Value *r = saved_return ? saved_return : create_value(j, TYPE_NULL);
                if (saved_return) {
                    saved_return = NULL;
                    j->return_value = NULL;
                }
                return r;
            }

            Variable *cv = find_var(j, fn);
            if (cv && cv->value && (cv->value->type == TYPE_FUNCTION || cv->value->type == TYPE_NATIVE_FUNC)) {
                return call_function_value(j, cv->value, args, ac);
            }
            return create_value(j, TYPE_NULL);
        }
        (*pos)++;
		Variable *v = find_var(j, t);
		if (v) return v->value;

		Function *fref = find_func(j, t);
		if (fref) {
			if (fref->is_native) {
				Value *fv = create_value(j, TYPE_NATIVE_FUNC);
				fv->data.native_func = fref->native_func;
				return fv;
			}
			Value *fv = create_value(j, TYPE_FUNCTION);
			fv->data.string = str_dup(t);
			return fv;
		}
		return create_value(j, TYPE_NULL);
    }
    (*pos)++;
    return create_value(j, TYPE_NULL);
}

Value* eval_postfix(JustState *j, int *pos) {
    Value *v = eval_primary(j, pos);
    while (*pos < j->token_count) {
        if (strcmp(j->tokens[*pos], ".") == 0) {
            (*pos)++;
            if (*pos < j->token_count) {
                char *f = j->tokens[*pos];
                (*pos)++;
                if (v->type == TYPE_OBJECT) {
                    Value *fv = object_get(v, f);
                    v = fv ? fv : create_value(j, TYPE_NULL);
                } else {
                    v = create_value(j, TYPE_NULL);
                }
            }
            continue;
        }
        if (strcmp(j->tokens[*pos], "[") == 0) {
            (*pos)++;
            Value *idx = eval_expression(j, pos);
            if (*pos < j->token_count && strcmp(j->tokens[*pos], "]") == 0) (*pos)++;
            if (v->type == TYPE_ARRAY) {
                Value *item = array_get(v, (int)value_to_number(idx));
                v = item;
            } else {
                v = create_value(j, TYPE_NULL);
            }
            continue;
        }
        break;
    }
    return v;
}

Value* eval_multiplicative(JustState *j, int *pos) {
    Value *l = eval_postfix(j, pos);
    while (*pos < j->token_count) {
        char *op = j->tokens[*pos];
        if (strcmp(op,"*") && strcmp(op,"/") && strcmp(op,"%")) break;
        (*pos)++;
        Value *r = eval_postfix(j, pos);
        double a = value_to_number(l), b = value_to_number(r), res = 0;
        if (strcmp(op,"*")==0) res=a*b;
        else if (strcmp(op,"/")==0) {
            if (b == 0) {

                just_error(j, "Division by zero");
                free(j->error_message);
                j->error_message = str_dup("Division by zero");
                j->error_value = create_string(j, "Division by zero");
                j->current_flow = FLOW_ERROR;
                return create_value(j, TYPE_NULL);
            }
            res = a / b;
        }
        else {
            if (b == 0) {
                just_error(j, "Modulo by zero");
                free(j->error_message);
                j->error_message = str_dup("Modulo by zero");
                j->error_value = create_string(j, "Modulo by zero");
                j->current_flow = FLOW_ERROR;
                return create_value(j, TYPE_NULL);
            }
            res = fmod(a, b);
        }
        l = create_number(j, res);
    }
    return l;
}

Value* eval_additive(JustState *j, int *pos) {
    Value *l = eval_multiplicative(j, pos);
    while (*pos < j->token_count) {
        char *op = j->tokens[*pos];
        if (strcmp(op,"+") && strcmp(op,"-")) break;
        (*pos)++;
        Value *r = eval_multiplicative(j, pos);
        if (l->type == TYPE_STRING || r->type == TYPE_STRING) {
            char *ls = value_to_string_raw(l), *rs = value_to_string_raw(r);
            char *res = malloc(strlen(ls)+strlen(rs)+1);
            strcpy(res, ls);
            if (strcmp(op,"+")==0) strcat(res, rs);
            free(ls);
            free(rs);
            l = create_string(j, res);
            free(res);
        } else {
            double a = value_to_number(l), b = value_to_number(r);
            l = create_number(j, strcmp(op,"+")==0?a+b:a-b);
        }
    }
    return l;
}

Value* eval_comparison(JustState *j, int *pos) {
    Value *l = eval_additive(j, pos);
    while (*pos < j->token_count) {
        char *op = j->tokens[*pos];
        if (strcmp(op,"==") && strcmp(op,"!=") && strcmp(op,"<") && strcmp(op,">") &&
            strcmp(op,"<=") && strcmp(op,">=") && strcmp(op,"===") && strcmp(op,"!==")) break;
        (*pos)++;
        Value *r = eval_additive(j, pos);
        bool res = false;
        if (strcmp(op,"===")==0) {

            res = (l->type == r->type && values_equal(l, r));
        } else if (strcmp(op,"!==")==0) {
            res = !(l->type == r->type && values_equal(l, r));
        } else if (strcmp(op,"==")==0) {
            res = values_equal(l, r);
        } else if (strcmp(op,"!=")==0) {
            res = !values_equal(l, r);
        } else if (l->type == TYPE_STRING && r->type == TYPE_STRING) {

            int c = strcmp(l->data.string, r->data.string);
            if (strcmp(op,"<")==0) res = c < 0;
            else if (strcmp(op,">")==0) res = c > 0;
            else if (strcmp(op,"<=")==0) res = c <= 0;
            else res = c >= 0;
        } else {
            double a = value_to_number(l), b = value_to_number(r);
            if (strcmp(op,"<")==0) res=a<b;
            else if (strcmp(op,">")==0) res=a>b;
            else if (strcmp(op,"<=")==0) res=a<=b;
            else res=a>=b;
        }
        l = create_bool(j, res);
    }
    return l;
}

Value* eval_logical_and(JustState *j, int *pos) {
    Value *l = eval_comparison(j, pos);
    while (*pos < j->token_count && (strcmp(j->tokens[*pos],"and")==0 || strcmp(j->tokens[*pos],"&&")==0)) {
        (*pos)++;
        Value *r = eval_comparison(j, pos);
        bool b = value_to_bool(l) && value_to_bool(r);
        l = create_bool(j, b);
    }
    return l;
}

Value* eval_logical_or(JustState *j, int *pos) {
    Value *l = eval_logical_and(j, pos);
    while (*pos < j->token_count && (strcmp(j->tokens[*pos],"or")==0 || strcmp(j->tokens[*pos],"||")==0)) {
        (*pos)++;
        Value *r = eval_logical_and(j, pos);
        bool b = value_to_bool(l) || value_to_bool(r);
        l = create_bool(j, b);
    }
    return l;
}

Value* eval_expression(JustState *j, int *pos) {
    return eval_logical_or(j, pos);
}

Value* just_number(double n) {
    Value *v = calloc(1, sizeof(Value));
    if (!v) return NULL;
    v->type = TYPE_NUMBER;
    v->data.number = n;
    v->marked = true;
    return v;
}
Value* just_string(const char *s) {
    Value *v = calloc(1, sizeof(Value));
    if (!v) return NULL;
    v->type = TYPE_STRING;
    v->data.string = s ? str_dup(s) : str_dup("");
    v->marked = true;
    return v;
}
Value* just_bool(bool b) {
    Value *v = calloc(1, sizeof(Value));
    if (!v) return NULL;
    v->type = TYPE_BOOL;
    v->data.boolean = b;
    v->marked = true;
    return v;
}
Value* just_null(void) {
    Value *v = calloc(1, sizeof(Value));
    if (!v) return NULL;
    v->type = TYPE_NULL;
    v->marked = true;
    return v;
}
double just_as_number(Value *v) { return value_to_number(v); }
const char* just_as_string(Value *v) {
    return v && v->type == TYPE_STRING ? v->data.string : "";
}
bool just_as_bool(Value *v) { return value_to_bool(v); }

int execute_block(JustState *j, int start, int end) {
    int pos = start, od = j->scope_depth;
    while (pos < end && pos < j->token_count && j->current_flow == FLOW_NORMAL)
        pos = execute_statement(j, pos);
    while (j->scope_depth > od) pop_scope(j);
    return pos;
}

int tokenize_append(JustState *j, const char *src) {
    char **old_tokens = j->tokens;
    int *old_lines = j->token_lines;
    int old_count = j->token_count;

    j->tokens = NULL;
    j->token_lines = NULL;
    j->token_count = 0;
    j->token_capacity = 0;
    tokenize(j, src);

    char **new_tokens = j->tokens;
    int *new_lines = j->token_lines;
    int new_count = j->token_count;

    int total = old_count + new_count;
    char **merged = malloc(sizeof(char*) * (total > 0 ? total : 1));
    int *merged_lines = malloc(sizeof(int) * (total > 0 ? total : 1));
    for (int i = 0; i < old_count; i++) { merged[i] = old_tokens[i]; merged_lines[i] = old_lines[i]; }
    for (int i = 0; i < new_count; i++) { merged[old_count + i] = new_tokens[i]; merged_lines[old_count + i] = new_lines[i]; }
    free(old_tokens);
    free(old_lines);
    free(new_tokens);
    free(new_lines);

    j->tokens = merged;
    j->token_lines = merged_lines;
    j->token_count = total;
    j->token_capacity = total > 0 ? total : 1;
    return old_count;
}

int execute_statement(JustState *j, int pos) {
    if (pos >= j->token_count) return pos;

    if (j->call_depth == 0 && j->total_allocations >= j->gc_threshold) gc_collect(j);

    if (j->token_lines) j->current_line = j->token_lines[pos];
    char *cmd = j->tokens[pos];

    if (strcmp(cmd, ";") == 0) return pos + 1;

    if (strcmp(cmd, "return") == 0) {
        pos++;
        if (pos < j->token_count && strcmp(j->tokens[pos], ";") != 0) {
            j->return_value = eval_expression(j, &pos);
        }

        if (j->current_flow != FLOW_ERROR) j->current_flow = FLOW_RETURN;
        return pos;
    }

    if (strcmp(cmd, "throw") == 0) {
        pos++;
        Value *v = create_value(j, TYPE_NULL);
        if (pos < j->token_count && strcmp(j->tokens[pos], ";") != 0) {
            v = eval_expression(j, &pos);
        }
        if (j->current_flow != FLOW_ERROR) {

            char *msg = just_to_string(j, v);
            just_error(j, msg ? msg : "thrown error");
            free(j->error_message);
            j->error_message = str_dup(msg ? msg : "thrown error");
            free(msg);
            j->error_value = v;
            j->current_flow = FLOW_ERROR;
        }
        return pos;
    }

    if (strcmp(cmd, "break") == 0) {
        j->current_flow = FLOW_BREAK;
        return pos + 1;
    }

    if (strcmp(cmd, "continue") == 0) {
        j->current_flow = FLOW_CONTINUE;
        return pos + 1;
    }

	if (strcmp(cmd, "import") == 0) {
		pos++;
		if (pos >= j->token_count) return pos;
		char *fn = j->tokens[pos++];
		if (fn[0] == '"' || fn[0] == '\'') {
			int l = strlen(fn);
			fn[l-1] = '\0';
			fn++;
		}

		int saved_line = j->current_line;
		ControlFlow saved_flow = j->current_flow;

		for (int i = 0; i < j->imported_file_count; i++) {
			if (strcmp(j->imported_files[i], fn) == 0) return pos;
		}
		if (j->imported_file_count < 100)
			j->imported_files[j->imported_file_count++] = str_dup(fn);

		FILE *f = fopen(fn, "r");
		if (!f) {
			just_error(j, "Cannot import file");
			return pos;
		}
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *code = malloc(sz + 1);
		size_t _rd = fread(code, 1, sz, f); (void)_rd;
		code[sz] = '\0';
		fclose(f);
		int import_start = tokenize_append(j, code);
		int import_end = j->token_count;
		free(code);

		j->current_flow = FLOW_NORMAL;
		j->return_value = NULL;
		execute_block(j, import_start, import_end);

		j->current_line = saved_line;
		j->current_flow = saved_flow;

		return pos;
	}

    if (strcmp(cmd, "const") == 0 || strcmp(cmd, "let") == 0) {
        bool ic = (strcmp(cmd, "const") == 0);
        pos++;
        if (pos >= j->token_count) return pos;
        char *vn = j->tokens[pos++];
        if (pos < j->token_count && strcmp(j->tokens[pos], "=") == 0) {
            pos++;
            Value *v = eval_expression(j, &pos);
            declare_var(j, vn, v, ic);
            return pos;
        }
        return pos;
    }

    if (strcmp(cmd, "try") == 0) {
        pos++;
        while (pos < j->token_count && strcmp(j->tokens[pos], "{") != 0) pos++;
        if (pos >= j->token_count) return pos;
        pos++;
        int bs = pos, d = 1;
        while (pos < j->token_count && d > 0) {
            if (strcmp(j->tokens[pos], "{") == 0) d++;
            else if (strcmp(j->tokens[pos], "}") == 0) d--;
            pos++;
        }
        int be = pos - 1;

        char *catch_var = NULL;
        int cs = 0, ce = 0;
        bool has_catch = false;
        if (pos < j->token_count && strcmp(j->tokens[pos], "catch") == 0) {
            has_catch = true;
            pos++;
            if (pos < j->token_count && strcmp(j->tokens[pos], "(") == 0) {
                pos++;
                if (pos < j->token_count && strcmp(j->tokens[pos], ")") != 0)
                    catch_var = j->tokens[pos++];
                if (pos < j->token_count && strcmp(j->tokens[pos], ")") == 0) pos++;
            }
            while (pos < j->token_count && strcmp(j->tokens[pos], "{") != 0) pos++;
            if (pos >= j->token_count) return pos;
            pos++;
            cs = pos;
            int d2 = 1;
            while (pos < j->token_count && d2 > 0) {
                if (strcmp(j->tokens[pos], "{") == 0) d2++;
                else if (strcmp(j->tokens[pos], "}") == 0) d2--;
                pos++;
            }
            ce = pos - 1;
        }

        bool has_finally = false;
        int fs = 0, fe = 0;
        if (pos < j->token_count && strcmp(j->tokens[pos], "finally") == 0) {
            has_finally = true;
            pos++;
            while (pos < j->token_count && strcmp(j->tokens[pos], "{") != 0) pos++;
            if (pos >= j->token_count) return pos;
            pos++;
            fs = pos;
            int d3 = 1;
            while (pos < j->token_count && d3 > 0) {
                if (strcmp(j->tokens[pos], "{") == 0) d3++;
                else if (strcmp(j->tokens[pos], "}") == 0) d3--;
                pos++;
            }
            fe = pos - 1;
        }

        push_scope(j);
        j->current_flow = FLOW_NORMAL;
        execute_block(j, bs, be);
        bool errored = (j->current_flow == FLOW_ERROR);
        if (errored) j->current_flow = FLOW_NORMAL;
        if (j->scope_depth > 0) pop_scope(j);

        if (errored && has_catch) {
            push_scope(j);
            if (catch_var) {

                Value *ev = j->error_value ? j->error_value : create_string(j, j->error_message ? j->error_message : "error");
                declare_var(j, catch_var, ev, false);
            }
            free(j->error_message);
            j->error_message = NULL;
            j->error_value = NULL;
            j->current_flow = FLOW_NORMAL;
            execute_block(j, cs, ce);
            if (j->scope_depth > 0) pop_scope(j);
        } else if (errored) {

            j->current_flow = FLOW_ERROR;
        }

        if (has_finally) {

            ControlFlow pending_flow = j->current_flow;
            Value *pending_return = j->return_value;
            char *pending_err_msg = j->error_message;
            Value *pending_err_val = j->error_value;
            j->error_message = NULL;
            j->error_value = NULL;

            push_scope(j);
            j->current_flow = FLOW_NORMAL;
            j->return_value = NULL;
            execute_block(j, fs, fe);
            bool finally_errored = (j->current_flow == FLOW_ERROR);
            if (j->scope_depth > 0) pop_scope(j);

            if (!finally_errored) {
                j->current_flow = pending_flow;
                j->return_value = pending_return;
                j->error_message = pending_err_msg;
                j->error_value = pending_err_val;
            } else {
                free(pending_err_msg);
            }
        }

        return pos;
    }

    if (strcmp(cmd, "if") == 0) {
        pos++;
        Value *c = eval_expression(j, &pos);
        bool cond = value_to_bool(c);
        while (pos < j->token_count && strcmp(j->tokens[pos], "{") != 0) pos++;
        if (pos >= j->token_count) return pos;
        pos++;
        int bs = pos, d = 1;
        while (pos < j->token_count && d > 0) {
            if (strcmp(j->tokens[pos], "{") == 0) d++;
            else if (strcmp(j->tokens[pos], "}") == 0) d--;
            pos++;
        }
        int be = pos - 1;
        bool eh = false;
        ControlFlow sf = j->current_flow;
        if (cond) {
            push_scope(j);
            j->current_flow = FLOW_NORMAL;
            execute_block(j, bs, be);
            if (j->current_flow == FLOW_NORMAL) j->current_flow = sf;
            if (j->scope_depth > 0) pop_scope(j);
            eh = true;
        }
        while (pos < j->token_count && strcmp(j->tokens[pos], "else") == 0) {
            pos++;
            if (pos < j->token_count && strcmp(j->tokens[pos], "if") == 0) {
                pos++;
                Value *c2 = eval_expression(j, &pos);
                bool c2b = value_to_bool(c2);
                while (pos < j->token_count && strcmp(j->tokens[pos], "{") != 0) pos++;
                if (pos >= j->token_count) return pos;
                pos++;
                int b2s = pos, d2 = 1;
                while (pos < j->token_count && d2 > 0) {
                    if (strcmp(j->tokens[pos], "{") == 0) d2++;
                    else if (strcmp(j->tokens[pos], "}") == 0) d2--;
                    pos++;
                }
                int b2e = pos - 1;
                if (!eh && c2b) {
                    push_scope(j);
                    j->current_flow = FLOW_NORMAL;
                    execute_block(j, b2s, b2e);
                    if (j->current_flow == FLOW_NORMAL) j->current_flow = sf;
                    if (j->scope_depth > 0) pop_scope(j);
                    eh = true;
                }
                if (c2b) eh = true;
            } else if (pos < j->token_count && strcmp(j->tokens[pos], "{") == 0) {
                pos++;
                int es = pos, d2 = 1;
                while (pos < j->token_count && d2 > 0) {
                    if (strcmp(j->tokens[pos], "{") == 0) d2++;
                    else if (strcmp(j->tokens[pos], "}") == 0) d2--;
                    pos++;
                }
                int ee = pos - 1;
                if (!eh) {
                    push_scope(j);
                    j->current_flow = FLOW_NORMAL;
                    execute_block(j, es, ee);
                    if (j->current_flow == FLOW_NORMAL) j->current_flow = sf;
                    if (j->scope_depth > 0) pop_scope(j);
                }
                break;
            }
        }
        return pos;
    }

    if (strcmp(cmd, "while") == 0) {
        pos++;
        int cp = pos;
        while (pos < j->token_count && strcmp(j->tokens[pos], "{") != 0) pos++;
        if (pos >= j->token_count) return pos;
        pos++;
        int bs = pos, d = 1;
        while (pos < j->token_count && d > 0) {
            if (strcmp(j->tokens[pos], "{") == 0) d++;
            else if (strcmp(j->tokens[pos], "}") == 0) d--;
            pos++;
        }
        int be = pos - 1;
        for (int lc = 0; lc < j->max_iterations; lc++) {
            int chp = cp;
            Value *c = eval_expression(j, &chp);
            bool cond = value_to_bool(c);
            if (!cond) break;
            ControlFlow of = j->current_flow;
            j->current_flow = FLOW_NORMAL;
            execute_block(j, bs, be);
            if (j->current_flow == FLOW_BREAK) {
                j->current_flow = of;
                break;
            }
            if (j->current_flow == FLOW_CONTINUE) {
                j->current_flow = FLOW_NORMAL;
                continue;
            }
            if (j->current_flow != FLOW_NORMAL) {

                break;
            }
            j->current_flow = of;
        }
        return pos;
    }

	if (strcmp(cmd, "for") == 0) {
		pos++;
		if (pos + 4 >= j->token_count) return pos;
		if (strcmp(j->tokens[pos], "(") == 0) pos++;

		int is = pos;
		while (pos < j->token_count && strcmp(j->tokens[pos], ";") != 0) pos++;
		if (pos >= j->token_count) return pos;
		int ie = pos;
		pos++;

		int cp = pos;
		while (pos < j->token_count && strcmp(j->tokens[pos], ";") != 0) pos++;
		if (pos >= j->token_count) return pos;
		pos++;

		int xs = pos;
		while (pos < j->token_count && strcmp(j->tokens[pos], ")") != 0) pos++;
		if (pos >= j->token_count) return pos;
		int xe = pos;
		pos++;

		if (strcmp(j->tokens[pos], "{") != 0) return pos;
		pos++;
		int bs = pos, d = 1;
		while (pos < j->token_count && d > 0) {
			if (strcmp(j->tokens[pos], "{") == 0) d++;
			else if (strcmp(j->tokens[pos], "}") == 0) d--;
			pos++;
		}
		int be = pos - 1;

		char *iter_name = NULL;
		int temp_pos = is;
		if (temp_pos < j->token_count) {
			char *first_token = j->tokens[temp_pos];
			if (strcmp(first_token, "let") == 0 || strcmp(first_token, "const") == 0) {
				temp_pos++;
				if (temp_pos < j->token_count) {
					iter_name = j->tokens[temp_pos];
				}
			} else {
				iter_name = first_token;
			}
		}

		push_scope(j);

		ControlFlow fi = j->current_flow;
		j->current_flow = FLOW_NORMAL;
		execute_block(j, is, ie);
		j->current_flow = fi;

		Variable *iter_var = NULL;
		if (iter_name) {
			for (int s = j->scope_depth - 1; s >= 0; s--) {
				for (int i = 0; i < j->scopes[s]->var_count; i++) {
					if (strcmp(j->scopes[s]->vars[i].name, iter_name) == 0) {
						iter_var = &j->scopes[s]->vars[i];
						break;
					}
				}
				if (iter_var) break;
			}
		}

		for (int lc = 0; lc < j->max_iterations; lc++) {
			int chp = cp;
			Value *c = eval_expression(j, &chp);
			bool cond = value_to_bool(c);
			if (!cond) break;

			ControlFlow of = j->current_flow;
			j->current_flow = FLOW_NORMAL;
			execute_block(j, bs, be);

			if (j->current_flow == FLOW_BREAK) {
				j->current_flow = of;
				break;
			}
			if (j->current_flow == FLOW_CONTINUE) {
				j->current_flow = FLOW_NORMAL;
				ControlFlow fi2 = j->current_flow;
				j->current_flow = FLOW_NORMAL;
				execute_block(j, xs, xe);
				j->current_flow = fi2;
				continue;
			}
			if (j->current_flow != FLOW_NORMAL) {

				break;
			}
			j->current_flow = of;

			ControlFlow fi2 = j->current_flow;
			j->current_flow = FLOW_NORMAL;
			execute_block(j, xs, xe);
			j->current_flow = fi2;
		}

		if (j->scope_depth > 0) pop_scope(j);
		return pos;
	}

    if (strcmp(cmd, "func") == 0) {
        pos++;
        if (pos >= j->token_count) return pos;
        char *fn = j->tokens[pos++];
        char *params[MAX_ARGS];
        int pc = 0;
        if (pos < j->token_count && strcmp(j->tokens[pos], "(") == 0) {
            pos++;
            while (pos < j->token_count && strcmp(j->tokens[pos], ")") != 0) {
                if (pc > 0 && strcmp(j->tokens[pos], ",") == 0) pos++;
                if (pos < j->token_count && strcmp(j->tokens[pos], ")") != 0)
                    params[pc++] = j->tokens[pos++];
            }
            if (pos < j->token_count) pos++;
        }
        while (pos < j->token_count && strcmp(j->tokens[pos], "{") != 0) pos++;
        if (pos >= j->token_count) return pos;
        pos++;
        int bs = pos, d = 1;
        while (pos < j->token_count && d > 0) {
            if (strcmp(j->tokens[pos], "{") == 0) d++;
            else if (strcmp(j->tokens[pos], "}") == 0) d--;
            pos++;
        }
        int be = pos - 1;
        add_func(j, fn, params, pc, bs, be);
        return pos;
    }

    if (isalpha(cmd[0]) || cmd[0] == '_') {
        Variable *var = find_var(j, cmd);

        if (var && var->value && var->value->type == TYPE_ARRAY &&
            pos + 1 < j->token_count && strcmp(j->tokens[pos + 1], "[") == 0) {
            pos += 2;
            Value *idx_val = eval_expression(j, &pos);
            int idx = normalize_index((int)value_to_number(idx_val), var->value->data.array.count);

            if (pos < j->token_count && strcmp(j->tokens[pos], "]") == 0) {
                pos++;

                if (pos < j->token_count && strcmp(j->tokens[pos], ".") == 0) {
                    pos++;
                    char *prop = j->tokens[pos++];

                    if (pos < j->token_count && strcmp(j->tokens[pos], "=") == 0) {
                        pos++;
                        Value *v = eval_expression(j, &pos);
                        Value *item = array_get(var->value, idx);
                        if (item && item->type == TYPE_OBJECT) {
                            object_set(item, prop, v);
                        }
                        return pos;
                    } else if (pos < j->token_count && strcmp(j->tokens[pos], "+=") == 0) {
                        pos++;
                        Value *v = eval_expression(j, &pos);
                        Value *item = array_get(var->value, idx);
                        if (item && item->type == TYPE_OBJECT) {
                            Value *cur = object_get(item, prop);
                            Value *nv = create_number(j, value_to_number(cur) + value_to_number(v));
                            object_set(item, prop, nv);
                        }
                        return pos;
                    } else if (pos < j->token_count && strcmp(j->tokens[pos], "-=") == 0) {
                        pos++;
                        Value *v = eval_expression(j, &pos);
                        Value *item = array_get(var->value, idx);
                        if (item && item->type == TYPE_OBJECT) {
                            Value *cur = object_get(item, prop);
                            Value *nv = create_number(j, value_to_number(cur) - value_to_number(v));
                            object_set(item, prop, nv);
                        }
                        return pos;
                    }
                } else if (pos < j->token_count && strcmp(j->tokens[pos], "=") == 0) {
                    pos++;
                    Value *v = eval_expression(j, &pos);
                    array_set(var->value, idx, v);
                    return pos;
                }
            }
            return pos;
        }

        if (var && var->value && var->value->type == TYPE_OBJECT &&
            pos + 1 < j->token_count && strcmp(j->tokens[pos + 1], ".") == 0) {
            pos += 2;
            char *f = j->tokens[pos++];
            if (pos < j->token_count) {
                if (strcmp(j->tokens[pos], "=") == 0) {
                    pos++;
                    Value *v = eval_expression(j, &pos);
                    object_set(var->value, f, v);
                    return pos;
                } else if (strcmp(j->tokens[pos], "+=") == 0) {
                    pos++;
                    Value *v = eval_expression(j, &pos);
                    Value *cur = object_get(var->value, f);
                    Value *nv = create_number(j, value_to_number(cur) + value_to_number(v));
                    object_set(var->value, f, nv);
                    return pos;
                } else if (strcmp(j->tokens[pos], "-=") == 0) {
                    pos++;
                    Value *v = eval_expression(j, &pos);
                    Value *cur = object_get(var->value, f);
                    Value *nv = create_number(j, value_to_number(cur) - value_to_number(v));
                    object_set(var->value, f, nv);
                    return pos;
                }
            }
            return pos;
        }

        if (pos + 1 < j->token_count && strcmp(j->tokens[pos + 1], "=") == 0) {
            pos += 2;
            Value *v = eval_expression(j, &pos);
            set_var(j, cmd, v, false);
            return pos;
        }

        if (pos + 1 < j->token_count && (strcmp(j->tokens[pos + 1], "+=") == 0 || strcmp(j->tokens[pos + 1], "-=") == 0)) {
            char *op = j->tokens[pos + 1];
            pos += 2;
            Value *v = eval_expression(j, &pos);
            double cur = (var && var->value) ? value_to_number(var->value) : 0;
            double r = (strcmp(op, "+=") == 0) ? cur + value_to_number(v) : cur - value_to_number(v);
            Value *nv = create_number(j, r);
            set_var(j, cmd, nv, false);
            return pos;
        }

        Function *f = find_func(j, cmd);
        if (f && pos + 1 < j->token_count && strcmp(j->tokens[pos + 1], "(") == 0) {
            pos += 2;
            Value *args[MAX_ARGS];
            int ac = 0;
            while (pos < j->token_count && strcmp(j->tokens[pos], ")") != 0) {
                if (ac > 0 && strcmp(j->tokens[pos], ",") == 0) pos++;
                if (pos >= j->token_count || strcmp(j->tokens[pos], ")") == 0) break;
                args[ac++] = eval_expression(j, &pos);
            }
            if (pos < j->token_count) pos++;
            if (f->is_native) {
                Value *r = f->native_func(j, args, ac);
                adopt_into_gc(j, r); 
                (void)r;
            } else {
                if (j->call_depth >= j->max_call_depth) {
                    just_error(j, "Maximum call stack size exceeded");
                    free(j->error_message);
                    j->error_message = str_dup("Maximum call stack size exceeded");
                    j->error_value = create_string(j, "Maximum call stack size exceeded");
                    j->current_flow = FLOW_ERROR;
                    return pos;
                }
                j->call_stack[j->call_depth] = f->name;
                j->call_depth++;

                int od = j->scope_depth;
                push_scope(j);
                for (int i = 0; i < f->param_count && i < ac; i++)
                    declare_var(j, f->params[i], args[i], false);
                ControlFlow of = j->current_flow;
                j->current_flow = FLOW_NORMAL;
                j->return_value = NULL;
                execute_block(j, f->body_start, f->body_end);
                if (j->current_flow != FLOW_ERROR) j->current_flow = of;
                while (j->scope_depth > od) pop_scope(j);
                j->call_depth--;
                Value *r = j->return_value ? j->return_value : create_value(j, TYPE_NULL);
                (void)r;
                if (j->return_value) { j->return_value = NULL; }
            }
            return pos;
        }
        if (!f && var && var->value && (var->value->type == TYPE_FUNCTION || var->value->type == TYPE_NATIVE_FUNC) &&
            pos + 1 < j->token_count && strcmp(j->tokens[pos + 1], "(") == 0) {
            pos += 2;
            Value *args[MAX_ARGS];
            int ac = 0;
            while (pos < j->token_count && strcmp(j->tokens[pos], ")") != 0) {
                if (ac > 0 && strcmp(j->tokens[pos], ",") == 0) pos++;
                if (pos >= j->token_count || strcmp(j->tokens[pos], ")") == 0) break;
                args[ac++] = eval_expression(j, &pos);
            }
            if (pos < j->token_count) pos++;
            Value *r = call_function_value(j, var->value, args, ac);
            (void)r;
            return pos;
        }
    }
    pos++;
    return pos;
}

static void just_lock(JustState *j);
static void just_unlock(JustState *j);

Value* just_get_var(JustState *j, const char *name) {
    if (!j) return just_null();
    just_lock(j);
    Variable *v = find_var(j, name);
    Value *r = v ? v->value : just_null();
    just_unlock(j);
    return r;
}

void just_set_var(JustState *j, const char *name, Value *val) {
    if (!j) return;
    just_lock(j);
    adopt_into_gc(j, val);
    set_var(j, name, val, false);
    just_unlock(j);
}

void just_register_function(JustState *j, const char *name, NativeFunc func) {
    if (!j) return;
    just_lock(j);
    add_native_func(j, name, func);
    just_unlock(j);
}

void run_code(JustState *j, const char *src) {
    int start = tokenize_append(j, src);
    if (j->token_limit_hit) {

        j->token_limit_hit = false;
        just_error(j, "Token limit exceeded (see just_set_max_tokens)");
        return;
    }
    register_builtins(j);
    j->current_flow = FLOW_NORMAL;
    j->return_value = NULL;
    execute_block(j, start, j->token_count);
    if (j->current_flow == FLOW_ERROR) {

        j->current_flow = FLOW_NORMAL;
        free(j->error_message);
        j->error_message = NULL;
        j->error_value = NULL;
    }

}

JustState* just_init(void) {
    return just_init_ex(JUST_CAP_ALL);
}

JustState* just_init_ex(int capabilities) {
    JustState *j = calloc(1, sizeof(JustState));
    if (!j) return NULL;
    j->capabilities = capabilities;
    j->gc_threshold = GC_THRESHOLD;
    j->imported_file_count = 0;

    j->rand_seed = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)j ^ (uint32_t)clock();
    if (j->rand_seed == 0) j->rand_seed = 0xA5A5A5A5u;
    j->max_iterations = MAX_ITERATIONS;
    j->max_call_depth = MAX_CALL_DEPTH;
    j->max_tokens = MAX_TOKENS;
#ifdef _WIN32
    InitializeCriticalSection(&j->eval_lock);
#else
    pthread_mutex_init(&j->eval_lock, NULL);
#endif
#ifdef _WIN32
    j->win10_ansi_supported = false;
#endif

    push_scope(j);
    return j;
}

static void just_lock(JustState *j) {
#ifdef _WIN32
    EnterCriticalSection(&j->eval_lock);
#else
    pthread_mutex_lock(&j->eval_lock);
#endif
}
static void just_unlock(JustState *j) {
#ifdef _WIN32
    LeaveCriticalSection(&j->eval_lock);
#else
    pthread_mutex_unlock(&j->eval_lock);
#endif
}

Value* just_eval(JustState *j, const char *code) {
    if (!j) return just_null();
    just_lock(j);
    if (j->scope_depth == 0) push_scope(j);
    run_code(j, code);
    Value *r = j->return_value;
    just_unlock(j);
    return r;
}

Value* just_call(JustState *j, const char *func_name, Value **args, int argc) {
    if (!j || !func_name) return just_null();
    just_lock(j);
    if (j->scope_depth == 0) push_scope(j);
    Value *fn = create_value(j, TYPE_FUNCTION);
    fn->is_lambda = false;
    fn->data.string = str_dup(func_name);
    Value *r = call_function_value(j, fn, args, argc);
    just_unlock(j);
    return r ? r : just_null();
}

void just_eval_file(JustState *j, const char *filename) {
    if (!j) return;
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot open '%s'\n", filename);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *code = malloc(sz + 1);
    size_t _rd = fread(code, 1, sz, f); (void)_rd;
    code[sz] = '\0';
    fclose(f);
    just_lock(j);
    if (j->scope_depth == 0) push_scope(j);
    run_code(j, code);
    just_unlock(j);
    free(code);
}

void just_destroy(JustState *j) {
    if (!j) return;
    shutdown_cleanup(j);
    free(j);
}

void just_set_const(JustState *j, const char *name, Value *val) {
    if (!j) return;
    just_lock(j);
    adopt_into_gc(j, val);
    set_var(j, name, val, true);
    just_unlock(j);
}

void just_gc_collect(JustState *j) {
    if (!j) return;
    just_lock(j);
    gc_collect(j);
    just_unlock(j);
}

void just_gc_set_threshold(JustState *j, int threshold) {

    if (!j || threshold <= 0) return;
    j->gc_threshold = threshold;
}

void just_set_max_iterations(JustState *j, int n) {
    if (!j || n <= 0) return;
    j->max_iterations = n;
}

void just_set_max_call_depth(JustState *j, int n) {
    if (!j || n <= 0) return;
    j->max_call_depth = n;
}

void just_set_max_tokens(JustState *j, long n) {
    if (!j || n <= 0) return;
    j->max_tokens = n;
}

int just_gc_get_count(JustState *j) {
    return j ? j->gc_count : 0;
}

int just_gc_get_allocations(JustState *j) {
    return j ? j->total_allocations : 0;
}

Value* just_object_new(JustState *j) {
    return j ? create_object(j) : just_null();
}

void just_object_set(Value *obj, const char *key, Value *val) {
    object_set(obj, key, val);
}

Value* just_object_get(Value *obj, const char *key) {
    return object_get(obj, key);
}

bool just_object_has(Value *obj, const char *key) {
    return object_has(obj, key);
}

Value* just_array_new(JustState *j) {
    return j ? create_array(j) : just_null();
}

void just_array_push(Value *arr, Value *val) {
    array_push(arr, val);
}

Value* just_array_get(Value *arr, int index) {
    return array_get(arr, index);
}

void just_array_set(Value *arr, int index, Value *val) {
    array_set(arr, index, val);
}

int just_array_length(Value *arr) {
    if (!arr || arr->type != TYPE_ARRAY) return 0;
    return arr->data.array.count;
}

ValueType just_get_type(Value *v) {
    return v ? v->type : TYPE_NULL;
}

const char* just_type_name(Value *v) {
    static const char *names[] = {
        "null", "number", "string", "bool",
        "object", "array", "function", "native_function"
    };
    if (!v) return "null";
    if (v->type < 0 || v->type > TYPE_NATIVE_FUNC) return "unknown";
    return names[v->type];
}

char* just_to_json(JustState *j, Value *v) {
    if (!j || !v) return str_dup("null");
    Value *jv = value_to_json(j, v);
    char *result = str_dup(jv->data.string);
    return result;
}

Value* just_from_json(JustState *j, const char *json) {
    if (!j || !json) return just_null();
    const char *p = json;
    return json_parse_value(j, &p);
}

char* just_to_string(JustState *j, Value *v) {
    if (!j || !v) return str_dup("null");
    char *result = value_to_string_raw(v);
    return result;
}

void just_set_debug(JustState *j, bool on) {
    (void)j;
    (void)on;
}

void just_print_state(JustState *j) {
    if (!j) {
        printf("State: NULL\n");
        return;
    }
    printf("JustState:\n");
    printf("  GC count: %d\n", j->gc_count);
    printf("  Allocations: %d\n", j->total_allocations);
    printf("  Scope depth: %d\n", j->scope_depth);
    printf("  Functions: %d\n", j->func_count);
    printf("  Tokens: %d\n", j->token_count);
    printf("  Current line: %d\n", j->current_line);
}

const char* just_version(void) {
    return "1.0.0";
}

int just_version_major(void) {
    return 1;
}

int just_version_minor(void) {
    return 0;
}

int just_version_patch(void) {
    return 0;
}

void shutdown_cleanup(JustState *j) {
#ifdef _WIN32
    DeleteCriticalSection(&j->eval_lock);
#else
    pthread_mutex_destroy(&j->eval_lock);
#endif
    for (int i = 0; i < j->func_count; i++) {
        free(j->funcs[i].name);
        if (!j->funcs[i].is_native) {
            for (int jj = 0; jj < j->funcs[i].param_count; jj++)
                free(j->funcs[i].params[jj]);
            free(j->funcs[i].params);
        }
    }
    free(j->funcs);

#ifdef _WIN32
    if (j->winsock_initialized) WSACleanup();
#endif

    for (int i = 0; i < j->loaded_plugin_count; i++) {
#ifdef _WIN32
        FreeLibrary((HMODULE)j->loaded_plugins[i]);
#else
        dlclose(j->loaded_plugins[i]);
#endif
    }
    free(j->loaded_plugins);

    for (int i = 0; i < j->scope_depth; i++) {
        if (j->scopes[i]) {
            for (int k = 0; k < j->scopes[i]->var_count; k++) {
                free(j->scopes[i]->vars[k].name);
            }
            free(j->scopes[i]->vars);
            free(j->scopes[i]);
        }
    }

    free(j->scopes);
    j->scopes = NULL;
    j->scope_capacity = 0;

    for (int i = 0; i < j->imported_file_count; i++) free(j->imported_files[i]);

    if (j->tokens) { for (int i = 0; i < j->token_count; i++) free(j->tokens[i]); free(j->tokens); }
    free(j->token_lines);
    free(j->error_message);

    free(j->handles);

    GCNode *node = j->gc_head;
    while (node) {
        GCNode *next = node->next;

        if (node->value) {
            Value *v = node->value;
            switch (v->type) {
                case TYPE_STRING: free(v->data.string); break;
                case TYPE_FUNCTION:
                    if (v->is_lambda) {
                        for (int i = 0; i < v->data.lambda.param_count; i++) free(v->data.lambda.params[i]);
                        free(v->data.lambda.params);
                    } else {
                        free(v->data.string);
                    }
                    break;
                case TYPE_OBJECT:
                    for (int i = 0; i < v->data.object.count; i++) free(v->data.object.keys[i]);
                    free(v->data.object.keys);
                    free(v->data.object.values);
                    break;
                case TYPE_ARRAY: free(v->data.array.items); break;
                default: break;
            }
            free(v);
        }
        free(node);
        node = next;
    }
    j->gc_head = j->gc_tail = NULL;
    j->gc_count = 0;
}