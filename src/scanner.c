#include <string.h>  // memcpy()
#include <wctype.h>  // iswspace()

#include "tree_sitter/parser.h"

// Uncomment if debugging for extra output during parsing. Note that we can't
// use `vprintf()` for print debugging in WASM or on CRAN for the R package!
// #define TREE_SITTER_R_DEBUG

#ifdef TREE_SITTER_R_DEBUG
#include <stdarg.h>  // va_list, va_start(), va_end()
#include <stdio.h>   // vprintf()

static inline void debug_print(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}
#else
#define debug_print(...)
#endif

enum TokenType {
  NEWLINE,
  SEMICOLON,
  RAW_STRING_LITERAL,
  ELSE,
  OPEN_PAREN,
  CLOSE_PAREN,
  OPEN_BRACE,
  CLOSE_BRACE,
  OPEN_BRACKET,
  CLOSE_BRACKET,
  OPEN_BRACKET2,
  CLOSE_BRACKET2
};

// ---------------------------------------------------------------------------------------
// Temporary Stack structure until we can use the `<tree_sitter/array.h>` header from
// tree sitter 1.0.0. Inspired from tree-sitter-julia.

typedef char Scope;

const Scope SCOPE_TOP_LEVEL = 0;
const Scope SCOPE_BRACE = 1;
const Scope SCOPE_PAREN = 2;
const Scope SCOPE_BRACKET = 3;
const Scope SCOPE_BRACKET2 = 4;

// A `Stack` data structure for tracking the current `Scope`
//
// `SCOPE_TOP_LEVEL` is never actually pushed onto the stack. It is returned from
// `stack_peek()` as a base case when `len = 0`. Note that in `stack_pop()` we still check
// for `len > 0` before peeking to retain the invariant that we can't pop without
// something on the stack.
//
// This actually makes serialization/deserialization very simple. Even if we pushed an
// initial `SCOPE_TOP_LEVEL` in the create hook, there is no guarantee that that will get
// serialized (so the length of the stack won't be remembered) because the serialize hook
// only runs when we accept a token from our external scanner. That would complicate the
// deserialize hook by forcing us to differentiate between the cases of:
// 1) A deserialization call restoring state from a previous serialization (len > 0).
// 2) A deserialization call when there wasn't a previous serialization (len = 0), where
//    we'd have to repush an initial `SCOPE_TOP_LEVEL`.
typedef struct {
  Scope* arr;
  unsigned len;
} Stack;

static Stack* stack_new(void) {
  Scope* arr = malloc(TREE_SITTER_SERIALIZATION_BUFFER_SIZE);
  if (arr == NULL) exit(1);
  Stack* stack = malloc(sizeof(Stack));
  if (stack == NULL) exit(1);
  stack->arr = arr;
  stack->len = 0;
  return stack;
}

static void stack_free(Stack* stack) {
  free(stack->arr);
  free(stack);
}

static bool stack_push(Stack* stack, Scope scope) {
  if (stack->len >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    // Return `false` so `scan()` can return `false` and refuse to handle the token.
    // Should only ever happen in pathological cases (i.e. 1025 unmatched opening braces).
    debug_print("`stack_push()` failed. Stack is at maximum capacity.\n");
    return false;
  }

  stack->arr[stack->len] = scope;
  stack->len++;

  return true;
}

static Scope stack_peek(Stack* stack) {
  if (stack->len == 0) {
    return SCOPE_TOP_LEVEL;
  } else {
    return stack->arr[stack->len - 1];
  }
}

static bool stack_pop(Stack* stack, Scope scope) {
  if (stack->len == 0) {
    // Return `false` so `scan()` can return `false` and refuse to handle the token
    debug_print("`stack_pop()` failed. Stack is empty, nothing to pop.\n");
    return false;
  }

  Scope x = stack_peek(stack);
  stack->len--;

  if (x != scope) {
    // Return `false` so `scan()` can return `false` and refuse to handle the token
    debug_print(
        "`stack_pop()` failed. Actual scope '%c' does not match expected scope '%c'.\n",
        x,
        scope
    );
    return false;
  }

  return true;
}

static unsigned stack_serialize(Stack* stack, char* buffer) {
  unsigned len = stack->len;
  if (len > 0) {
    memcpy(buffer, stack->arr, len);
  }
  return len;
}

static void stack_deserialize(Stack* stack, const char* buffer, unsigned len) {
  if (len > 0) {
    memcpy(stack->arr, buffer, len);
  }
  stack->len = len;
}

// ---------------------------------------------------------------------------------------

