/*
  Copyright 2011 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serd_internal.h"

#define NS_XSD "http://www.w3.org/2001/XMLSchema#"
#define NS_RDF "http://www.w3.org/1999/02/22-rdf-syntax-ns#"

#define TRY_THROW(exp) if (!(exp)) goto except;
#define TRY_RET(exp)   if (!(exp)) return 0;

#define STACK_PAGE_SIZE 4096
#define READ_BUF_LEN    4096

typedef struct {
	const uint8_t* filename;
	unsigned       line;
	unsigned       col;
} Cursor;

typedef uint32_t uchar;

typedef size_t Ref;

typedef struct {
	SerdType type;
	Ref      value;
} Node;

typedef struct {
	const Node*         graph;
	const Node*         subject;
	const Node*         predicate;
	SerdStatementFlags* flags;
} ReadContext;

/** Measured UTF-8 string. */
typedef struct {
	size_t  n_bytes;  ///< Size in bytes
	size_t  n_chars;  ///< Length in characters
	uint8_t buf[];    ///< Buffer
} SerdString;

static const Node INTERNAL_NODE_NULL = { 0, 0 };

struct SerdReaderImpl {
	void*             handle;
	void              (*free_handle)(void* ptr);
	SerdBaseSink      base_sink;
	SerdPrefixSink    prefix_sink;
	SerdStatementSink statement_sink;
	SerdEndSink       end_sink;
	Node              rdf_type;
	Node              rdf_first;
	Node              rdf_rest;
	Node              rdf_nil;
	FILE*             fd;
	SerdStack         stack;
	SerdSyntax        syntax;
	Cursor            cur;
	uint8_t*          buf;
	uint8_t*          bprefix;
	size_t            bprefix_len;
	unsigned          next_id;
	int               err;
	uint8_t*          read_buf;
	int32_t           read_head;    ///< Offset into read_buf
	bool              from_file;    ///< True iff reading from @ref fd
	bool              eof;
#ifdef SERD_STACK_CHECK
	Ref*              alloc_stack;  ///< Stack of push offsets
	size_t            n_allocs;     ///< Number of stack pushes
#endif
};

static int
error(SerdReader* reader, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "error: %s:%u:%u: ",
	        reader->cur.filename, reader->cur.line, reader->cur.col);
	vfprintf(stderr, fmt, args);
	return 0;
}

static Node
make_node(SerdType type, Ref value)
{
	const Node ret = { type, value };
	return ret;
}

static inline bool
page(SerdReader* reader)
{
	assert(reader->from_file);
	reader->read_head = 0;
	const size_t n_read = fread(reader->read_buf, 1, READ_BUF_LEN, reader->fd);
	if (n_read == 0) {
		reader->read_buf[0] = '\0';
		reader->eof = true;
		return false;
	} else if (n_read < READ_BUF_LEN) {
		reader->read_buf[n_read] = '\0';
	}
	return true;
}

static inline bool
peek_string(SerdReader* reader, uint8_t* pre, int n)
{
	uint8_t* ptr = reader->read_buf + reader->read_head;
	for (int i = 0; i < n; ++i) {
		if (reader->from_file && (reader->read_head + i >= READ_BUF_LEN)) {
			if (!page(reader)) {
				return false;
			}
			ptr = reader->read_buf;
			reader->read_head = -i;
			memcpy(reader->read_buf + reader->read_head, pre, i);
			assert(reader->read_buf[reader->read_head] == pre[0]);
		}
		if ((pre[i] = *ptr++) == '\0') {
			return false;
		}
	}
	return true;
}

static inline uint8_t
peek_byte(SerdReader* reader)
{
	return reader->read_buf[reader->read_head];
}

static inline uint8_t
eat_byte(SerdReader* reader, const uint8_t byte)
{
	const uint8_t c = peek_byte(reader);
	++reader->read_head;
	switch (c) {
	case '\n': ++reader->cur.line; reader->cur.col = 0; break;
	default:   ++reader->cur.col;
	}

	if (c != byte) {
		return error(reader, "expected `%c', not `%c'\n", byte, c);
	}
	if (reader->from_file && (reader->read_head == READ_BUF_LEN)) {
		TRY_RET(page(reader));
		assert(reader->read_head < READ_BUF_LEN);
	}
	if (reader->read_buf[reader->read_head] == '\0') {
		reader->eof = true;
	}
	return c;
}

static inline void
eat_string(SerdReader* reader, const char* str, unsigned n)
{
	for (unsigned i = 0; i < n; ++i) {
		eat_byte(reader, ((const uint8_t*)str)[i]);
	}
}

#ifdef SERD_STACK_CHECK
static inline bool
stack_is_top_string(SerdReader* reader, Ref ref)
{
	return ref == reader->alloc_stack[reader->n_allocs - 1];
}
#endif

// Make a new string from a non-UTF-8 C string (internal use only)
static Ref
push_string(SerdReader* reader, const char* c_str, size_t n_bytes)
{
	uint8_t* mem = serd_stack_push(&reader->stack,
	                               sizeof(SerdString) + n_bytes + 1);
	SerdString* const str = (SerdString*)mem;
	str->n_bytes = n_bytes;
	str->n_chars = n_bytes;
	memcpy(str->buf, c_str, n_bytes + 1);
#ifdef SERD_STACK_CHECK
	reader->alloc_stack = realloc(
		reader->alloc_stack, sizeof(uint8_t*) * (++reader->n_allocs));
	reader->alloc_stack[reader->n_allocs - 1] = (mem - reader->stack.buf);
#endif
	return (uint8_t*)str - reader->stack.buf;
}

static Node
push_uri(SerdReader* reader, const char* str, size_t len)
{
	return make_node(SERD_URI, push_string(reader, str, len));
}

static inline SerdString*
deref(SerdReader* reader, const Ref ref)
{
	if (ref) {
		return (SerdString*)(reader->stack.buf + ref);
	}
	return NULL;
}

