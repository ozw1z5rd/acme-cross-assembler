// ACME - a crossassembler for producing 6502/65c02/65816 code.
// Copyright (C) 1998-2014 Marco Baye
// Have a look at "acme.c" for further info
//
// Flow control stuff (loops, conditional assembly etc.)
//
// Macros, conditional assembly, loops and sourcefile-includes are all based on
// parsing blocks of code. When defining macros or using loops or conditional
// assembly, the block starts with "{" and ends with "}". In the case of
// "!source", the given file is treated like a block - the outermost assembler
// function uses the same technique to parse the top level file.
//
// 24 Nov 2007	Added "!ifndef"
#include <string.h>
#include "acme.h"
#include "alu.h"
#include "config.h"
#include "dynabuf.h"
#include "global.h"
#include "input.h"
#include "macro.h"
#include "mnemo.h"
#include "symbol.h"
#include "tree.h"


// type definitions

struct loop_condition {
	int	line;	// original line number
	int	invert;	// actually bool (0 for WHILE, 1 for UNTIL)
	char	*body;	// pointer to actual expression
};


// variables

// predefined stuff
static struct node_t	*condkey_tree	= NULL;	// tree to hold UNTIL and WHILE
static struct node_t	condkeys[]	= {
	PREDEFNODE("until",	TRUE),	// UNTIL inverts the condition
	PREDEFLAST("while",	FALSE),	// WHILE does not change the condition
	//    ^^^^ this marks the last element
};


// helper functions for "!for" and "!do"

// parse a loop body (could also be used for macro body)
static void parse_ram_block(int line_number, char *body)
{
	Input_now->line_number = line_number;	// set line number to loop start
	Input_now->src.ram_ptr = body;	// set RAM read pointer to loop
	// Parse loop body
	Parse_until_eob_or_eof();
	if (GotByte != CHAR_EOB)
		Bug_found("IllegalBlockTerminator", GotByte);
}


// try to read a condition into DynaBuf and store copy pointer in
// given loop_condition structure.
// if no condition given, NULL is written to structure.
// call with GotByte = first interesting character
static void store_condition(struct loop_condition *condition, char terminator)
{
	void	*node_body;

	// write line number
	condition->line = Input_now->line_number;
	// Check for empty condition
	if (GotByte == terminator) {
		// Write NULL condition, then return
		condition->invert = FALSE;
		condition->body = NULL;
		return;
	}
	// Seems as if there really *is* a condition.
	// Read UNTIL/WHILE keyword
	if (Input_read_and_lower_keyword()) {
		// Search for new tree item
		if (!Tree_easy_scan(condkey_tree, &node_body, GlobalDynaBuf)) {
			Throw_error(exception_syntax);
			condition->invert = FALSE;
			condition->body = NULL;
			return;
		}
		condition->invert = (int) node_body;
		// Write given condition into buffer
		SKIPSPACE();
		DYNABUF_CLEAR(GlobalDynaBuf);
		Input_until_terminator(terminator);
		DynaBuf_append(GlobalDynaBuf, CHAR_EOS);	// ensure terminator
		condition->body = DynaBuf_get_copy(GlobalDynaBuf);
	}
}


// check a condition expression
static int check_condition(struct loop_condition *condition)
{
	intval_t	expression;

	// First, check whether there actually *is* a condition
	if (condition->body == NULL)
		return TRUE;	// non-existing conditions are always true

	// set up input for expression evaluation
	Input_now->line_number = condition->line;
	Input_now->src.ram_ptr = condition->body;
	GetByte();	// proceed with next char
	expression = ALU_defined_int();
	if (GotByte)
		Throw_serious_error(exception_syntax);
	return condition->invert ? !expression : !!expression;
}


// looping assembly ("!do"). Has to be re-entrant.
static enum eos PO_do(void)	// Now GotByte = illegal char
{
	struct loop_condition	condition1,
				condition2;
	struct input	loop_input,
			*outer_input;
	char		*loop_body;
	int		go_on,
			loop_start;	// line number of loop pseudo opcode
	// Init
	condition1.invert = FALSE;
	condition1.body = NULL;
	condition2.invert = FALSE;
	condition2.body = NULL;
	