static void consume_whitespace_and_ignored_newlines(TSLexer* lexer, Stack* stack) {
  while (iswspace(lexer->lookahead)) {
    if (lexer->lookahead != '\n') {
      // Consume all spaces, tabs, etc, unconditionally
      lexer->advance(lexer, true);
      continue;
    }

    // If we are inside `(`, `[`, or `[[`, we consume newlines unconditionally.
    // Notably not within `{` nor at "top level", where newlines have contextual
    // meaning, particularly for `if` statements. Both of those are handled elsewhere.
    Scope scope = stack_peek(stack);
    if (scope == SCOPE_PAREN || scope == SCOPE_BRACKET || scope == SCOPE_BRACKET2) {
      lexer->advance(lexer, true);
      continue;
    }

    // We've hit a newline with contextual meaning to be handled elsewhere
    break;
  }
}

static bool scan_else(TSLexer* lexer) {
  if (lexer->lookahead != 'e') {
    return false;
  }

  lexer->advance(lexer, false);
  if (lexer->lookahead != 'l') {
    return false;
  }

  lexer->advance(lexer, false);
  if (lexer->lookahead != 's') {
    return false;
  }

  lexer->advance(lexer, false);
  if (lexer->lookahead != 'e') {
    return false;
  }

  // We found `else`, return special `external` for it
  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  lexer->result_symbol = ELSE;

  return true;
}

// Due to `consume_whitespace_and_ignored_newlines()`, expect that we are either in
// a `SCOPE_TOP_LEVEL` or a `SCOPE_BRACE` if we saw a new line at this point.
static bool
scan_newline_or_else(TSLexer* lexer, Stack* stack, const bool* valid_symbols) {
  // Advance to the next non-newline, non-space character,
  // we know we have at least 1 newline because this function was called
  while (iswspace(lexer->lookahead)) {
    if (lexer->lookahead != '\n') {
      lexer->advance(lexer, true);
      continue;
    }

    lexer->advance(lexer, true);
    lexer->mark_end(lexer);
  }

  // If the next symbol is a comment, we go ahead and consume the newline as it won't
  // affect the context, and would otherwise interfere with a situation like below, as
  // the rogue newline would make it look like we exited the `if` statement, making a
  // potential `else` node "invalid" in terms of `valid_symbols`.
  //
  // if (cond) {
  // }
  // # comment
  // else {
  //
  // }
  if (lexer->lookahead == '#') {
    lexer->advance(lexer, true);
    return false;
  }

  // At this point the most recent newline is marked by `mark_end()`, so lock
  // it in as a result before giving the special `else` case a chance to run.
  lexer->result_symbol = NEWLINE;

  // If we are inside a `SCOPE_BRACE`, this is an extremely special case where `else`
  // can follow any number of newlines or whitespace and still be valid.
  if (valid_symbols[ELSE] && stack_peek(stack) == SCOPE_BRACE && scan_else(lexer)) {
    return true;
  }

  return true;
}

static bool scan_raw_string_literal(TSLexer* lexer) {
  // scan a raw string literal; see R source code for implementation:
  // https://github.com/wch/r-source/blob/52b730f217c12ba3d95dee0cd1f330d1977b5ea3/src/main/gram.y#L3102

  // raw string literals can start with either 'r' or 'R'
  lexer->mark_end(lexer);
  char prefix = lexer->lookahead;
  if (prefix != 'r' && prefix != 'R') {
    return false;
  }
  lexer->advance(lexer, false);

  // check for quote character
  char quote = lexer->lookahead;
  if (quote != '"' && quote != '\'') {
    return false;
  }
  lexer->advance(lexer, false);

  // start counting '-' characters
  int hyphen_count = 0;
  while (lexer->lookahead == '-') {
    lexer->advance(lexer, false);
    hyphen_count += 1;
  }

  // check for an opening bracket, and figure out
  // the corresponding closing bracket
  char opening_bracket = lexer->lookahead;
  char closing_bracket = 0;
  if (opening_bracket == '(') {
    closing_bracket = ')';
    lexer->advance(lexer, false);
  } else if (opening_bracket == '[') {
    closing_bracket = ']';
    lexer->advance(lexer, false);
  } else if (opening_bracket == '{') {
    closing_bracket = '}';
    lexer->advance(lexer, false);
  } else {
    return false;
  }

  // we're in the body of the raw string; start looping until
  // we find the matching closing bracket
  for (; lexer->lookahead != 0; lexer->advance(lexer, false)) {
    // consume a closing bracket
    if (lexer->lookahead != closing_bracket) {
      continue;
    }
    lexer->advance(lexer, false);

    // consume hyphens
    bool hyphens_ok = true;
    for (int i = 0; i < hyphen_count; i++) {
      if (lexer->lookahead != '-') {
        hyphens_ok = false;
        break;
      }
      lexer->advance(lexer, false);
    }

    if (!hyphens_ok) {
      continue;
    }

    // consume a closing quote character
    if (lexer->lookahead != quote) {
      continue;
    }
    lexer->advance(lexer, false);

    // success!
    lexer->mark_end(lexer);
    lexer->result_symbol = RAW_STRING_LITERAL;
    return true;
  }

  // if we get here, this implies we hit eof (and so we have
  // an unclosed raw string)
  return false;
}

