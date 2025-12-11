/* Copyright (c) 2012, Bastien Dejean
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdlib.h>
#include "bspwm.h"
#include "window.h"
#include "subscribe.h"
#include "ewmh.h"
#include "tree.h"
#include "stack.h"

#define MAX_STACK_DEPTH 1000

stacking_list_t *make_stack(node_t *n)
{
	if (!n)
		return NULL;
		
	stacking_list_t *s = calloc(1, sizeof(stacking_list_t));
	if (!s)
		return NULL;
		
	s->node = n;
	s->prev = s->next = NULL;
	return s;
}

void stack_insert_after(stacking_list_t *a, node_t *n)
{
	if (!n)
		return;
		
	stacking_list_t *s = make_stack(n);
	if (!s)
		return;
		
	if (!a) {
		stack_head = stack_tail = s;
	} else {
		if (a->node == n) {
			free(s);
			return;
		}
		remove_stack_node(n);
		stacking_list_t *b = a->next;
		if (b)
			b->prev = s;
		s->next = b;
		s->prev = a;
		a->next = s;
		if (stack_tail == a)
			stack_tail = s;
	}
}

void stack_insert_before(stacking_list_t *a, node_t *n)
{
	if (!n)
		return;
		
	stacking_list_t *s = make_stack(n);
	if (!s)
		return;
		
	if (!a) {
		stack_head = stack_tail = s;
	} else {
		if (a->node == n) {
			free(s);
			return;
		}
		remove_stack_node(n);
		stacking_list_t *b = a->prev;
		if (b)
			b->next = s;
		s->prev = b;
		s->next = a;
		a->prev = s;
		if (stack_head == a)
			stack_head = s;
	}
}

void remove_stack(stacking_list_t *s)
{
	if (!s)
		return;
		
	stacking_list_t *a = s->prev;
	stacking_list_t *b = s->next;
	
	if (a)
		a->next = b;
	if (b)
		b->prev = a;
	if (s == stack_head)
		stack_head = b;
	if (s == stack_tail)
		stack_tail = a;
		
	free(s);
}

void remove_stack_node(node_t *n)
{
	if (!n)
		return;
		
	for (node_t *f = first_extrema(n); f; f = next_leaf(f, n)) {
		stacking_list_t *s = stack_head;
		stacking_list_t *next;
		while (s) {
			next = s->next;
			if (s->node == f) {
				remove_stack(s);
				break;
			}
			s = next;
		}
	}
}

int stack_level(client_t *c)
{
	if (!c)
		return 0;

	/* Branchless lookup: layer enum is 0,1,2 which matches level directly.
	 * State needs lookup: TILED=0, PSEUDO_TILED=0, FLOATING=1, FULLSCREEN=2 */
	static const int8_t state_table[4] = {0, 0, 1, 2};
	return 3 * c->layer + state_table[c->state];
}

int stack_cmp(client_t *c1, client_t *c2)
{
	if (!c1 && !c2)
		return 0;
	if (!c1)
		return -1;
	if (!c2)
		return 1;
		
	return stack_level(c1) - stack_level(c2);
}

stacking_list_t *limit_above(node_t *n)
{
	if (!n || !n->client)
		return NULL;
		
	stacking_list_t *s = stack_head;
	while (s && s->node && s->node->client && 
	       stack_cmp(n->client, s->node->client) >= 0) {
		s = s->next;
	}
	
	if (!s)
		s = stack_tail;
		
	if (s && s->node == n)
		s = s->prev;
		
	return s;
}

stacking_list_t *limit_below(node_t *n)
{
	if (!n || !n->client)
		return NULL;
		
	stacking_list_t *s = stack_tail;
	while (s && s->node && s->node->client && 
	       stack_cmp(n->client, s->node->client) <= 0) {
		s = s->prev;
	}
	
	if (!s)
		s = stack_head;
		
	if (s && s->node == n)
		s = s->next;
		
	return s;
}

void stack(desktop_t *d, node_t *n, bool focused)
{
	if (!d || !n)
		return;
		
	for (node_t *f = first_extrema(n); f; f = next_leaf(f, n)) {
		if (!f->client || (IS_FLOATING(f->client) && !auto_raise))
			continue;
			
		if (!stack_head) {
			stack_insert_after(NULL, f);
		} else {
			stacking_list_t *s = focused ? limit_above(f) : limit_below(f);
			if (!s)
				continue;
				
			if (!s->node || !s->node->client)
				continue;
				
			int i = stack_cmp(f->client, s->node->client);
			if (i < 0 || (i == 0 && !focused)) {
				stack_insert_before(s, f);
				window_below(f->id, s->node->id);
				put_status(SBSC_MASK_NODE_STACK, "node_stack 0x%08X below 0x%08X\n", 
				          f->id, s->node->id);
			} else {
				stack_insert_after(s, f);
				window_above(f->id, s->node->id);
				put_status(SBSC_MASK_NODE_STACK, "node_stack 0x%08X above 0x%08X\n", 
				          f->id, s->node->id);
			}
		}
	}
	
	ewmh_update_client_list(true);
	restack_presel_feedbacks(d);
}

static void restack_presel_feedbacks_in_depth(node_t *r, node_t *n, int depth);

void restack_presel_feedbacks(desktop_t *d)
{
	if (!d)
		return;
		
	stacking_list_t *s = stack_tail;
	while (s && s->node && s->node->client && !IS_TILED(s->node->client)) {
		s = s->prev;
	}
	
	if (s && s->node)
		restack_presel_feedbacks_in_depth(d->root, s->node, 0);
}

static void restack_presel_feedbacks_in_depth(node_t *r, node_t *n, int depth)
{
	if (!r || !n || depth > MAX_STACK_DEPTH)
		return;
		
	if (r->presel)
		window_above(r->presel->feedback, n->id);
		
	restack_presel_feedbacks_in_depth(r->first_child, n, depth + 1);
	restack_presel_feedbacks_in_depth(r->second_child, n, depth + 1);
}

void restack_presel_feedbacks_in(node_t *r, node_t *n)
{
	restack_presel_feedbacks_in_depth(r, n, 0);
}
