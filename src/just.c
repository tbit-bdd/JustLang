#include "just.h"

#define SQLITE_OMIT_LOAD_EXTENSION  
#define SQLITE_THREADSAFE 0        
#define SQLITE_OMIT_DEPRECATED      
#include "sqlite3.c"                 

typedef struct {
    JustState *j;
    Value *arr;
} DbCallbackCtx;

static void add_native_func(JustState *j, const char *name, NativeFunc func);

static JustState* plugin_state = NULL;

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
    fprintf(stderr, "\nERROR (line %d): %s\n", j->current_line, msg); 
    fprintf(stderr, "  Stack trace:\n"); 
    for (int s = j->scope_depth - 1; s >= 0; s--) 
        fprintf(stderr, "  [%d] scope\n", s); 
}

#ifdef _WIN32
void just_sleep_ms(int ms) { Sleep(ms); }
#else
void just_sleep_ms(int ms) { usleep(ms * 1000); }
#endif

static void gc_add_node(JustState *j, Value *v);
static void gc_mark_value(JustState *j, Value *v);
static void gc_mark_roots(JustState *j);
static void gc_sweep(JustState *j);
static void gc_collect(JustState *j);
static Value* create_value(JustState *j, ValueType type);
static Value* create_number(JustState *j, double n);
static Value* create_string(JustState *j, const char *s);
static Value* create_bool(JustState *j, bool b);
static Value* create_object(JustState *j);
static Value* create_array(JustState *j);
static Value* clone_value_internal(JustState *j, Value *v, int depth);
static Value* clone_value(JustState *j, Value *v);
static void object_set(Value *obj, const char *key, Value *val);
static Value* object_get(Value *obj, const char *key);
static bool object_has(Value *obj, const char *key);
static void array_push(Value *arr, Value *val);
static Value* array_get(Value *arr, int index);
static void array_set(Value *arr, int index, Value *val);
static double value_to_number(Value *v);
static bool value_to_bool(Value *v);
static char* value_to_string_raw(Value *v);
static Value* value_to_string(JustState *j, Value *v);
static Value* value_to_json(JustState *j, Value *v);
static Value* json_parse_value(JustState *j, const char **p);
static void push_scope(JustState *j);
static void pop_scope(JustState *j);
static Variable* find_var(JustState *j, const char *name);
static void set_var(JustState *j, const char *name, Value *value, bool constant);
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
    node->marked = false;
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