static bool scan(TSLexer* lexer, Stack* stack, const bool* valid_symbols) {
  consume_whitespace_and_ignored_newlines(lexer, stack);

  // check for semi-colons
  if (valid_symbols[SEMICOLON] && lexer->lookahead == ';') {
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    lexer->result_symbol = SEMICOLON;
    return true;
  }

  // check for an open bracket
  if (valid_symbols[OPEN_PAREN] && lexer->lookahead == '(') {
    if (!stack_push(stack, SCOPE_PAREN)) {
      return false;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    lexer->result_symbol = OPEN_PAREN;
    return true;
  }

  if (valid_symbols[CLOSE_PAREN] && lexer->lookahead == ')') {
    if (!stack_pop(stack, SCOPE_PAREN)) {
      return false;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    lexer->result_symbol = CLOSE_PAREN;
    return true;
  }

  if (valid_symbols[OPEN_BRACE] && lexer->lookahead == '{') {
    if (!stack_push(stack, SCOPE_BRACE)) {
      return false;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    lexer->result_symbol = OPEN_BRACE;
    return true;
  }

  if (valid_symbols[CLOSE_BRACE] && lexer->lookahead == '}') {
    if (!stack_pop(stack, SCOPE_BRACE)) {
      return false;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    lexer->result_symbol = CLOSE_BRACE;
    return true;
  }

  if ((valid_symbols[OPEN_BRACKET] || valid_symbols[OPEN_BRACKET2]) &&
      lexer->lookahead == '[') {
    lexer->advance(lexer, false);

    // If we see `[[` when it's a valid symbol, greedily accept that
    if (valid_symbols[OPEN_BRACKET2] && lexer->lookahead == '[') {
      if (!stack_push(stack, SCOPE_BRACKET2)) {
        return false;
      }
      lexer->advance(lexer, false);
      lexer->mark_end(lexer);
      lexer->result_symbol = OPEN_BRACKET2;
      return true;
    }

    // If we see either `[` followed by something else, or `[[` when `[[` happens to
    // not be a valid symbol, accept the single `[` if it's a valid symbol.
    if (valid_symbols[OPEN_BRACKET]) {
      if (!stack_push(stack, SCOPE_BRACKET)) {
        return false;
      }
      lexer->mark_end(lexer);
      lexer->result_symbol = OPEN_BRACKET;
      return true;
    }

    // If we see a `[` that isn't captured by the above cases, we don't know how to
    // handle it
    return false;
  }

  if (valid_symbols[CLOSE_BRACKET] && lexer->lookahead == ']' &&
      stack_peek(stack) == SCOPE_BRACKET) {
    // Must check the scope before entering this branch to account for `x[[a[1]]]` where
    // the first `]` occurs when both `]` and `]]` are valid. The scope breaks the tie
    // in favor of this branch.
    if (!stack_pop(stack, SCOPE_BRACKET)) {
      return false;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    lexer->result_symbol = CLOSE_BRACKET;
    return true;
  }

  if (valid_symbols[CLOSE_BRACKET2] && lexer->lookahead == ']' &&
      stack_peek(stack) == SCOPE_BRACKET2) {
    // Must check the scope before entering this branch to account for `x[a[[1]]]` where
    // the first `]` occurs when both `]` and `]]` are valid. The scope breaks the tie
    // in favor of this branch.
    lexer->advance(lexer, false);
    if (lexer->lookahead != ']') {
      // Like `x[[1]` where we instead want an unmatched `]`
      return false;
    }
    if (!stack_pop(stack, SCOPE_BRACKET2)) {
      return false;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    lexer->result_symbol = CLOSE_BRACKET2;
    return true;
  }

  // There absolutely must not be any other conditions after these.
  // These functions `advance()` internally, so if they return `false` then we can't
  // check any other conditions after these because `lookahead` won't be accurate.
  if (valid_symbols[RAW_STRING_LITERAL] &&
      (lexer->lookahead == 'r' || lexer->lookahead == 'R')) {
    return scan_raw_string_literal(lexer);
  } else if (valid_symbols[ELSE] && lexer->lookahead == 'e') {
    return scan_else(lexer);
  } else if (valid_symbols[NEWLINE] && lexer->lookahead == '\n') {
    return scan_newline_or_else(lexer, stack, valid_symbols);
  }

  return false;
}

// ---------------------------------------------------------------------------------------

void* tree_sitter_r_external_scanner_create(void) {
  return stack_new();
}

bool tree_sitter_r_external_scanner_scan(
    void* payload,
    TSLexer* lexer,
    const bool* valid_symbols
) {
  return scan(lexer, payload, valid_symbols);
}

unsigned tree_sitter_r_external_scanner_serialize(void* payload, char* buffer) {
  return stack_serialize(payload, buffer);
}

void tree_sitter_r_external_scanner_deserialize(
    void* payload,
    const char* buffer,
    unsigned length
) {
  stack_deserialize(payload, buffer, length);
}

void tree_sitter_r_external_scanner_destroy(void* payload) {
  stack_free(payload);
}