	// Read head condition to buffer
	SKIPSPACE();
	store_condition(&condition1, CHAR_SOB);
	if (GotByte != CHAR_SOB)
		Throw_serious_error(exception_no_left_brace);
	// Remember line number of loop body,
	// then read block and get copy
	loop_start = Input_now->line_number;
	loop_body = Input_skip_or_store_block(TRUE);	// changes line number!
	// now GotByte = '}'
	NEXTANDSKIPSPACE();	// Now GotByte = first non-blank char after block
	// Read tail condition to buffer
	store_condition(&condition2, CHAR_EOS);
	// now GotByte = CHAR_EOS
	// set up new input
	loop_input = *Input_now;	// copy current input structure into new
	loop_input.source_is_ram = TRUE;	// set new byte source
	// remember old input
	outer_input = Input_now;
	// activate new input (not useable yet, as pointer and
	// line number are not yet set up)
	Input_now = &loop_input;
	do {
		// Check head condition
		go_on = check_condition(&condition1);
		if (go_on) {
			parse_ram_block(loop_start, loop_body);
			// Check tail condition
			go_on = check_condition(&condition2);
		}
	} while (go_on);
	// Free memory
	free(condition1.body);
	free(loop_body);
	free(condition2.body);
	// restore previous input:
	Input_now = outer_input;
	GotByte = CHAR_EOS;	// CAUTION! Very ugly kluge.
	// But by switching input, we lost the outer input's GotByte. We know
	// it was CHAR_EOS. We could just call GetByte() to get real input, but
	// then the main loop could choke on unexpected bytes. So we pretend
	// that we got the outer input's GotByte value magically back.
	return AT_EOS_ANYWAY;
}


// looping assembly ("!for"). Has to be re-entrant.
// old syntax: !for VAR, END { BLOCK }		VAR counts from 1 to END
// new syntax: !for VAR, START, END { BLOCK }	VAR counts from START to END
static enum eos PO_for(void)	// Now GotByte = illegal char
{
	struct input	loop_input,
			*outer_input;
	struct result	loop_counter;
	intval_t	first_arg,
			counter_first,
			counter_last,
			counter_increment;
	int		old_algo;	// actually bool
	char		*loop_body;	// pointer to loop's body block
	struct symbol	*symbol;
	zone_t		zone;
	int		force_bit,
			loop_start;	// line number of "!for" pseudo opcode

	if (Input_read_zone_and_keyword(&zone) == 0)	// skips spaces before
		return SKIP_REMAINDER;
	// Now GotByte = illegal char
	force_bit = Input_get_force_bit();	// skips spaces after
	symbol = symbol_find(zone, force_bit);
	if (!Input_accept_comma()) {
		Throw_error(exception_syntax);
		return SKIP_REMAINDER;
	}
	first_arg = ALU_defined_int();
	if (Input_accept_comma()) {
		old_algo = FALSE;	// new format - yay!
		if (!warn_on_old_for)
			Throw_first_pass_warning("Found new \"!for\" syntax.");
		counter_first = first_arg;	// use given argument
		counter_last = ALU_defined_int();	// read second argument
		counter_increment = (counter_last < counter_first) ? -1 : 1;
	} else {
		old_algo = TRUE;	// old format - booo!
		if (warn_on_old_for)
			Throw_first_pass_warning("Found old \"!for\" syntax.");
		if (first_arg < 0)
			Throw_serious_error("Loop count is negative.");
		counter_first = 0;	// CAUTION - old algo pre-increments and therefore starts with 1!
		counter_last = first_arg;	// use given argument
		counter_increment = 1;
	}
	if (GotByte != CHAR_SOB)
		Throw_serious_error(exception_no_left_brace);
	// remember line number of loop pseudo opcode
	loop_start = Input_now->line_number;
	// read loop body into DynaBuf and get copy
	loop_body = Input_skip_or_store_block(TRUE);	// changes line number!
	// switching input makes us lose GotByte. But we know it's '}' anyway!
	// set up new input
	loop_input = *Input_now;	// copy current input structure into new
	loop_input.source_is_ram = TRUE;	// set new byte source
	// remember old input
	outer_input = Input_now;
	// activate new input
	// (not yet useable; pointer and line number are still missing)
	Input_now = &loop_input;
	// init counter
	loop_counter.flags = MVALUE_DEFINED | MVALUE_EXISTS;
	loop_counter.val.intval = counter_first;
	symbol_set_value(symbol, &loop_counter, TRUE);
	if (old_algo) {
		// old algo for old syntax:
		// if count == 0, skip loop
		if (counter_last) {
			do {
				loop_counter.val.intval += counter_increment;
				symbol_set_value(symbol, &loop_counter, TRUE);
				parse_ram_block(loop_start, loop_body);
			} while (loop_counter.val.intval < counter_last);
		}
	} else {
		// new algo for new syntax:
		do {
			parse_ram_block(loop_start, loop_body);
			loop_counter.val.intval += counter_increment;
			symbol_set_value(symbol, &loop_counter, TRUE);
		} while (loop_counter.val.intval != (counter_last + counter_increment));
	}
	// Free memory
	free(loop_body);
	// restore previous input:
	Input_now = outer_input;
	// GotByte of OuterInput would be '}' (if it would still exist)
	GetByte();	// fetch next byte
	return ENSURE_EOS;
}