void gc_mark_value(JustState *j, Value *v) {
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

void gc_mark_roots(JustState *j) {
    for (int s = 0; s < j->scope_depth; s++)
        for (int i = 0; i < j->scopes[s]->var_count; i++)
            gc_mark_value(j, j->scopes[s]->vars[i].value);
    if (j->return_value) gc_mark_value(j, j->return_value);
}

void gc_sweep(JustState *j) {
    GCNode *prev = NULL, *curr = j->gc_head;
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
        node->marked = false; 
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
    if (j->total_allocations >= GC_THRESHOLD) gc_collect(j);
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

Value* array_get(Value *arr, int index) { 
    if (!arr || arr->type != TYPE_ARRAY || index < 0 || index >= arr->data.array.count) 
        return create_value(NULL, TYPE_NULL); 
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

char* value_to_string_raw(Value *v) { 
    if (!v) return str_dup("null"); 
    switch (v->type) { 
        case TYPE_NULL: return str_dup("null"); 
        case TYPE_NUMBER: { 
            char b[64]; 
            if (v->data.number == (int)v->data.number) 
                snprintf(b, 64, "%d", (int)v->data.number); 
            else 
                snprintf(b, 64, "%g", v->data.number); 
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
            snprintf(b, 64, "%g", v->data.number); 
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

void set_var(JustState *j, const char *name, Value *value, bool constant) { 
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
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, ""); 
    FILE *f = fopen(args[0]->data.string, "r"); 
    if (!f) return create_string(j, ""); 
    fseek(f, 0, SEEK_END); 
    long sz = ftell(f); 
    fseek(f, 0, SEEK_SET); 
    char *b = malloc(sz + 1); 
    fread(b, 1, sz, f); 
    b[sz] = '\0'; 
    fclose(f); 
    Value *v = create_string(j, b); 
    free(b); 
    return v; 
}

Value* builtin_write_file(JustState *j, Value **args, int count) { 
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

Value* builtin_http_get(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_string(j, "");
#ifdef _WIN32
    if (!j->winsock_initialized) { 
        WSADATA wsa; 
        WSAStartup(MAKEWORD(2,2), &wsa); 
        j->winsock_initialized = true; 
    }
#endif
    char *url_orig = args[0]->data.string;
    char *url = str_dup(url_orig);
    char host[256] = {0}, path[1024] = "/"; 
    int port = 80;
    Value *result = create_string(j, "");
    
    char *p = strstr(url, "://"); 
    if (p) p += 3; 
    else p = url;
    char *ps = strchr(p, '/'); 
    if (ps) { 
        strncpy(path, ps, 1023); 
        *ps = '\0'; 
    }
    char *pp = strchr(p, ':'); 
    if (pp) { 
        *pp = '\0'; 
        port = atoi(pp + 1); 
    }
    strncpy(host, p, 255);
    
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { 
        free(url); 
        return result; 
    }
    
    struct sockaddr_in addr; 
    addr.sin_family = AF_INET; 
    addr.sin_port = htons(port);
    struct hostent *he = gethostbyname(host);
    if (!he) { 
        closesocket(sock); 
        free(url); 
        return result; 
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { 
        closesocket(sock); 
        free(url); 
        return result; 
    }
    
    char req[2048]; 
    snprintf(req, 2048, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    send(sock, req, strlen(req), 0);
    
    char *buf = malloc(MAX_STRING); 
    int total = 0, n;
    while ((n = recv(sock, buf + total, MAX_STRING - total - 1, 0)) > 0) 
        total += n;
    buf[total] = '\0'; 
    closesocket(sock);
    
    char *body = strstr(buf, "\r\n\r\n");
    result = body ? create_string(j, body + 4) : create_string(j, buf);
    free(buf);
    free(url);
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
    fread(b, 1, sz, f); 
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

Value* builtin_sqrt(JustState *j, Value **args, int count) { 
    if (count < 1) return create_number(j, 0); 
    return create_number(j, sqrt(value_to_number(args[0]))); 
}

Value* builtin_random(JustState *j, Value **args, int count) { 
    double max = count >= 1 ? value_to_number(args[0]) : 1.0; 
    double r = (double)(rand() % 10000) / 10000.0; 
    return create_number(j, r * max); 
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
    if (count < 1 || args[0]->type != TYPE_ARRAY) return create_array(j); 
    Value *result = create_array(j); 
    for (int i = 0; i < args[0]->data.array.count; i++) { 
        Value *item = args[0]->data.array.items[i]; 
        if (value_to_bool(item)) array_push(result, item); 
    } 
    return result; 
}

Value* builtin_map(JustState *j, Value **args, int count) { 
    if (count < 2 || args[0]->type != TYPE_ARRAY || args[1]->type != TYPE_NATIVE_FUNC) 
        return create_array(j); 
    Value *result = create_array(j); 
    for (int i = 0; i < args[0]->data.array.count; i++) { 
        Value *item = args[0]->data.array.items[i]; 
        item->marked = true; 
        Value *mapped = args[1]->data.native_func(j, &item, 1); 
        array_push(result, mapped); 
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
    if (count < 2 || args[0]->type != TYPE_STRING || args[1]->type != TYPE_STRING) 
        return create_string(j, "");
#ifdef _WIN32
    if (!j->winsock_initialized) { 
        WSADATA wsa; 
        WSAStartup(MAKEWORD(2,2), &wsa); 
        j->winsock_initialized = true; 
    }
#endif
    char *url_orig = args[0]->data.string;
    char *url = str_dup(url_orig);
    char *body = args[1]->data.string;
    char host[256] = {0}, path[1024] = "/"; 
    int port = 80;
    Value *result = create_string(j, "");
    
    char *p = strstr(url, "://"); 
    if (p) p += 3; 
    else p = url;
    char *ps = strchr(p, '/'); 
    if (ps) { 
        strncpy(path, ps, 1023); 
        *ps = '\0'; 
    }
    char *pp = strchr(p, ':'); 
    if (pp) { 
        *pp = '\0'; 
        port = atoi(pp + 1); 
    }
    strncpy(host, p, 255);
    
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { 
        free(url); 
        return result; 
    }
    
    struct sockaddr_in addr; 
    addr.sin_family = AF_INET; 
    addr.sin_port = htons(port);
    struct hostent *he = gethostbyname(host);
    if (!he) { 
        closesocket(sock); 
        free(url); 
        return result; 
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { 
        closesocket(sock); 
        free(url); 
        return result; 
    }
    
    char req[4096]; 
    snprintf(req, 4096, "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", 
             path, host, (int)strlen(body), body);
    send(sock, req, strlen(req), 0);
    
    char *buf = malloc(MAX_STRING); 
    int total = 0, n;
    while ((n = recv(sock, buf + total, MAX_STRING - total - 1, 0)) > 0) 
        total += n;
    buf[total] = '\0'; 
    closesocket(sock);
    
    char *resp = strstr(buf, "\r\n\r\n");
    result = resp ? create_string(j, resp + 4) : create_string(j, buf);
    free(buf);
    free(url);
    return result;
}

Value* builtin_exec(JustState *j, Value **args, int count) { 
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
    push_scope(j); 
    ControlFlow of = j->current_flow; 
    j->current_flow = FLOW_NORMAL; 
    j->return_value = NULL; 
    execute_block(j, f->body_start, f->body_end); 
    j->current_flow = of; 
    while (j->scope_depth > 0) pop_scope(j); 
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
    while (1) { 
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
    if (count >= 1 && args[0]->type == TYPE_STRING) 
        just_error(j, args[0]->data.string); 
    return create_value(j, TYPE_NULL); 
}

Value* builtin_load_plugin(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_STRING) return create_bool(j, false);
    char *name = args[0]->data.string;
    char fn[MAX_STRING];
    
    plugin_state = j;
    
#ifdef _WIN32
    snprintf(fn, MAX_STRING, "%s.dll", name);
    HMODULE h = LoadLibraryA(fn);
    if (!h) {
        plugin_state = NULL;
        return create_bool(j, false);
    }
    void (*init)(RegisterFunc) = (void(*)(RegisterFunc))GetProcAddress(h, "init_plugin");
    if (!init) { 
        FreeLibrary(h); 
        plugin_state = NULL;
        return create_bool(j, false); 
    }
    init(plugin_adapter);
#else
    snprintf(fn, MAX_STRING, "./%s.so", name);
    void *h = dlopen(fn, RTLD_NOW);
    if (!h) {
        plugin_state = NULL;
        return create_bool(j, false);
    }
    void (*init)(RegisterFunc) = (void(*)(RegisterFunc))dlsym(h, "init_plugin");
    if (!init) { 
        dlclose(h); 
        plugin_state = NULL;
        return create_bool(j, false); 
    }
    init(plugin_adapter);
#endif
    
    j->loaded_plugins = realloc(j->loaded_plugins, sizeof(void*) * (j->loaded_plugin_count + 1));
    j->loaded_plugins[j->loaded_plugin_count++] = h;
    
    plugin_state = NULL;
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
    return builtin_import_json(j, args, count); 
}

Value* builtin_write_json(JustState *j, Value **args, int count) { 
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

Value* builtin_db_open(JustState *j, Value **args, int count) {
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

    return create_number(j, (double)(intptr_t)db);
}

Value* builtin_db_close(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_value(j, TYPE_NULL);
    }
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
    sqlite3_close(db);
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
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
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
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
    sqlite3_stmt *stmt;
    const char *sql = args[1]->data.string;
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        just_error(j, sqlite3_errmsg(db));
        return create_value(j, TYPE_NULL);
    }
    
    return create_number(j, (double)(intptr_t)stmt);
}

Value* builtin_db_bind(JustState *j, Value **args, int count) {
    if (count < 3 || args[0]->type != TYPE_NUMBER) {
        return create_bool(j, false);
    }
    
    sqlite3_stmt *stmt = (sqlite3_stmt*)(intptr_t)value_to_number(args[0]);
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
    
    sqlite3_stmt *stmt = (sqlite3_stmt*)(intptr_t)value_to_number(args[0]);
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
    
    sqlite3_stmt *stmt = (sqlite3_stmt*)(intptr_t)value_to_number(args[0]);
    sqlite3_finalize(stmt);
    return create_value(j, TYPE_NULL);
}

Value* builtin_db_last_insert_id(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_number(j, 0);
    }
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
    return create_number(j, sqlite3_last_insert_rowid(db));
}

Value* builtin_db_changes(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_number(j, 0);
    }
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
    return create_number(j, sqlite3_changes(db));
}

Value* builtin_db_begin(JustState *j, Value **args, int count) {
    if (count < 1 || args[0]->type != TYPE_NUMBER) {
        return create_bool(j, false);
    }
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
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
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
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
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
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
    
    sqlite3 *db = (sqlite3*)(intptr_t)value_to_number(args[0]);
    const char *err = sqlite3_errmsg(db);
    return create_string(j, err ? err : "");
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
    static bool registered = false;
    if (registered) return;
    registered = true;
    
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
    add_native_func(j, "sqrt", builtin_sqrt); 
    add_native_func(j, "random", builtin_random);
	add_native_func(j, "sin", builtin_sin);
	add_native_func(j, "cos", builtin_cos);
	add_native_func(j, "tan", builtin_tan);
	add_native_func(j, "log", builtin_log);
	add_native_func(j, "exp", builtin_exp);
	just_set_const(j, "PI", create_number(j, 3.141592653589793));
    just_set_const(j, "E", create_number(j, 2.718281828459045));
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
}

// Tokenizer
void tokenize(JustState *j, const char *src) {
    j->current_line = 1;
    if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) 
        src += 3;
    if (j->tokens) { 
        for (int i = 0; i < j->token_count; i++) free(j->tokens[i]); 
        free(j->tokens); 
    }
    j->tokens = NULL; 
    j->token_count = 0; 
    j->token_capacity = 256;
    j->tokens = malloc(sizeof(char*) * j->token_capacity);
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
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity); 
            }
            if (j->token_count >= MAX_TOKENS) { 
                just_error(j, "Too many tokens"); 
                exit(1); 
            }
            j->tokens[j->token_count++] = t; 
            continue;
        }
        
        if (p[0]=='=' && p[1]=='=' && p[2]=='=') { 
            char *t = malloc(4); 
            t[0]='='; t[1]='='; t[2]='='; t[3]='\0'; 
            if (j->token_count >= j->token_capacity) { 
                j->token_capacity *= 2; 
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity); 
            }
            if (j->token_count >= MAX_TOKENS) { 
                just_error(j, "Too many tokens"); 
                exit(1); 
            }
            j->tokens[j->token_count++] = t; 
            p += 3; 
            continue; 
        }
        if (p[0]=='!' && p[1]=='=' && p[2]=='=') { 
            char *t = malloc(4); 
            t[0]='!'; t[1]='='; t[2]='='; t[3]='\0'; 
            if (j->token_count >= j->token_capacity) { 
                j->token_capacity *= 2; 
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity); 
            }
            if (j->token_count >= MAX_TOKENS) { 
                just_error(j, "Too many tokens"); 
                exit(1); 
            }
            j->tokens[j->token_count++] = t; 
            p += 3; 
            continue; 
        }
        
        if (p[0]=='*' && p[1]=='*') { 
            char *t = malloc(3); 
            t[0]='*'; t[1]='*'; t[2]='\0'; 
            if (j->token_count >= j->token_capacity) { 
                j->token_capacity *= 2; 
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity); 
            }
            if (j->token_count >= MAX_TOKENS) { 
                just_error(j, "Too many tokens"); 
                exit(1); 
            }
            j->tokens[j->token_count++] = t; 
            p += 2; 
            continue; 
        }
        
        if ((p[0]=='='&&p[1]=='=')||(p[0]=='!'&&p[1]=='=')||(p[0]=='<'&&p[1]=='=')||(p[0]=='>'&&p[1]=='=')||(p[0]=='+'&&p[1]=='=')||(p[0]=='-'&&p[1]=='=')) {
            char *t = malloc(3); 
            t[0]=p[0]; t[1]=p[1]; t[2]='\0';
            if (j->token_count >= j->token_capacity) { 
                j->token_capacity *= 2; 
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity); 
            }
            if (j->token_count >= MAX_TOKENS) { 
                just_error(j, "Too many tokens"); 
                exit(1); 
            }
            j->tokens[j->token_count++] = t; 
            p += 2; 
            continue;
        }
        
        if (strchr("(){}[]:,;=+*/<>.!@#$%^&|~?-", *p)) {
            char *t = malloc(2); 
            t[0]=*p; t[1]='\0';
            if (j->token_count >= j->token_capacity) { 
                j->token_capacity *= 2; 
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity); 
            }
            if (j->token_count >= MAX_TOKENS) { 
                just_error(j, "Too many tokens"); 
                exit(1); 
            }
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
                j->tokens = realloc(j->tokens, sizeof(char*) * j->token_capacity); 
            }
            if (j->token_count >= MAX_TOKENS) { 
                just_error(j, "Too many tokens"); 
                exit(1); 
            }
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
					return r; 
				}
                push_scope(j); 
                for (int i = 0; i < f->param_count && i < ac; i++) 
                    set_var(j, f->params[i], args[i], false); 
                int od = j->scope_depth; 
                ControlFlow of = j->current_flow; 
                j->current_flow = FLOW_NORMAL; 
                j->return_value = NULL; 
                execute_block(j, f->body_start, f->body_end); 
                j->current_flow = of; 
                Value *saved_return = j->return_value;
                while (j->scope_depth > od) pop_scope(j);
                Value *r = saved_return ? saved_return : create_value(j, TYPE_NULL);
                if (saved_return) { 
                    saved_return = NULL; 
                    j->return_value = NULL; 
                }
                return r;
            } 
            return create_value(j, TYPE_NULL); 
        } 
        (*pos)++; 
        Variable *v = find_var(j, t); 
        if (v) return v->value; 
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
        else if (strcmp(op,"/")==0) res=b!=0?a/b:0; 
        else res=b!=0?fmod(a,b):0; 
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
        l = create_bool(j, res);
    }
    return l;
}

