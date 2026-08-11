//  MIT License - Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.

#include "string_handler.h"
#include "memory_pool.h"
#include <string.h>

/* Alocacao: SEMPRE via memory_pool (memop_alloc_raw/memop_free_raw/
 * memop_realloc_raw), nunca malloc/realloc do CRT -- decisao do projeto.
 * O struct StringX e o buffer Content saem ambos do pool. Consumidores de
 * StringX de vida longa devem estar cientes de que o pool e resetavel
 * (memop_test_reset/memop_shutdown): nao segure um StringX vivo atraves de um
 * reset do pool. */


/* ------------------------------------------------------------------ */
/* Helpers internos                                                     */
/* ------------------------------------------------------------------ */

static boolean sh_is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}


/* Garante que o buffer comporta 'required' caracteres uteis (+1 do NUL).
 * Cresce dobrando. Devolve false (e dispara evento) em falha de alocacao,
 * mantendo o buffer antigo intacto. */
static boolean sh_ensure_capacity(StringX* s, uint64 required)
{
	if (required <= s->Max)
	{
		return true;
	}

	uint64 new_max = s->Max ? s->Max : (uint64)INITIAL_STRING_LENGTH;
	while (new_max < required)
	{
		new_max *= 2;
	}

	char* nc = (char*)memop_realloc_raw(s->Content, (new_max + 1) * sizeof(char));
	if (!nc)
	{
		xpb_event_trigger_error(0, "Falha ao realocar %zu caracteres para a string.", (size_t)(new_max + 1));
		return false;
	}

	s->Content = nc;
	s->Max     = new_max;
	return true;
}


/* ------------------------------------------------------------------ */
/* Basicas: init / create / release                                     */
/* ------------------------------------------------------------------ */

void string_init(StringX* ni)
{
	if (!ni)
	{
		return;
	}

	ni->Active  = true;
	ni->Length  = 0;
	ni->Max     = (uint64)INITIAL_STRING_LENGTH;

	ni->Content = (char*)memop_alloc_raw((ni->Max + 1) * sizeof(char));
	if (!ni->Content)
	{
		xpb_event_trigger_error(0, "Falha ao alocar %zu caracteres para a string.", (size_t)(ni->Max + 1));
		ni->Active = false;
		ni->Max    = 0;
		return;
	}
	ni->Content[0] = 0;
}


StringX string_create()
{
	StringX str;
	string_init(&str);
	return str;
}


void string_release(StringX* str)
{
	if (!str)
	{
		return;
	}

	if (str->Content)
	{
		memop_free_raw(str->Content);
		str->Content = NULL;
	}
	str->Length = 0;
	str->Max    = 0;
	str->Active = false;
}


/* ------------------------------------------------------------------ */
/* Copia / append                                                       */
/* ------------------------------------------------------------------ */

/* Escreve 'length' chars de content[src_start..] em src->Content[dst_start..].
 * Estende Length se a escrita passar do fim atual. */
void string_copy(StringX* src, const char* content, const int cont_length, const int src_start, const int length, const int dst_start)
{
	if (!src || !src->Content || !content)
	{
		return;
	}
	if (length <= 0 || src_start < 0 || dst_start < 0)
	{
		return;
	}
	if (src_start + length > cont_length)
	{
		return; /* leitura fora da origem */
	}

	uint64 end = (uint64)dst_start + (uint64)length;
	if (!sh_ensure_capacity(src, end))
	{
		return;
	}

	for (int i = 0; i < length; i++)
	{
		src->Content[dst_start + i] = content[src_start + i];
	}

	if (end > src->Length)
	{
		src->Length = end;
	}
	src->Content[src->Length] = 0;
}


void string_copys(StringX* src, StringX* dst, const int src_start, const int length, const int dst_start)
{
	if (!src || !src->Content)
	{
		return;
	}
	string_copy(dst, src->Content, (int)src->Length, src_start, length, dst_start);
}


/* Concatena 'length' chars de src->Content[src_start..] ao fim de dst. */
void string_append(StringX* src, StringX* dst, const int src_start, const int length)
{
	if (!src || !src->Content || !dst)
	{
		return;
	}
	if (length <= 0 || src_start < 0)
	{
		return;
	}
	if ((uint64)(src_start + length) > src->Length)
	{
		return;
	}
	string_copy(dst, src->Content, (int)src->Length, src_start, length, (int)dst->Length);
}