static inline void
push_byte(SerdReader* reader, Ref ref, const uint8_t c)
{
	#ifdef SERD_STACK_CHECK
	assert(stack_is_top_string(reader, ref));
	#endif
	serd_stack_push(&reader->stack, 1);
	SerdString* const str = deref(reader, ref);
	++str->n_bytes;
	if ((c & 0xC0) != 0x80) {
		// Does not start with `10', start of a new character
		++str->n_chars;
	}
	assert(str->n_bytes >= str->n_chars);
	str->buf[str->n_bytes - 1] = c;
	str->buf[str->n_bytes]     = '\0';
}

static inline void
append_string(SerdReader* reader, Ref ref, const uint8_t* suffix, size_t len)
{
	#ifdef SERD_STACK_CHECK
	assert(stack_is_top_string(reader, ref));
	#endif
	serd_stack_push(&reader->stack, len);
	SerdString* const str = deref(reader, ref);
	assert(str->n_bytes >= str->n_chars);
	memcpy(str->buf + str->n_bytes, suffix, len + 1);
	str->n_bytes += len;
	str->n_chars += len;
}

static void
pop_string(SerdReader* reader, Ref ref)
{
	if (ref) {
		if (ref == reader->rdf_nil.value
		    || ref == reader->rdf_first.value
		    || ref == reader->rdf_rest.value) {
			return;
		}
		#ifdef SERD_STACK_CHECK
		if (!stack_is_top_string(reader, ref)) {
			fprintf(stderr, "Attempt to pop non-top string %s\n",
			        deref(reader, ref)->buf);
		}
		assert(stack_is_top_string(reader, ref));
		--reader->n_allocs;
		#endif
		SerdString* str = deref(reader, ref);
		serd_stack_pop(&reader->stack, sizeof(SerdString) + str->n_bytes + 1);
	}
}

static inline SerdNode
public_node_from_ref(SerdReader* reader, SerdType type, Ref ref)
{
	if (!ref) {
		return SERD_NODE_NULL;
	}
	const SerdString* str  = deref(reader, ref);
	const SerdNode    node = { str->buf, str->n_bytes, str->n_chars, 0, type };
	return node;
}

static inline SerdNode
public_node(SerdReader* reader, const Node* private)
{
	if (private) {
		return public_node_from_ref(reader, private->type, private->value);
	} else {
		return SERD_NODE_NULL;
	}
}

static inline bool
emit_statement(SerdReader* reader, SerdStatementFlags* flags,
               const Node* g, const Node* s, const Node* p, const Node* o,
               const Node* d, Ref l, uint32_t f)
{
	assert(s && p && o);
	assert(s->value && p->value && o->value);
	const SerdNode graph     = public_node(reader, g);
	const SerdNode subject   = public_node(reader, s);
	const SerdNode predicate = public_node(reader, p);
	SerdNode       object    = public_node(reader, o);
	const SerdNode datatype  = public_node(reader, d);
	const SerdNode lang      = public_node_from_ref(reader, SERD_LITERAL, l);
	object.flags = f;
	bool ret = !reader->statement_sink
		|| !reader->statement_sink(reader->handle,
		                           *flags,
		                           &graph,
		                           &subject,
		                           &predicate,
		                           &object,
		                           &datatype,
		                           &lang);
	*flags = (*flags & SERD_ANON_CONT) ? SERD_ANON_CONT : 0;
	return ret;
}

static bool
read_collection(SerdReader* reader, ReadContext ctx, Node* dest);

static bool
read_predicateObjectList(SerdReader* reader, ReadContext ctx, bool blank);

// [40]	hex	::=	[#x30-#x39] | [#x41-#x46]
static inline uint8_t
read_hex(SerdReader* reader)
{
	const uint8_t c = peek_byte(reader);
	if (in_range(c, 0x30, 0x39) || in_range(c, 0x41, 0x46)) {
		return eat_byte(reader, c);
	} else {
		return error(reader, "illegal hexadecimal digit `%c'\n", c);
	}
}

