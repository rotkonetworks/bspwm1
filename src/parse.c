#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include "parse.h"

// binary search tables for O(log n) lookups
typedef struct {
   const char *key;
   int value;
} lookup_entry_t;

#define LOOKUP_TABLE(name, type) \
static const struct { const char *key; type value; } name##_table[] =

#define BINARY_SEARCH_PARSER(name, type) \
bool parse_##name(char *s, type *v) { \
    if (!s || !v) return false; \
    int left = 0, right = (sizeof(name##_table)/sizeof(name##_table[0])) - 1; \
    while (left <= right) { \
        int mid = left + ((right - left) >> 1); /* Avoid overflow */ \
        int cmp = strcmp(s, name##_table[mid].key); \
        if (cmp == 0) { \
            *v = name##_table[mid].value; \
            return true; \
        } \
        if (cmp > 0) left = mid + 1; \
        else right = mid - 1; \
    } \
    return false; \
}

// sorted lookup tables
LOOKUP_TABLE(split_type, split_type_t) {
   {"horizontal", TYPE_HORIZONTAL},
   {"vertical", TYPE_VERTICAL}
};

LOOKUP_TABLE(split_mode, split_mode_t) {
   {"automatic", MODE_AUTOMATIC},
   {"vertical", MODE_MANUAL}
};

LOOKUP_TABLE(layout, layout_t) {
   {"monocle", LAYOUT_MONOCLE},
   {"tiled", LAYOUT_TILED}
};

LOOKUP_TABLE(client_state, client_state_t) {
   {"floating", STATE_FLOATING},
   {"fullscreen", STATE_FULLSCREEN},
   {"pseudo_tiled", STATE_PSEUDO_TILED},
   {"tiled", STATE_TILED}
};

LOOKUP_TABLE(stack_layer, stack_layer_t) {
   {"above", LAYER_ABOVE},
   {"below", LAYER_BELOW},
   {"normal", LAYER_NORMAL}
};

LOOKUP_TABLE(direction, direction_t) {
   {"east", DIR_EAST},
   {"north", DIR_NORTH},
   {"south", DIR_SOUTH},
   {"west", DIR_WEST}
};

LOOKUP_TABLE(cycle_direction, cycle_dir_t) {
   {"next", CYCLE_NEXT},
   {"prev", CYCLE_PREV}
};

LOOKUP_TABLE(circulate_direction, circulate_dir_t) {
   {"backward", CIRCULATE_BACKWARD},
   {"forward", CIRCULATE_FORWARD}
};

LOOKUP_TABLE(history_direction, history_dir_t) {
   {"newer", HISTORY_NEWER},
   {"older", HISTORY_OLDER}
};

LOOKUP_TABLE(flip, flip_t) {
   {"horizontal", FLIP_HORIZONTAL},
   {"vertical", FLIP_VERTICAL}
};

LOOKUP_TABLE(resize_handle, resize_handle_t) {
   {"bottom", HANDLE_BOTTOM},
   {"bottom_left", HANDLE_BOTTOM_LEFT},
   {"bottom_right", HANDLE_BOTTOM_RIGHT},
   {"left", HANDLE_LEFT},
   {"right", HANDLE_RIGHT},
   {"top", HANDLE_TOP},
   {"top_left", HANDLE_TOP_LEFT},
   {"top_right", HANDLE_TOP_RIGHT}
};

LOOKUP_TABLE(pointer_action, pointer_action_t) {
   {"focus", ACTION_FOCUS},
   {"move", ACTION_MOVE},
   {"none", ACTION_NONE},
   {"resize_corner", ACTION_RESIZE_CORNER},
   {"resize_side", ACTION_RESIZE_SIDE}
};

LOOKUP_TABLE(child_polarity, child_polarity_t) {
   {"first_child", FIRST_CHILD},
   {"second_child", SECOND_CHILD}
};

LOOKUP_TABLE(automatic_scheme, automatic_scheme_t) {
   {"alternate", SCHEME_ALTERNATE},
   {"longest_side", SCHEME_LONGEST_SIDE},
   {"spiral", SCHEME_SPIRAL}
};

LOOKUP_TABLE(tightness, tightness_t) {
   {"high", TIGHTNESS_HIGH},
   {"low", TIGHTNESS_LOW}
};

// generate binary search functions
BINARY_SEARCH_PARSER(split_type, split_type_t)
BINARY_SEARCH_PARSER(split_mode, split_mode_t)
BINARY_SEARCH_PARSER(layout, layout_t)
BINARY_SEARCH_PARSER(client_state, client_state_t)
BINARY_SEARCH_PARSER(stack_layer, stack_layer_t)
BINARY_SEARCH_PARSER(direction, direction_t)
BINARY_SEARCH_PARSER(cycle_direction, cycle_dir_t)
BINARY_SEARCH_PARSER(circulate_direction, circulate_dir_t)
BINARY_SEARCH_PARSER(history_direction, history_dir_t)
BINARY_SEARCH_PARSER(flip, flip_t)
BINARY_SEARCH_PARSER(resize_handle, resize_handle_t)
BINARY_SEARCH_PARSER(pointer_action, pointer_action_t)
BINARY_SEARCH_PARSER(child_polarity, child_polarity_t)
BINARY_SEARCH_PARSER(automatic_scheme, automatic_scheme_t)
BINARY_SEARCH_PARSER(tightness, tightness_t)

// optimized bool parser - check first char
bool parse_bool(char *value, bool *b)
{
   if (!value) return false;

   switch (value[0]) {
       case 't':
           if (value[1] == 'r' && value[2] == 'u' &&
               value[3] == 'e' && value[4] == '\0') {
               *b = true;
               return true;
           }
           break;
       case 'o':
           if (value[1] == 'n' && value[2] == '\0') {
               *b = true;
               return true;
           } else if (value[1] == 'f' && value[2] == 'f' && value[3] == '\0') {
               *b = false;
               return true;
           }
           break;
       case 'f':
           if (value[1] == 'a' && value[2] == 'l' &&
               value[3] == 's' && value[4] == 'e' && value[5] == '\0') {
               *b = false;
               return true;
           }
           break;
   }
   return false;
}

bool parse_honor_size_hints_mode(char *s, honor_size_hints_mode_t *a)
{
   bool b;
   if (parse_bool(s, &b)) {
       *a = b ? HONOR_SIZE_HINTS_YES : HONOR_SIZE_HINTS_NO;
       return true;
   }

   if (s[0] == 'f' && strcmp(s, "floating") == 0) {
       *a = HONOR_SIZE_HINTS_FLOATING;
       return true;
   } else if (s[0] == 't' && strcmp(s, "tiled") == 0) {
       *a = HONOR_SIZE_HINTS_TILED;
       return true;
   }
   return false;
}

// Optimized state transition parser
bool parse_state_transition(char *s, state_transition_t *m)
{
   if (s[0] == 'n' && strcmp(s, "none") == 0) {
       *m = 0;
       return true;
   } else if (s[0] == 'a' && strcmp(s, "all") == 0) {
       *m = STATE_TRANSITION_ENTER | STATE_TRANSITION_EXIT;
       return true;
   }

   state_transition_t w = 0;
   char *x = copy_string(s, strlen(s));
   char *key = strtok(x, ",");

   while (key != NULL) {
       if (key[0] == 'e') {
           if (strcmp(key, "enter") == 0) {
               w |= STATE_TRANSITION_ENTER;
           } else if (strcmp(key, "exit") == 0) {
               w |= STATE_TRANSITION_EXIT;
           } else {
               free(x);
               return false;
           }
       } else {
           free(x);
           return false;
       }
       key = strtok(NULL, ",");
   }

   free(x);
   return w != 0 ? (*m = w, true) : false;
}

// Lookup table for valid degrees
static const bool valid_degrees[360] = {
   [0] = true, [90] = true, [180] = true, [270] = true
};

bool parse_degree(char *s, int *d)
{
   int i = atoi(s);
   i = ((i % 360) + 360) % 360;

   if (valid_degrees[i]) {
       *d = i;
       return true;
   }
   return false;
}

bool parse_id(char *s, uint32_t *id)
{
   char *end;
   errno = 0;
   uint32_t v = strtol(s, &end, 0);
   if (errno != 0 || *end != '\0') {
       return false;
   }
   *id = v;
   return true;
}

bool parse_bool_declaration(char *s, char **key, bool *value, alter_state_t *state)
{
   *key = strtok(s, EQL_TOK);
   char *v = strtok(NULL, EQL_TOK);
   if (v == NULL) {
       *state = ALTER_TOGGLE;
       return true;
   } else {
       if (parse_bool(v, value)) {
           *state = ALTER_SET;
           return true;
       }
   }
   return false;
}

bool parse_index(char *s, uint16_t *idx)
{
   if (*s == '^') {
       char *end;
       unsigned long val = strtoul(s + 1, &end, 10);
       if (*end == '\0' && val <= UINT16_MAX) {
           *idx = (uint16_t)val;
           return true;
       }
   }
   return false;
}

// optimized rectangle parser - manual parsing instead of sscanf
bool parse_rectangle(char *s, bspwm_rect_t *r)
{
   char *end;

   unsigned long w = strtoul(s, &end, 10);
   if (*end != 'x' || w == 0 || w > UINT16_MAX) return false;
   s = end + 1;

   unsigned long h = strtoul(s, &end, 10);
   if (*end != '+' || h == 0 || h > UINT16_MAX) return false;
   s = end + 1;

   long x = strtol(s, &end, 10);
   if (*end != '+' || x < INT16_MIN || x > INT16_MAX) return false;
   s = end + 1;

   long y = strtol(s, &end, 10);
   if (*end != '\0' || y < INT16_MIN || y > INT16_MAX) return false;

   r->width = (uint16_t)w;
   r->height = (uint16_t)h;
   r->x = (int16_t)x;
   r->y = (int16_t)y;

   return true;
}

// modifier mask lookup table
static const struct {
   const char *key;
   uint16_t mask;
} modifier_table[] = {
   {"control", BSP_MOD_MASK_CONTROL},
   {"lock", BSP_MOD_MASK_LOCK},
   {"mod1", BSP_MOD_MASK_1},
   {"mod2", BSP_MOD_MASK_2},
   {"mod3", BSP_MOD_MASK_3},
   {"mod4", BSP_MOD_MASK_4},
   {"mod5", BSP_MOD_MASK_5},
   {"shift", BSP_MOD_MASK_SHIFT}
};

bool parse_modifier_mask(char *s, uint16_t *m)
{
   int left = 0, right = sizeof(modifier_table)/sizeof(modifier_table[0]) - 1;

   while (left <= right) {
       int mid = (left + right) >> 1;
       int cmp = strcmp(s, modifier_table[mid].key);
       if (cmp == 0) {
           *m = modifier_table[mid].mask;
           return true;
       }
       left = cmp > 0 ? mid + 1 : left;
       right = cmp < 0 ? mid - 1 : right;
   }
   return false;
}

bool parse_button_index(char *s, int8_t *b)
{
   switch (s[0]) {
       case 'a':
           if (strcmp(s, "any") == 0) {
               *b = BSP_BUTTON_ANY;
               return true;
           }
           break;
       case 'b':
           if (s[1] == 'u' && s[2] == 't' && s[3] == 't' && s[4] == 'o' && s[5] == 'n') {
               if (s[6] >= '1' && s[6] <= '3' && s[7] == '\0') {
                   *b = s[6] - '0';
                   return true;
               }
           }
           break;
       case 'n':
           if (strcmp(s, "none") == 0) {
               *b = -1;
               return true;
           }
           break;
   }
   return false;
}

// Subscriber mask sorted table
static const struct {
   const char *name;
   subscriber_mask_t mask;
} subscriber_table[] = {
   {"all", SBSC_MASK_ALL},
   {"desktop", SBSC_MASK_DESKTOP},
   {"desktop_activate", SBSC_MASK_DESKTOP_ACTIVATE},
   {"desktop_add", SBSC_MASK_DESKTOP_ADD},
   {"desktop_focus", SBSC_MASK_DESKTOP_FOCUS},
   {"desktop_layout", SBSC_MASK_DESKTOP_LAYOUT},
   {"desktop_remove", SBSC_MASK_DESKTOP_REMOVE},
   {"desktop_rename", SBSC_MASK_DESKTOP_RENAME},
   {"desktop_swap", SBSC_MASK_DESKTOP_SWAP},
   {"desktop_transfer", SBSC_MASK_DESKTOP_TRANSFER},
   {"monitor", SBSC_MASK_MONITOR},
   {"monitor_add", SBSC_MASK_MONITOR_ADD},
   {"monitor_focus", SBSC_MASK_MONITOR_FOCUS},
   {"monitor_geometry", SBSC_MASK_MONITOR_GEOMETRY},
   {"monitor_remove", SBSC_MASK_MONITOR_REMOVE},
   {"monitor_rename", SBSC_MASK_MONITOR_RENAME},
   {"monitor_swap", SBSC_MASK_MONITOR_SWAP},
   {"node", SBSC_MASK_NODE},
   {"node_activate", SBSC_MASK_NODE_ACTIVATE},
   {"node_add", SBSC_MASK_NODE_ADD},
   {"node_flag", SBSC_MASK_NODE_FLAG},
   {"node_focus", SBSC_MASK_NODE_FOCUS},
   {"node_geometry", SBSC_MASK_NODE_GEOMETRY},
   {"node_layer", SBSC_MASK_NODE_LAYER},
   {"node_presel", SBSC_MASK_NODE_PRESEL},
   {"node_remove", SBSC_MASK_NODE_REMOVE},
   {"node_stack", SBSC_MASK_NODE_STACK},
   {"node_state", SBSC_MASK_NODE_STATE},
   {"node_swap", SBSC_MASK_NODE_SWAP},
   {"node_transfer", SBSC_MASK_NODE_TRANSFER},
   {"pointer_action", SBSC_MASK_POINTER_ACTION},
   {"report", SBSC_MASK_REPORT}
};

bool parse_subscriber_mask(char *s, subscriber_mask_t *mask)
{
   int left = 0, right = sizeof(subscriber_table)/sizeof(subscriber_table[0]) - 1;

   while (left <= right) {
       int mid = (left + right) >> 1;
       int cmp = strcmp(s, subscriber_table[mid].name);
       if (cmp == 0) {
           *mask = subscriber_table[mid].mask;
           return true;
       }
       left = cmp > 0 ? mid + 1 : left;
       right = cmp < 0 ? mid - 1 : right;
   }
   return false;
}

// Modifier parsing using string interning
#define PARSE_MODIFIER(name, field) \
   if (strcmp(#field, tok) == 0) { \
       sel->field = OPTION_TRUE; \
   } else if (tok[0] == '!' && strcmp(#field, tok + 1) == 0) { \
       sel->field = OPTION_FALSE; \
   }

bool parse_monitor_modifiers(char *desc, monitor_select_t *sel)
{
   char *tok;
   while ((tok = strrchr(desc, CAT_CHR)) != NULL) {
       tok[0] = '\0';
       tok++;

       if (strcmp("occupied", tok) == 0) {
           sel->occupied = OPTION_TRUE;
       } else if (tok[0] == '!' && strcmp("occupied", tok + 1) == 0) {
           sel->occupied = OPTION_FALSE;
       } else PARSE_MODIFIER(monitor, focused)
       else {
           return false;
       }
   }
   return true;
}

bool parse_desktop_modifiers(char *desc, desktop_select_t *sel)
{
   char *tok;
   while ((tok = strrchr(desc, CAT_CHR)) != NULL) {
       tok[0] = '\0';
       tok++;

       if (strcmp("occupied", tok) == 0) {
           sel->occupied = OPTION_TRUE;
       } else if (tok[0] == '!' && strcmp("occupied", tok + 1) == 0) {
           sel->occupied = OPTION_FALSE;
       }
       else PARSE_MODIFIER(desktop, focused)
       else PARSE_MODIFIER(desktop, active)
       else PARSE_MODIFIER(desktop, urgent)
       else PARSE_MODIFIER(desktop, local)
       else PARSE_MODIFIER(desktop, tiled)
       else PARSE_MODIFIER(desktop, monocle)
       else PARSE_MODIFIER(desktop, user_tiled)
       else PARSE_MODIFIER(desktop, user_monocle)
       else {
           return false;
       }
   }
   return true;
}

bool parse_node_modifiers(char *desc, node_select_t *sel)
{
   char *tok;
   while ((tok = strrchr(desc, CAT_CHR)) != NULL) {
       tok[0] = '\0';
       tok++;

       if (strcmp("tiled", tok) == 0) {
           sel->tiled = OPTION_TRUE;
       } else if (tok[0] == '!' && strcmp("tiled", tok + 1) == 0) {
           sel->tiled = OPTION_FALSE;
       }
       else PARSE_MODIFIER(node, automatic)
       else PARSE_MODIFIER(node, focused)
       else PARSE_MODIFIER(node, active)
       else PARSE_MODIFIER(node, local)
       else PARSE_MODIFIER(node, leaf)
       else PARSE_MODIFIER(node, window)
       else PARSE_MODIFIER(node, pseudo_tiled)
       else PARSE_MODIFIER(node, floating)
       else PARSE_MODIFIER(node, fullscreen)
       else PARSE_MODIFIER(node, hidden)
       else PARSE_MODIFIER(node, sticky)
       else PARSE_MODIFIER(node, private)
       else PARSE_MODIFIER(node, locked)
       else PARSE_MODIFIER(node, marked)
       else PARSE_MODIFIER(node, urgent)
       else PARSE_MODIFIER(node, same_class)
       else PARSE_MODIFIER(node, descendant_of)
       else PARSE_MODIFIER(node, ancestor_of)
       else PARSE_MODIFIER(node, below)
       else PARSE_MODIFIER(node, normal)
       else PARSE_MODIFIER(node, above)
       else PARSE_MODIFIER(node, horizontal)
       else PARSE_MODIFIER(node, vertical)
       else {
           return false;
       }
   }
   return true;
}