Value* eval_logical_and(JustState *j, int *pos) { 
    Value *l = eval_comparison(j, pos); 
    while (*pos < j->token_count && strcmp(j->tokens[*pos],"and")==0) { 
        (*pos)++; 
        Value *r = eval_comparison(j, pos); 
        bool b = value_to_bool(l) && value_to_bool(r); 
        l = create_bool(j, b); 
    } 
    return l; 
}

Value* eval_logical_or(JustState *j, int *pos) { 
    Value *l = eval_logical_and(j, pos); 
    while (*pos < j->token_count && strcmp(j->tokens[*pos],"or")==0) { 
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
    JustState dummy = {0};
    return create_number(&dummy, n); 
}
Value* just_string(const char *s) { 
    JustState dummy = {0};
    return create_string(&dummy, s); 
}
Value* just_bool(bool b) { 
    JustState dummy = {0};
    return create_bool(&dummy, b); 
}
Value* just_null(void) { 
    JustState dummy = {0};
    return create_value(&dummy, TYPE_NULL); 
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
    int old_count = j->token_count;

    j->tokens = NULL;
    j->token_count = 0;
    j->token_capacity = 0;
    tokenize(j, src);

    char **new_tokens = j->tokens;
    int new_count = j->token_count;

    int total = old_count + new_count;
    char **merged = malloc(sizeof(char*) * (total > 0 ? total : 1));
    for (int i = 0; i < old_count; i++) merged[i] = old_tokens[i];
    for (int i = 0; i < new_count; i++) merged[old_count + i] = new_tokens[i];
    free(old_tokens);
    free(new_tokens);

    j->tokens = merged;
    j->token_count = total;
    j->token_capacity = total > 0 ? total : 1;
    return old_count;
}

int execute_statement(JustState *j, int pos) { 
    if (pos >= j->token_count) return pos; 
    char *cmd = j->tokens[pos]; 
    
    if (strcmp(cmd, ";") == 0) return pos + 1; 
    
    if (strcmp(cmd, "return") == 0) { 
        pos++; 
        if (pos < j->token_count && strcmp(j->tokens[pos], ";") != 0) { 
            j->return_value = eval_expression(j, &pos); 
        } 
        j->current_flow = FLOW_RETURN; 
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
            set_var(j, vn, v, ic); 
            return pos; 
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
        for (int lc = 0; lc < MAX_ITERATIONS; lc++) { 
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
		
		for (int lc = 0; lc < MAX_ITERATIONS; lc++) { 
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
            int idx = (int)value_to_number(idx_val);
            
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
            } else {
                push_scope(j);
                for (int i = 0; i < f->param_count && i < ac; i++) 
                    set_var(j, f->params[i], args[i], false);
                int od = j->scope_depth;
                ControlFlow of = j->current_flow;
                j->current_flow = FLOW_NORMAL;
                j->return_value = NULL;
                execute_block(j, f->body_start, f->body_end);
                j->current_flow = of;
                while (j->scope_depth > od) pop_scope(j);
                Value *r = j->return_value ? j->return_value : create_value(j, TYPE_NULL);
                if (j->return_value) { j->return_value = NULL; }
            }
            return pos;
        }
    }
    pos++;
    return pos;
}

Value* just_get_var(JustState *j, const char *name) {
    Variable *v = find_var(j, name);
    return v ? v->value : just_null();
}

void just_set_var(JustState *j, const char *name, Value *val) {
    set_var(j, name, val, false);
}

void just_register_function(JustState *j, const char *name, NativeFunc func) {
    add_native_func(j, name, func);
}

void run_code(JustState *j, const char *src) { 
    j->current_line = 1; 
    tokenize(j, src); 
    register_builtins(j); 
    j->current_flow = FLOW_NORMAL; 
    j->return_value = NULL; 
    execute_block(j, 0, j->token_count); 
    for (int i = 0; i < j->token_count; i++) free(j->tokens[i]); 
    free(j->tokens); 
    j->tokens = NULL; 
    j->token_count = 0; 
}

JustState* just_init(void) {
    srand((unsigned)time(NULL));
    JustState *j = calloc(1, sizeof(JustState));
#ifdef _WIN32
    j->win10_ansi_supported = false;
#endif
    return j;
}

Value* just_eval(JustState *j, const char *code) {
    if (!j) return just_null();
    if (j->scope_depth == 0) push_scope(j);
    run_code(j, code);
    return j->return_value;
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
    fread(code, 1, sz, f);
    code[sz] = '\0';
    fclose(f);
    if (j->scope_depth == 0) push_scope(j);
    run_code(j, code);
    free(code);
}

void just_destroy(JustState *j) {
    if (!j) return;
    shutdown_cleanup(j);
    free(j);
}

void just_set_const(JustState *j, const char *name, Value *val) {
    if (!j) return;
    set_var(j, name, val, true);
}

void just_gc_collect(JustState *j) {
    if (!j) return;
    gc_collect(j);
}

void just_gc_set_threshold(JustState *j, int threshold) {
    (void)j;
    (void)threshold;
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
    return "3.0.0";
}

int just_version_major(void) {
    return 3;
}

int just_version_minor(void) {
    return 0;
}

int just_version_patch(void) {
    return 0;
}

void shutdown_cleanup(JustState *j) {
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
    
    GCNode *node = j->gc_head;
    while (node) { 
        GCNode *next = node->next; 
        free(node); 
        node = next; 
    }
    j->gc_head = j->gc_tail = NULL; 
    j->gc_count = 0;
}