static inline bool
read_hex_escape(SerdReader* reader, unsigned length, Ref dest)
{
	uint8_t buf[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	for (unsigned i = 0; i < length; ++i) {
		buf[i] = read_hex(reader);
	}

	uint32_t c;
	sscanf((const char*)buf, "%X", &c);

	unsigned size = 0;
	if (c < 0x00000080) {
		size = 1;
	} else if (c < 0x00000800) {
		size = 2;
	} else if (c < 0x00010000) {
		size = 3;
	} else if (c < 0x00200000) {
		size = 4;
	} else {
		return false;
	}

	// Build output in buf
	// (Note # of bytes = # of leading 1 bits in first byte)
	switch (size) {
	case 4:
		buf[3] = 0x80 | (uint8_t)(c & 0x3F);
		c >>= 6;
		c |= (16 << 12);  // set bit 4
	case 3:
		buf[2] = 0x80 | (uint8_t)(c & 0x3F);
		c >>= 6;
		c |= (32 << 6);  // set bit 5
	case 2:
		buf[1] = 0x80 | (uint8_t)(c & 0x3F);
		c >>= 6;
		c |= 0xC0;  // set bits 6 and 7
	case 1:
		buf[0] = (uint8_t)c;
	}

	for (unsigned i = 0; i < size; ++i) {
		push_byte(reader, dest, buf[i]);
	}
	return true;
}

static inline bool
read_character_escape(SerdReader* reader, Ref dest)
{
	switch (peek_byte(reader)) {
	case '\\':
		push_byte(reader, dest, eat_byte(reader, '\\'));
		return true;
	case 'u':
		eat_byte(reader, 'u');
		return read_hex_escape(reader, 4, dest);
	case 'U':
		eat_byte(reader, 'U');
		return read_hex_escape(reader, 8, dest);
	default:
		return false;
	}
}

static inline bool
read_echaracter_escape(SerdReader* reader, Ref dest, SerdNodeFlags* flags)
{
	switch (peek_byte(reader)) {
	case 't':
		eat_byte(reader, 't');
		push_byte(reader, dest, '\t');
		return true;
	case 'n':
		*flags |= SERD_HAS_NEWLINE;
		eat_byte(reader, 'n');
		push_byte(reader, dest, '\n');
		return true;
	case 'r':
		*flags |= SERD_HAS_NEWLINE;
		eat_byte(reader, 'r');
		push_byte(reader, dest, '\r');
		return true;
	default:
		return read_character_escape(reader, dest);
	}
}

static inline bool
read_scharacter_escape(SerdReader* reader, Ref dest, SerdNodeFlags* flags)
{
	switch (peek_byte(reader)) {
	case '"':
		*flags |= SERD_HAS_QUOTE;
		push_byte(reader, dest, eat_byte(reader, '"'));
		return true;
	default:
		return read_echaracter_escape(reader, dest, flags);
	}
}

static inline bool
read_ucharacter_escape(SerdReader* reader, Ref dest)
{
	SerdNodeFlags flags = 0;
	switch (peek_byte(reader)) {
	case '>':
		push_byte(reader, dest, eat_byte(reader, '>'));
		return true;
	default:
		return read_echaracter_escape(reader, dest, &flags);
	}
}

// [38] character ::= '\u' hex hex hex hex
//    | '\U' hex hex hex hex hex hex hex hex
//    | '\\'
//    | [#x20-#x5B] | [#x5D-#x10FFFF]
static inline SerdStatus
read_character(SerdReader* reader, Ref dest)
{
	const uint8_t c = peek_byte(reader);
	assert(c != '\\');  // Only called from methods that handle escapes first
	switch (c) {
	case '\0':
		error(reader, "unexpected end of file\n", peek_byte(reader));
		return SERD_ERR_BAD_SYNTAX;
	default:
		if (c < 0x20) {  // ASCII control character
			error(reader, "unexpected control character\n");
			return SERD_ERR_BAD_SYNTAX;
		} else if (c <= 0x7E) {  // Printable ASCII
			push_byte(reader, dest, eat_byte(reader, c));
			return SERD_SUCCESS;
		} else {  // Wide UTF-8 character
			unsigned size = 1;
			if ((c & 0xE0) == 0xC0) {  // Starts with `110'
				size = 2;
			} else if ((c & 0xF0) == 0xE0) {  // Starts with `1110'
				size = 3;
			} else if ((c & 0xF8) == 0xF0) {  // Starts with `11110'
				size = 4;
			} else {
				error(reader, "invalid character\n");
				return SERD_ERR_BAD_SYNTAX;
			}
			for (unsigned i = 0; i < size; ++i) {
				push_byte(reader, dest, eat_byte(reader, peek_byte(reader)));
			}
			return SERD_SUCCESS;
		}
	}
}

// [39] echaracter ::= character | '\t' | '\n' | '\r'
static inline SerdStatus
read_echaracter(SerdReader* reader, Ref dest)
{
	SerdNodeFlags flags = 0;
	uint8_t       c     = peek_byte(reader);
	switch (c) {
	case '\\':
		eat_byte(reader, '\\');
		if (read_echaracter_escape(reader, peek_byte(reader), &flags)) {
			return SERD_SUCCESS;
		} else {
			error(reader, "illegal escape `\\%c'\n", peek_byte(reader));
			return SERD_ERR_BAD_SYNTAX;
		}
	default:
		return read_character(reader, dest);
	}
}

// [43] lcharacter ::= echaracter | '\"' | #x9 | #xA | #xD
static inline SerdStatus
read_lcharacter(SerdReader* reader, Ref dest, SerdNodeFlags* flags)
{
	const uint8_t c = peek_byte(reader);
	uint8_t       pre[4];
	switch (c) {
	case '"':
		peek_string(reader, pre, 4);
		if (pre[1] == '\"' && pre[2] == '\"' && pre[3] != '\"') {
			eat_byte(reader, '\"');
			eat_byte(reader, '\"');
			eat_byte(reader, '\"');
			return SERD_FAILURE;
		} else {
			*flags |= SERD_HAS_QUOTE;
			push_byte(reader, dest, eat_byte(reader, '"'));
			return SERD_SUCCESS;
		}
	case '\\':
		eat_byte(reader, '\\');
		if (read_scharacter_escape(reader, dest, flags)) {
			return SERD_SUCCESS;
		} else {
			error(reader, "illegal escape `\\%c'\n", peek_byte(reader));
			return SERD_ERR_BAD_SYNTAX;
		}
	case 0xA: case 0xD:
		*flags |= SERD_HAS_NEWLINE;
	case 0x9:
		push_byte(reader, dest, eat_byte(reader, c));
		return SERD_SUCCESS;
	default:
		return read_echaracter(reader, dest);
	}
}

// [42] scharacter ::= ( echaracter - #x22 ) | '\"'
static inline SerdStatus
read_scharacter(SerdReader* reader, Ref dest, SerdNodeFlags* flags)
{
	uint8_t c = peek_byte(reader);
	switch (c) {
	case '\\':
		eat_byte(reader, '\\');
		if (read_scharacter_escape(reader, dest, flags)) {
			return SERD_SUCCESS;
		} else {
			error(reader, "illegal escape `\\%c'\n", peek_byte(reader));
			return SERD_ERR_BAD_SYNTAX;
		}
	case '\"':
		return SERD_FAILURE;
	default:
		return read_character(reader, dest);
	}
}

// Spec: [41] ucharacter ::= ( character - #x3E ) | '\>'
// Impl: [41] ucharacter ::= ( echaracter - #x3E ) | '\>'
static inline SerdStatus
read_ucharacter(SerdReader* reader, Ref dest)
{
	const uint8_t c = peek_byte(reader);
	switch (c) {
	case '\\':
		eat_byte(reader, '\\');
		if (read_ucharacter_escape(reader, dest)) {
			return SERD_SUCCESS;
		} else {
			return error(reader, "illegal escape `\\%c'\n", peek_byte(reader));
		}
	case '>':
		return SERD_FAILURE;
	default:
		return read_character(reader, dest);
	}
}

// [10] comment ::= '#' ( [^#xA #xD] )*
static void
read_comment(SerdReader* reader)
{
	eat_byte(reader, '#');
	uint8_t c;
	while (((c = peek_byte(reader)) != 0xA) && (c != 0xD)) {
		eat_byte(reader, c);
	}
}

// [24] ws ::= #x9 | #xA | #xD | #x20 | comment
static inline bool
read_ws(SerdReader* reader)
{
	const uint8_t c = peek_byte(reader);
	switch (c) {
	case 0x9: case 0xA: case 0xD: case 0x20:
		eat_byte(reader, c);
		return true;
	case '#':
		read_comment(reader);
		return true;
	default:
		return false;
	}
}

static inline void
read_ws_star(SerdReader* reader)
{
	while (read_ws(reader)) {}
}

static inline bool
read_ws_plus(SerdReader* reader)
{
	TRY_RET(read_ws(reader));
	read_ws_star(reader);
	return true;
}

// [37] longString ::= #x22 #x22 #x22 lcharacter* #x22 #x22 #x22
static Ref
read_longString(SerdReader* reader, SerdNodeFlags* flags)
{
	eat_string(reader, "\"\"\"", 3);
	Ref        str = push_string(reader, "", 0);
	SerdStatus st;
	while (!(st = read_lcharacter(reader, str, flags))) {}
	if (st < SERD_ERR_UNKNOWN) {
		return str;
	}
	pop_string(reader, str);
	return 0;
}

// [36] string ::= #x22 scharacter* #x22
static Ref
read_string(SerdReader* reader, SerdNodeFlags* flags)
{
	eat_byte(reader, '\"');
	Ref        str = push_string(reader, "", 0);
	SerdStatus st;
	while (!(st = read_scharacter(reader, str, flags))) {}
	if (st < SERD_ERR_UNKNOWN) {
		eat_byte(reader, '\"');
		return str;
	}
	pop_string(reader, str);
	return 0;
}

// [35] quotedString ::= string | longString
static Ref
read_quotedString(SerdReader* reader, SerdNodeFlags* flags)
{
	uint8_t pre[3];
	peek_string(reader, pre, 3);
	assert(pre[0] == '\"');
	switch (pre[1]) {
	case '\"':
		if (pre[2] == '\"')
			return read_longString(reader, flags);
		else
			return read_string(reader, flags);
	default:
		return read_string(reader, flags);
	}
}

// [34] relativeURI ::= ucharacter*
static inline Ref
read_relativeURI(SerdReader* reader)
{
	Ref str = push_string(reader, "", 0);
	SerdStatus st;
	while (!(st = read_ucharacter(reader, str))) {}
	if (st < SERD_ERR_UNKNOWN) {
		return str;
	}
	pop_string(reader, str);
	return 0;
}

// [30] nameStartChar ::= [A-Z] | "_" | [a-z]
//    | [#x00C0-#x00D6] | [#x00D8-#x00F6] | [#x00F8-#x02FF] | [#x0370-#x037D]
//    | [#x037F-#x1FFF] | [#x200C-#x200D] | [#x2070-#x218F] | [#x2C00-#x2FEF]
//    | [#x3001-#xD7FF] | [#xF900-#xFDCF] | [#xFDF0-#xFFFD] | [#x10000-#xEFFFF]
static inline uchar
read_nameStartChar(SerdReader* reader, bool required)
{
	const uint8_t c = peek_byte(reader);
	if (c == '_' || is_alpha(c)) {  // TODO: not strictly correct
		return eat_byte(reader, c);
	} else {
		if (required) {
			error(reader, "illegal character `%c'\n", c);
		}
		return 0;
	}
}

// [31] nameChar ::= nameStartChar | '-' | [0-9]
//    | #x00B7 | [#x0300-#x036F] | [#x203F-#x2040]
static inline uchar
read_nameChar(SerdReader* reader)
{
	uchar c = read_nameStartChar(reader, false);
	if (c)
		return c;

	switch ((c = peek_byte(reader))) {
	case '-': case 0xB7: case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		return eat_byte(reader, c);
	default:  // TODO: 0x300-0x036F | 0x203F-0x2040
		return 0;
	}
	return 0;
}

// [33] prefixName ::= ( nameStartChar - '_' ) nameChar*
static Ref
read_prefixName(SerdReader* reader)
{
	uint8_t c = peek_byte(reader);
	if (c == '_') {
		error(reader, "unexpected `_'\n");
		return 0;
	}
	TRY_RET(c = read_nameStartChar(reader, false));
	Ref str = push_string(reader, "", 0);
	push_byte(reader, str, c);
	while ((c = read_nameChar(reader)) != 0) {
		push_byte(reader, str, c);
	}
	return str;
}

// [32] name ::= nameStartChar nameChar*
static Ref
read_name(SerdReader* reader, Ref dest, bool required)
{
	uchar c = read_nameStartChar(reader, required);
	if (!c) {
		if (required) {
			error(reader, "illegal character at start of name\n");
		}
		return 0;
	}
	do {
		push_byte(reader, dest, c);
	} while ((c = read_nameChar(reader)) != 0);
	return dest;
}

// [29] language ::= [a-z]+ ('-' [a-z0-9]+ )*
static Ref
read_language(SerdReader* reader)
{
	const uint8_t start = peek_byte(reader);
	if (!in_range(start, 'a', 'z')) {
		error(reader, "unexpected `%c'\n", start);
		return 0;
	}
	Ref str = push_string(reader, "", 0);
	push_byte(reader, str, eat_byte(reader, start));
	uint8_t c;
	while ((c = peek_byte(reader)) && in_range(c, 'a', 'z')) {
		push_byte(reader, str, eat_byte(reader, c));
	}
	while (peek_byte(reader) == '-') {
		push_byte(reader, str, eat_byte(reader, '-'));
		while ((c = peek_byte(reader)) && (
			       in_range(c, 'a', 'z') || in_range(c, '0', '9'))) {
			push_byte(reader, str, eat_byte(reader, c));
		}
	}
	return str;
}

// [28] uriref ::= '<' relativeURI '>'
static Ref
read_uriref(SerdReader* reader)
{
	TRY_RET(eat_byte(reader, '<'));
	Ref const str = read_relativeURI(reader);
	if (str && eat_byte(reader, '>')) {
		return str;
	}
	pop_string(reader, str);
	return 0;
}

// [27] qname ::= prefixName? ':' name?
static Ref
read_qname(SerdReader* reader)
{
	Ref prefix = read_prefixName(reader);
	if (!prefix) {
		prefix = push_string(reader, "", 0);
	}
	TRY_THROW(eat_byte(reader, ':'));
	push_byte(reader, prefix, ':');
	Ref str = read_name(reader, prefix, false);
	return str ? str : prefix;
except:
	pop_string(reader, prefix);
	return 0;
}

static bool
read_0_9(SerdReader* reader, Ref str, bool at_least_one)
{
	uint8_t c;
	if (at_least_one) {
		if (!is_digit((c = peek_byte(reader)))) {
			return error(reader, "expected digit\n");
		}
		push_byte(reader, str, eat_byte(reader, c));
	}
	while (is_digit((c = peek_byte(reader)))) {
		push_byte(reader, str, eat_byte(reader, c));
	}
	return true;
}

// [19] exponent ::= [eE] ('-' | '+')? [0-9]+
// [18] decimal ::= ( '-' | '+' )? ( [0-9]+ '.' [0-9]*
//                                  | '.' ([0-9])+
//                                  | ([0-9])+ )
// [17] double  ::= ( '-' | '+' )? ( [0-9]+ '.' [0-9]* exponent
//                                  | '.' ([0-9])+ exponent
//                                  | ([0-9])+ exponent )
// [16] integer ::= ( '-' | '+' ) ? [0-9]+
static bool
read_number(SerdReader* reader, Node* dest, Node* datatype)
{
	#define XSD_DECIMAL NS_XSD "decimal"
	#define XSD_DOUBLE  NS_XSD "double"
	#define XSD_INTEGER NS_XSD "integer"
	Ref     str         = push_string(reader, "", 0);
	uint8_t c           = peek_byte(reader);
	bool    has_decimal = false;
	if (c == '-' || c == '+') {
		push_byte(reader, str, eat_byte(reader, c));
	}
	if ((c = peek_byte(reader)) == '.') {
		has_decimal = true;
		// decimal case 2 (e.g. '.0' or `-.0' or `+.0')
		push_byte(reader, str, eat_byte(reader, c));
		TRY_THROW(read_0_9(reader, str, true));
	} else {
		// all other cases ::= ( '-' | '+' ) [0-9]+ ( . )? ( [0-9]+ )? ...
		TRY_THROW(read_0_9(reader, str, true));
		if ((c = peek_byte(reader)) == '.') {
			has_decimal = true;
			push_byte(reader, str, eat_byte(reader, c));
			TRY_THROW(read_0_9(reader, str, false));
		}
	}
	c = peek_byte(reader);
	if (c == 'e' || c == 'E') {
		// double
		push_byte(reader, str, eat_byte(reader, c));
		switch ((c = peek_byte(reader))) {
		case '+': case '-':
			push_byte(reader, str, eat_byte(reader, c));
		default: break;
		}
		read_0_9(reader, str, true);
		*datatype = push_uri(reader, XSD_DOUBLE, sizeof(XSD_DOUBLE) - 1);
	} else if (has_decimal) {
		*datatype = push_uri(reader, XSD_DECIMAL, sizeof(XSD_DECIMAL) - 1);
	} else {
		*datatype = push_uri(reader, XSD_INTEGER, sizeof(XSD_INTEGER) - 1);
	}
	*dest = make_node(SERD_LITERAL, str);
	assert(dest->value);
	return true;
except:
	pop_string(reader, datatype->value);
	pop_string(reader, str);
	return false;
}

// [25] resource ::= uriref | qname
static bool
read_resource(SerdReader* reader, Node* dest)
{
	switch (peek_byte(reader)) {
	case '<':
		*dest = make_node(SERD_URI, read_uriref(reader));
		break;
	default:
		*dest = make_node(SERD_CURIE, read_qname(reader));
	}
	return (dest->value != 0);
}

// [14] literal ::= quotedString ( '@' language )? | datatypeString
//    | integer | double | decimal | boolean
static bool
read_literal(SerdReader* reader, Node* dest,
             Node* datatype, Ref* lang, SerdNodeFlags* flags)
{
	Ref           str = 0;
	const uint8_t c   = peek_byte(reader);
	if (c == '-' || c == '+' || c == '.' || is_digit(c)) {
		return read_number(reader, dest, datatype);
	} else if (c == '\"') {
		str = read_quotedString(reader, flags);
		if (!str) {
			return false;
		}

		switch (peek_byte(reader)) {
		case '^':
			eat_byte(reader, '^');
			eat_byte(reader, '^');
			TRY_THROW(read_resource(reader, datatype));
			break;
		case '@':
			eat_byte(reader, '@');
			TRY_THROW(*lang = read_language(reader));
		}
		*dest = make_node(SERD_LITERAL, str);
	} else {
		return error(reader, "unknown literal type\n");
	}
	return true;
except:
	pop_string(reader, str);
	return false;
}

// [12] predicate ::= resource
static bool
read_predicate(SerdReader* reader, Node* dest)
{
	return read_resource(reader, dest);
}

// [9] verb ::= predicate | 'a'
static bool
read_verb(SerdReader* reader, Node* dest)
{
	uint8_t pre[2];
	peek_string(reader, pre, 2);
	switch (pre[0]) {
	case 'a':
		switch (pre[1]) {
		case 0x9: case 0xA: case 0xD: case 0x20:
			eat_byte(reader, 'a');
			*dest = push_uri(reader, NS_RDF "type", 47);
			return true;
		default: break;  // fall through
		}
	default:
		return read_predicate(reader, dest);
	}
}

// [26] nodeID ::= '_:' name
static Ref
read_nodeID(SerdReader* reader)
{
	eat_byte(reader, '_');
	eat_byte(reader, ':');
	Ref ref = push_string(reader, "", 0);
	read_name(reader, ref, true);
	SerdString* const str = deref(reader, ref);
	if (reader->syntax == SERD_TURTLE
	    && !strncmp((const char*)str->buf, "genid", 5)) {
		// Replace "genid" nodes with "docid" to prevent clashing
		memcpy(str->buf, "docid", 5);
	}
	return ref;
}

static Ref
blank_id(SerdReader* reader)
{
	Ref str;
	if (reader->bprefix) {
		str = push_string(reader,
		                  (const char*)reader->bprefix,
		                  reader->bprefix_len);
	} else {
		str = push_string(reader, "", 0);
	}
	char num[32];
	snprintf(num, sizeof(num), "%u", reader->next_id++);
	append_string(reader, str, (const uint8_t*)"genid", 5);
	append_string(reader, str, (const uint8_t*)num, strlen(num));
	return str;
}

// Spec: [21] blank ::= nodeID | '[]'
//          | '[' predicateObjectList ']' | collection
// Impl: [21] blank ::= nodeID | '[ ws* ]'
//          | '[' ws* predicateObjectList ws* ']' | collection
static bool
read_blank(SerdReader* reader, ReadContext ctx, bool subject, Node* dest)
{
	const bool was_anon_subject = subject && (*ctx.flags | SERD_ANON_CONT);
	switch (peek_byte(reader)) {
	case '_':
		*dest = make_node(SERD_BLANK, read_nodeID(reader));
		return true;
	case '[':
		eat_byte(reader, '[');
		read_ws_star(reader);
		*dest = make_node(SERD_BLANK, blank_id(reader));
		if (peek_byte(reader) == ']') {
			eat_byte(reader, ']');
			*ctx.flags |= (subject) ? SERD_EMPTY_S : SERD_EMPTY_O;
			if (ctx.subject) {
				TRY_RET(emit_statement(reader, ctx.flags,
				                       ctx.graph, ctx.subject, ctx.predicate,
				                       dest, NULL, 0, 0));
			}
			return true;
		}

		*ctx.flags |= (subject) ? SERD_ANON_S_BEGIN : SERD_ANON_O_BEGIN;
		if (ctx.subject) {
			TRY_RET(emit_statement(reader, ctx.flags,
			                       ctx.graph, ctx.subject, ctx.predicate,
			                       dest, NULL, 0, 0));
		}
		ctx.subject = dest;
		if (!subject) {
			*ctx.flags |= SERD_ANON_CONT;
		}
		read_predicateObjectList(reader, ctx, true);
		read_ws_star(reader);
		eat_byte(reader, ']');
		if (reader->end_sink) {
			const SerdNode end = public_node(reader, dest);
			reader->end_sink(reader->handle, &end);
		}
		if (!was_anon_subject) {
			*ctx.flags &= ~SERD_ANON_CONT;
		}
		return true;
	case '(':
		if (read_collection(reader, ctx, dest)) {
			if (ctx.subject) {
				TRY_RET(emit_statement(reader, ctx.flags,
				                       ctx.graph, ctx.subject, ctx.predicate,
				                       dest, NULL, 0, 0));
			}
			return true;
		}
		return false;
	default:
		return error(reader, "illegal blank node\n");
	}
}

inline static bool
is_object_end(const uint8_t c)
{
	switch (c) {
	case 0x9: case 0xA: case 0xD: case 0x20: case '\0':
	case '#': case '.': case ';':
		return true;
	default:
		return false;
	}
}

// [13] object ::= resource | blank | literal
// Recurses, calling statement_sink for every statement encountered.
// Leaves stack in original calling state (i.e. pops everything it pushes).
static bool
read_object(SerdReader* reader, ReadContext ctx)
{
	static const char* const XSD_BOOLEAN     = NS_XSD "boolean";
	static const size_t      XSD_BOOLEAN_LEN = 40;

#ifndef NDEBUG
	const size_t orig_stack_size = reader->stack.size;
#endif

	uint8_t       pre[6];
	bool          ret      = false;
	bool          emit     = (ctx.subject != 0);
	Node          o        = INTERNAL_NODE_NULL;
	Node          datatype = INTERNAL_NODE_NULL;
	Ref           lang     = 0;
	uint32_t      flags    = 0;
	const uint8_t c        = peek_byte(reader);
	switch (c) {
	case '\0':
	case ')':
		return false;
	case '[': case '(':
		emit = false;
		// fall through
	case '_':
		TRY_THROW(ret = read_blank(reader, ctx, false, &o));
		break;
	case '<': case ':':
		TRY_THROW(ret = read_resource(reader, &o));
		break;
	case '\"': case '+': case '-':
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9': case '.':
		TRY_THROW(ret = read_literal(reader, &o, &datatype, &lang, &flags));
		break;
	default:
		/* Either a boolean literal, or a qname.
		   Unfortunately there is no way to distinguish these without
		   readahead, since `true' or `false' could be the start of a qname.
		*/
		peek_string(reader, pre, 6);
		if (!memcmp(pre, "true", 4) && is_object_end(pre[4])) {
			eat_string(reader, "true", 4);
			const Ref value = push_string(reader, "true", 4);
			datatype = push_uri(reader, XSD_BOOLEAN, XSD_BOOLEAN_LEN);
			o = make_node(SERD_LITERAL, value);
		} else if (!memcmp(pre, "false", 5) && is_object_end(pre[5])) {
			eat_string(reader, "false", 5);
			const Ref value = push_string(reader, "false", 5);
			datatype = push_uri(reader, XSD_BOOLEAN, XSD_BOOLEAN_LEN);
			o = make_node(SERD_LITERAL, value);
		} else if (!is_object_end(c)) {
			o = make_node(SERD_CURIE, read_qname(reader));
		}
		ret = o.value;
	}

	if (ret && emit) {
		assert(o.value);
		ret = emit_statement(reader, ctx.flags,
		                     ctx.graph, ctx.subject, ctx.predicate,
		                     &o, &datatype, lang, flags);
	}

except:
	pop_string(reader, lang);
	pop_string(reader, datatype.value);
	pop_string(reader, o.value);
#ifndef NDEBUG
	assert(reader->stack.size == orig_stack_size);
#endif
	return ret;
}

// Spec: [8] objectList ::= object ( ',' object )*
// Impl: [8] objectList ::= object ( ws* ',' ws* object )*
static bool
read_objectList(SerdReader* reader, ReadContext ctx, bool blank)
{
	TRY_RET(read_object(reader, ctx));
	read_ws_star(reader);
	while (peek_byte(reader) == ',') {
		eat_byte(reader, ',');
		read_ws_star(reader);
		TRY_RET(read_object(reader, ctx));
		read_ws_star(reader);
	}
	return true;
}

// Spec: [7] predicateObjectList ::= verb objectList
//                                   (';' verb objectList)* (';')?
// Impl: [7] predicateObjectList ::= verb ws+ objectList
//                                   (ws* ';' ws* verb ws+ objectList)* (';')?
static bool
read_predicateObjectList(SerdReader* reader, ReadContext ctx, bool blank)
{
	if (reader->eof) {
		return false;
	}
	Node predicate = INTERNAL_NODE_NULL;
	TRY_RET(read_verb(reader, &predicate));
	TRY_THROW(read_ws_plus(reader));
	ctx.predicate = &predicate;
	TRY_THROW(read_objectList(reader, ctx, blank));
	pop_string(reader, predicate.value);
	predicate.value = 0;
	read_ws_star(reader);
	while (peek_byte(reader) == ';') {
		eat_byte(reader, ';');
		read_ws_star(reader);
		switch (peek_byte(reader)) {
		case '.': case ']':
			return true;
		default:
			TRY_THROW(read_verb(reader, &predicate));
			ctx.predicate = &predicate;
			TRY_THROW(read_ws_plus(reader));
			TRY_THROW(read_objectList(reader, ctx, blank));
			pop_string(reader, predicate.value);
			predicate.value = 0;
			read_ws_star(reader);
		}
	}
	pop_string(reader, predicate.value);
	return true;
except:
	pop_string(reader, predicate.value);
	return false;
}

/** Recursive helper for read_collection. */
static bool
read_collection_rec(SerdReader* reader, ReadContext ctx)
{
	read_ws_star(reader);
	if (peek_byte(reader) == ')') {
		eat_byte(reader, ')');
		TRY_RET(emit_statement(reader, ctx.flags,
		                       NULL,
		                       ctx.subject,
		                       &reader->rdf_rest,
		                       &reader->rdf_nil, NULL, 0, 0));
		return false;
	} else {
		const Node rest = make_node(SERD_BLANK, blank_id(reader));
		TRY_RET(emit_statement(reader, ctx.flags,
		                       ctx.graph,
		                       ctx.subject,
		                       &reader->rdf_rest,
		                       &rest, NULL, 0, 0));
		ctx.subject   = &rest;
		ctx.predicate = &reader->rdf_first;
		if (read_object(reader, ctx)) {
			read_collection_rec(reader, ctx);
			pop_string(reader, rest.value);
			return true;
		} else {
			pop_string(reader, rest.value);
			return false;
		}
	}
}

// [22] itemList   ::= object+
// [23] collection ::= '(' itemList? ')'
static bool
read_collection(SerdReader* reader, ReadContext ctx, Node* dest)
{
	TRY_RET(eat_byte(reader, '('));
	read_ws_star(reader);
	if (peek_byte(reader) == ')') {  // Empty collection
		eat_byte(reader, ')');
		*dest = reader->rdf_nil;
		return true;
	}

	*dest = make_node(SERD_BLANK, blank_id(reader));
	ctx.subject   = dest;
	ctx.predicate = &reader->rdf_first;
	if (!read_object(reader, ctx)) {
		return error(reader, "unexpected end of collection\n");
	}

	ctx.subject = dest;
	return read_collection_rec(reader, ctx);
}

// [11] subject ::= resource | blank
static Node
read_subject(SerdReader* reader, ReadContext ctx)
{
	Node subject = INTERNAL_NODE_NULL;
	switch (peek_byte(reader)) {
	case '[': case '(': case '_':
		read_blank(reader, ctx, true, &subject);
		break;
	default:
		read_resource(reader, &subject);
	}
	return subject;
}

// Spec: [6] triples ::= subject predicateObjectList
// Impl: [6] triples ::= subject ws+ predicateObjectList
static bool
read_triples(SerdReader* reader, ReadContext ctx)
{
	const Node subject = read_subject(reader, ctx);
	bool       ret     = false;
	if (subject.value != 0) {
		ctx.subject = &subject;
		TRY_RET(read_ws_plus(reader));
		ret = read_predicateObjectList(reader, ctx, false);
		pop_string(reader, subject.value);
	}
	ctx.subject = ctx.predicate = 0;
	return ret;
}

// [5] base ::= '@base' ws+ uriref
static bool
read_base(SerdReader* reader)
{
	// `@' is already eaten in read_directive
	eat_string(reader, "base", 4);
	TRY_RET(read_ws_plus(reader));
	Ref uri;
	TRY_RET(uri = read_uriref(reader));
	const SerdNode uri_node = public_node_from_ref(reader, SERD_URI, uri);
	if (reader->base_sink) {
		reader->base_sink(reader->handle, &uri_node);
	}
	pop_string(reader, uri);
	return true;
}

// Spec: [4] prefixID ::= '@prefix' ws+ prefixName? ':' uriref
// Impl: [4] prefixID ::= '@prefix' ws+ prefixName? ':' ws* uriref
static bool
read_prefixID(SerdReader* reader)
{
	// `@' is already eaten in read_directive
	eat_string(reader, "prefix", 6);
	TRY_RET(read_ws_plus(reader));
	bool ret = true;
	Ref name = read_prefixName(reader);
	if (!name) {
		name = push_string(reader, "", 0);
	}
	TRY_THROW(eat_byte(reader, ':') == ':');
	read_ws_star(reader);
	Ref uri = 0;
	TRY_THROW(uri = read_uriref(reader));
	const SerdNode name_node = public_node_from_ref(reader, SERD_LITERAL, name);
	const SerdNode uri_node  = public_node_from_ref(reader, SERD_URI, uri);
	if (reader->prefix_sink) {
		ret = !reader->prefix_sink(reader->handle, &name_node, &uri_node);
	}
	pop_string(reader, uri);
except:
	pop_string(reader, name);
	return ret;
}

// [3] directive ::= prefixID | base
static bool
read_directive(SerdReader* reader)
{
	eat_byte(reader, '@');
	switch (peek_byte(reader)) {
	case 'b':
		return read_base(reader);
	case 'p':
		return read_prefixID(reader);
	default:
		return error(reader, "illegal directive\n");
	}
}

// Spec: [1] statement ::= directive '.' | triples '.' | ws+
// Impl: [1] statement ::= directive ws* '.' | triples ws* '.' | ws+
static bool
read_statement(SerdReader* reader)
{
	SerdStatementFlags flags = 0;
	ReadContext ctx = { 0, 0, 0, &flags };
	read_ws_star(reader);
	if (reader->eof) {
		return true;
	}
	switch (peek_byte(reader)) {
	case '@':
		TRY_RET(read_directive(reader));
		break;
	default:
		TRY_RET(read_triples(reader, ctx));
		break;
	}
	read_ws_star(reader);
	return eat_byte(reader, '.');
}

// [1] turtleDoc ::= statement
static bool
read_turtleDoc(SerdReader* reader)
{
	while (!reader->eof) {
		TRY_RET(read_statement(reader));
	}
	return true;
}

SERD_API
SerdReader*
serd_reader_new(SerdSyntax        syntax,
                void*             handle,
                void              (*free_handle)(void*),
                SerdBaseSink      base_sink,
                SerdPrefixSink    prefix_sink,
                SerdStatementSink statement_sink,
                SerdEndSink       end_sink)
{
	const Cursor cur = { NULL, 0, 0 };
	SerdReader*  me  = malloc(sizeof(struct SerdReaderImpl));
	me->handle           = handle;
	me->free_handle      = free_handle;
	me->base_sink        = base_sink;
	me->prefix_sink      = prefix_sink;
	me->statement_sink   = statement_sink;
	me->end_sink         = end_sink;
	me->fd               = 0;
	me->stack            = serd_stack_new(STACK_PAGE_SIZE);
	me->syntax           = syntax;
	me->cur              = cur;
	me->bprefix          = NULL;
	me->bprefix_len      = 0;
	me->next_id          = 1;
	me->read_buf         = 0;
	me->read_head        = 0;
	me->eof              = false;
#ifdef SERD_STACK_CHECK
	me->alloc_stack      = 0;
	me->n_allocs         = 0;
#endif

#define RDF_FIRST NS_RDF "first"
#define RDF_REST  NS_RDF "rest"
#define RDF_NIL   NS_RDF "nil"
	me->rdf_first = make_node(SERD_URI, push_string(me, RDF_FIRST, 48));
	me->rdf_rest  = make_node(SERD_URI, push_string(me, RDF_REST, 47));
	me->rdf_nil   = make_node(SERD_URI, push_string(me, RDF_NIL, 46));

	return me;
}

SERD_API
void
serd_reader_free(SerdReader* reader)
{
	pop_string(reader, reader->rdf_nil.value);
	pop_string(reader, reader->rdf_rest.value);
	pop_string(reader, reader->rdf_first.value);

#ifdef SERD_STACK_CHECK
	free(reader->alloc_stack);
#endif
	free(reader->stack.buf);
	free(reader->bprefix);
	if (reader->free_handle) {
		reader->free_handle(reader->handle);
	}
	free(reader);
}

SERD_API
void*
serd_reader_get_handle(const SerdReader* reader)
{
	return reader->handle;
}

SERD_API
void
serd_reader_add_blank_prefix(SerdReader*    reader,
                             const uint8_t* prefix)
{
	if (reader->bprefix) {
		free(reader->bprefix);
		reader->bprefix_len = 0;
		reader->bprefix     = NULL;
	}
	if (prefix) {
		reader->bprefix_len = strlen((const char*)prefix);
		reader->bprefix     = malloc(reader->bprefix_len + 1);
		memcpy(reader->bprefix, prefix, reader->bprefix_len + 1);
	}
}

SERD_API
SerdStatus
serd_reader_read_file(SerdReader*    reader,
                      const uint8_t* uri)
{
	const uint8_t* path = serd_uri_to_path(uri);
	if (!path) {
		return SERD_ERR_BAD_ARG;
	}

	FILE* fd = fopen((const char*)path, "r");
	if (!fd) {
		fprintf(stderr, "Error opening file %s (%s)\n", path, strerror(errno));
		return SERD_ERR_UNKNOWN;
	}
	SerdStatus ret = serd_reader_read_file_handle(reader, fd, path);
	fclose(fd);
	return ret;
}

SERD_API
SerdStatus
serd_reader_read_file_handle(SerdReader* me, FILE* file, const uint8_t* name)
{
	const Cursor cur = { name, 1, 1 };
	me->fd        = file;
	me->read_buf  = (uint8_t*)malloc(READ_BUF_LEN * 2);
	me->read_head = 0;
	me->cur       = cur;
	me->from_file = true;
	me->eof       = false;

	/* Read into the second page of the buffer. Occasionally peek_string
	   will move the read_head to before this point when readahead causes
	   a page fault.
	*/
	memset(me->read_buf, '\0', READ_BUF_LEN * 2);
	me->read_buf += READ_BUF_LEN;

	const bool ret = !page(me) || read_turtleDoc(me);

	free(me->read_buf - READ_BUF_LEN);
	me->fd       = 0;
	me->read_buf = NULL;
	return ret ? SERD_SUCCESS : SERD_ERR_UNKNOWN;
}

SERD_API
SerdStatus
serd_reader_read_string(SerdReader* me, const uint8_t* utf8)
{
	const Cursor cur = { (const uint8_t*)"(string)", 1, 1 };

	me->read_buf  = (uint8_t*)utf8;
	me->read_head = 0;
	me->cur       = cur;
	me->from_file = false;
	me->eof       = false;

	const bool ret = read_turtleDoc(me);

	me->read_buf = NULL;
	return ret ? SERD_SUCCESS : SERD_ERR_UNKNOWN;
}