// helper functions for "!if", "!ifdef" and "!ifndef"

// parse or skip a block. Returns whether block's '}' terminator was missing.
// afterwards: GotByte = '}'
static int skip_or_parse_block(int parse)
{
	if (!parse) {
		Input_skip_or_store_block(FALSE);
		return 0;
	}
	// if block was correctly terminated, return FALSE
	Parse_until_eob_or_eof();
	// if block isn't correctly terminated, complain and exit
	if (GotByte != CHAR_EOB)
		Throw_serious_error(exception_no_right_brace);
	return 0;
}


// parse {block} [else {block}]
static void parse_block_else_block(int parse_first)
{
	// Parse first block.
	// If it's not correctly terminated, return immediately (because
	// in that case, there's no use in checking for an "else" part).
	if (skip_or_parse_block(parse_first))
		return;
	// now GotByte = '}'. Check for "else" part.
	// If end of statement, return immediately.
	NEXTANDSKIPSPACE();
	if (GotByte == CHAR_EOS)
		return;
	// read keyword and check whether really "else"
	if (Input_read_and_lower_keyword()) {
		if (strcmp(GlobalDynaBuf->buffer, "else")) {
			Throw_error(exception_syntax);
		} else {
			SKIPSPACE();
			if (GotByte != CHAR_SOB)
				Throw_serious_error(exception_no_left_brace);
			skip_or_parse_block(!parse_first);
			// now GotByte = '}'
			GetByte();
		}
	}
	Input_ensure_EOS();
}


// conditional assembly ("!if"). Has to be re-entrant.
static enum eos PO_if(void)	// Now GotByte = illegal char
{
	intval_t	cond;

	cond = ALU_defined_int();
	if (GotByte != CHAR_SOB)
		Throw_serious_error(exception_no_left_brace);
	parse_block_else_block(!!cond);
	return ENSURE_EOS;
}


// conditional assembly ("!ifdef" and "!ifndef"). Has to be re-entrant.
static enum eos ifdef_ifndef(int invert)	// Now GotByte = illegal char
{
	struct node_ra_t	*node;
	struct symbol		*symbol;
	zone_t			zone;
	int			defined	= FALSE;

	if (Input_read_zone_and_keyword(&zone) == 0)	// skips spaces before
		return SKIP_REMAINDER;