/* Concatena 'src_length' chars de content[src_start..] ao fim de src (StringX). */
void string_appends(StringX* src, const char* content, const int con_length, const int src_start, const int src_length)
{
	if (!src)
	{
		return;
	}
	string_copy(src, content, con_length, src_start, src_length, (int)src->Length);
}


/* ------------------------------------------------------------------ */
/* Igualdade (total e parcial)                                          */
/* ------------------------------------------------------------------ */

boolean string_equal(StringX* s1, const char* s2, const int s2_length)
{
	if (!s1 || !s1->Content || !s2 || s2_length < 0)
	{
		return false;
	}
	if (s1->Length != (uint64)s2_length)
	{
		return false;
	}
	for (uint64 i = 0; i < s1->Length; i++)
	{
		if (s1->Content[i] != s2[i])
		{
			return false;
		}
	}
	return true;
}


boolean string_equals(StringX* s1, StringX* s2)
{
	if (!s2)
	{
		return false;
	}
	return string_equal(s1, s2->Content, (int)s2->Length);
}


boolean string_equal_part(StringX* s1, const int s1_start, const char* s2, const int s2_length)
{
	if (!s1 || !s1->Content || !s2 || s1_start < 0 || s2_length < 0)
	{
		return false;
	}
	if ((uint64)(s1_start + s2_length) > s1->Length)
	{
		return false;
	}
	for (int i = 0; i < s2_length; i++)
	{
		if (s1->Content[s1_start + i] != s2[i])
		{
			return false;
		}
	}
	return true;
}


boolean string_equals_part(StringX* s1, const int s1_start, StringX* s2)
{
	if (!s2)
	{
		return false;
	}
	return string_equal_part(s1, s1_start, s2->Content, (int)s2->Length);
}


/* ------------------------------------------------------------------ */
/* Busca de posicao                                                     */
/* ------------------------------------------------------------------ */

int string_indexof(StringX* str, const char token, const int pos_start, const int pos_length)
{
	if (!str || !str->Content || pos_start < 0 || pos_length < 0)
	{
		return -1;
	}
	int end = pos_start + pos_length;
	if ((uint64)end > str->Length)
	{
		end = (int)str->Length;
	}
	for (int i = pos_start; i < end; i++)
	{
		if (str->Content[i] == token)
		{
			return i;
		}
	}
	return -1;
}


int string_indexofs(StringX* str, const char* tokens, const int length, const int pos_start, const int pos_length)
{
	if (!str || !str->Content || !tokens || length <= 0 || pos_start < 0 || pos_length < 0)
	{
		return -1;
	}
	int end = pos_start + pos_length;
	if ((uint64)end > str->Length)
	{
		end = (int)str->Length;
	}
	for (int i = pos_start; i + length <= end; i++)
	{
		if (memcmp(str->Content + i, tokens, (size_t)length) == 0)
		{
			return i;
		}
	}
	return -1;
}


int string_index_end(StringX* str, const char token)
{
	if (!str || !str->Content)
	{
		return -1;
	}
	for (int i = (int)str->Length - 1; i >= 0; i--)
	{
		if (str->Content[i] == token)
		{
			return i;
		}
	}
	return -1;
}


int string_index_ends(StringX* str, const char* tokens, const int length)
{
	if (!str || !str->Content || !tokens || length <= 0)
	{
		return -1;
	}
	for (int i = (int)str->Length - length; i >= 0; i--)
	{
		if (memcmp(str->Content + i, tokens, (size_t)length) == 0)
		{
			return i;
		}
	}
	return -1;
}


/* ------------------------------------------------------------------ */
/* Substring                                                            */
/* ------------------------------------------------------------------ */

StringX* string_substring(StringX* str, const int index, const int length)
{
	if (!str || !str->Content || index < 0 || length < 0)
	{
		return NULL;
	}
	if ((uint64)(index + length) > str->Length)
	{
		return NULL;
	}

	StringX* out = (StringX*)memop_alloc_raw(sizeof(StringX));
	if (!out)
	{
		xpb_event_trigger_error(0, "Falha ao alocar substring.");
		return NULL;
	}

	out->Content = (char*)memop_alloc_raw(((uint64)length + 1) * sizeof(char));
	if (!out->Content)
	{
		xpb_event_trigger_error(0, "Falha ao alocar %zu caracteres para a substring.", (size_t)(length + 1));
		memop_free_raw(out);
		return NULL;
	}

	for (int i = 0; i < length; i++)
	{
		out->Content[i] = str->Content[index + i];
	}
	out->Content[length] = 0;
	out->Length = (uint64)length;
	out->Max    = (uint64)length;
	out->Active = true;
	return out;
}


/* ------------------------------------------------------------------ */
/* Trim                                                                 */
/* ------------------------------------------------------------------ */

void string_trim_right(StringX* str)
{
	if (!str || !str->Content)
	{
		return;
	}
	while (str->Length > 0 && sh_is_ws(str->Content[str->Length - 1]))
	{
		str->Length--;
	}
	str->Content[str->Length] = 0;
}


void string_trim_left(StringX* str)
{
	if (!str || !str->Content)
	{
		return;
	}
	uint64 start = 0;
	while (start < str->Length && sh_is_ws(str->Content[start]))
	{
		start++;
	}
	if (start > 0)
	{
		uint64 n = str->Length - start;
		memmove(str->Content, str->Content + start, (size_t)n);
		str->Length = n;
		str->Content[n] = 0;
	}
}


void string_trim(StringX* str)
{
	string_trim_right(str);
	string_trim_left(str);
}


/* ------------------------------------------------------------------ */
/* Paragem por token (busca de substring)                               */
/* ------------------------------------------------------------------ */

/* left->right: posicao da primeira ocorrencia do token, ou -1. */
int string_stop_rigth_token(StringX* str, const char* token)
{
	if (!str || !token)
	{
		return -1;
	}
	int tl = (int)strlen(token);
	if (tl <= 0)
	{
		return -1;
	}
	return string_indexofs(str, token, tl, 0, (int)str->Length);
}


/* left->right: menor posicao dentre qualquer token (array terminado em NULL). */
int string_stop_rigth_tokens(StringX* str, const char** tokens)
{
	if (!str || !tokens)
	{
		return -1;
	}
	int best = -1;
	for (int t = 0; tokens[t] != NULL; t++)
	{
		int idx = string_stop_rigth_token(str, tokens[t]);
		if (idx >= 0 && (best < 0 || idx < best))
		{
			best = idx;
		}
	}
	return best;
}


/* right->left: posicao da ultima ocorrencia do token, ou -1. */
int string_stop_left_token(StringX* str, const char* token)
{
	if (!str || !token)
	{
		return -1;
	}
	int tl = (int)strlen(token);
	if (tl <= 0)
	{
		return -1;
	}
	return string_index_ends(str, token, tl);
}


/* right->left: maior posicao dentre qualquer token (array terminado em NULL). */
int string_stop_left_tokens(StringX* str, const char** tokens)
{
	if (!str || !tokens)
	{
		return -1;
	}
	int best = -1;
	for (int t = 0; tokens[t] != NULL; t++)
	{
		int idx = string_stop_left_token(str, tokens[t]);
		if (idx > best)
		{
			best = idx;
		}
	}
	return best;
}


/* ------------------------------------------------------------------ */
/* Split                                                                */
/* ------------------------------------------------------------------ */

/* Cada segmento e um StringX* alocado no pool (via string_substring) e
 * guardado na ListX. Segmentos vazios sao preservados. */
ListX* string_split(StringX* src, const char token)
{
	if (!src || !src->Content)
	{
		return NULL;
	}

	ListX* list = list_create(-1, sizeof(StringX));
	if (!list)
	{
		return NULL;
	}

	int n     = (int)src->Length;
	int start = 0;
	for (int i = 0; i <= n; i++)
	{
		if (i == n || src->Content[i] == token)
		{
			StringX* seg = string_substring(src, start, i - start);
			if (seg)
			{
				list_add(list, seg, sizeof(StringX));
			}
			start = i + 1;
		}
	}
	return list;
}


ListX* string_splits(StringX* src, const char* tokens, const int length)
{
	if (!src || !src->Content || !tokens || length <= 0)
	{
		return NULL;
	}

	ListX* list = list_create(-1, sizeof(StringX));
	if (!list)
	{
		return NULL;
	}

	int n     = (int)src->Length;
	int start = 0;
	int i     = 0;
	while (i + length <= n)
	{
		if (memcmp(src->Content + i, tokens, (size_t)length) == 0)
		{
			StringX* seg = string_substring(src, start, i - start);
			if (seg)
			{
				list_add(list, seg, sizeof(StringX));
			}
			i    += length;
			start = i;
		}
		else
		{
			i++;
		}
	}

	StringX* tail = string_substring(src, start, n - start);
	if (tail)
	{
		list_add(list, tail, sizeof(StringX));
	}
	return list;
}