	Tree_hard_scan(&node, symbols_forest, zone, FALSE);
	if (node) {
		symbol = (struct symbol *) node->body;
		// in first pass, count usage
		if (pass_count == 0)
			symbol->usage++;
		if (symbol->result.flags & MVALUE_DEFINED)
			defined = TRUE;
	}
	SKIPSPACE();
	// if "ifndef", invert condition
	if (invert)
		defined = !defined;
	if (GotByte != CHAR_SOB)
		return defined ? PARSE_REMAINDER : SKIP_REMAINDER;

	parse_block_else_block(defined);
	return ENSURE_EOS;
}


// conditional assembly ("!ifdef"). Has to be re-entrant.
static enum eos PO_ifdef(void)	// Now GotByte = illegal char
{
	return ifdef_ifndef(FALSE);
}


// conditional assembly ("!ifndef"). Has to be re-entrant.
static enum eos PO_ifndef(void)	// Now GotByte = illegal char
{
	return ifdef_ifndef(TRUE);
}


// macro definition ("!macro").
static enum eos PO_macro(void)	// Now GotByte = illegal char
{
	// In first pass, parse. In all other passes, skip.
	if (pass_count == 0) {
		Macro_parse_definition();	// now GotByte = '}'
	} else {
		// skip until CHAR_SOB ('{') is found.
		// no need to check for end-of-statement, because such an
		// error would already have been detected in first pass.
		// for the same reason, there is no need to check for quotes.
		while (GotByte != CHAR_SOB)
			GetByte();
		Input_skip_or_store_block(FALSE);	// now GotByte = '}'
	}
	GetByte();	// Proceed with next character
	return ENSURE_EOS;
}


// parse a whole source code file
void Parse_and_close_file(FILE *fd, const char *filename)
{
	// be verbose
	if (Process_verbosity > 2)
		printf("Parsing source file '%s'\n", filename);
	// set up new input
	Input_new_file(filename, fd);
	// Parse block and check end reason
	Parse_until_eob_or_eof();
	if (GotByte != CHAR_EOF)
		Throw_error("Found '}' instead of end-of-file.");
	// close sublevel src
	fclose(Input_now->src.fd);
}


// include source file ("!source" or "!src"). Has to be re-entrant.
static enum eos PO_source(void)	// Now GotByte = illegal char
{
	FILE		*fd;
	char		local_gotbyte;
	struct input	new_input,
			*outer_input;

	// Enter new nesting level.
	// Quit program if recursion too deep.
	if (--source_recursions_left < 0)
		Throw_serious_error("Too deeply nested. Recursive \"!source\"?");
	// Read file name. Quit function on error.
	if (Input_read_filename(TRUE))
		return SKIP_REMAINDER;

	// If file could be opened, parse it. Otherwise, complain.
	if ((fd = fopen(GLOBALDYNABUF_CURRENT, FILE_READBINARY))) {
		char	filename[GlobalDynaBuf->size];

		strcpy(filename, GLOBALDYNABUF_CURRENT);
		outer_input = Input_now;	// remember old input
		local_gotbyte = GotByte;	// CAUTION - ugly kluge
		Input_now = &new_input;	// activate new input
		Parse_and_close_file(fd, filename);
		Input_now = outer_input;	// restore previous input
		GotByte = local_gotbyte;	// CAUTION - ugly kluge
	} else {
		Throw_error(exception_cannot_open_input_file);
	}
	// Leave nesting level
	source_recursions_left++;
	return ENSURE_EOS;
}


// pseudo opcode table
static struct node_t	pseudo_opcodes[]	= {
	PREDEFNODE("do",	PO_do),
	PREDEFNODE("for",	PO_for),
	PREDEFNODE("if",	PO_if),
	PREDEFNODE("ifdef",	PO_ifdef),
	PREDEFNODE("ifndef",	PO_ifndef),
	PREDEFNODE("macro",	PO_macro),
	PREDEFNODE("source",	PO_source),
	PREDEFLAST("src",	PO_source),
	//    ^^^^ this marks the last element
};


// register pseudo opcodes and build keyword tree for until/while
void Flow_init(void)
{
	Tree_add_table(&condkey_tree, condkeys);
	Tree_add_table(&pseudo_opcode_tree, pseudo_opcodes);
}