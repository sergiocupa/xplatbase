/*
 * string_handler.h — StringX: buffer de string dinamico, cross-platform.
 *
 *   StringX guarda um buffer contiguo terminado em NUL (Content), com capacidade
 *   (Max) e comprimento logico (Length) separados; o buffer cresce sozinho
 *   (dobrando) conforme o conteudo. Toda a memoria (struct e buffer) sai do
 *   memory_pool (memop_*), nunca do malloc/realloc do CRT.
 *
 *   Convencoes da API:
 *     - Sufixo "s": variante que opera com outro StringX no lugar de char* cru.
 *     - Indices/comprimentos sao int; fora dos limites vira no-op (ou -1 nas
 *       buscas), sem escrita parcial.
 *     - As funcoes que devolvem StringX* (substring) e ListX* (split) alocam no
 *       pool; o chamador e dono e deve liberar (Content via string_release e a
 *       propria struct/lista).
 */

#ifndef STRING_HAND_H
#define STRING_HAND_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "list_hander.h"
    #include "../include/xplatbase.h"


	typedef struct
	{
		boolean Active;
		uint64  Max;
		uint64  Length;
		char* Content;
	}
	StringX;

	// Funocoes basicas
	XPLATBASE_API void     string_init(StringX* ni);
	XPLATBASE_API StringX  string_create();
	XPLATBASE_API void     string_release(StringX* str);
	XPLATBASE_API void     string_copy(StringX* src, const char* content, const int cont_length, const int src_start, const int length, const int dst_start);
	XPLATBASE_API void     string_copys(StringX* src, StringX* dst, const int src_start, const int length, const int dst_start);
	XPLATBASE_API void     string_append(StringX* src, StringX* dst, const int src_start, const int length);
	XPLATBASE_API void     string_appends(StringX* src, const char* content, const int con_length, const int src_start, const int src_length);

	// Iguals. Tambem compara somente parte da string. Devolve true/false.
	XPLATBASE_API boolean  string_equal(StringX* s1, const char* s2, const int s2_length);
	XPLATBASE_API boolean  string_equals(StringX* s1, StringX* s2);
	XPLATBASE_API boolean  string_equal_part(StringX* s1, const int s1_start, const char* s2, const int s2_length);
	XPLATBASE_API boolean  string_equals_part(StringX* s1, const int s1_start, StringX* s2);

	// Busca posição por tokens. left->right 
	XPLATBASE_API int      string_indexof(StringX* str, const char token, const int pos_start, const int pos_length);
	XPLATBASE_API int      string_indexofs(StringX* str, const char* tokens, const int length, const int pos_start, const int pos_length);
	// Busca posição por tokens. right->left
	XPLATBASE_API int      string_index_end(StringX* str, const char token);
	XPLATBASE_API int      string_index_ends(StringX* str, const char* tokens, const int length);

	XPLATBASE_API StringX* string_substring(StringX* str, const int index, const int length);
	XPLATBASE_API void     string_trim(StringX* str);
	XPLATBASE_API void     string_trim_left(StringX* str);
	XPLATBASE_API void     string_trim_right(StringX* str);

	// conjunto de funcoes de paragem ate encontrar o primeiro token. primeiro char e segundo string
	XPLATBASE_API int      string_stop_rigth_token(StringX* str, const char* token);
	XPLATBASE_API int      string_stop_rigth_tokens(StringX* str, const char** tokens);
	// conjunto de funcoes de paragem ate encontrar o ultimo token. primeiro char e segundo string
	XPLATBASE_API int      string_stop_left_token(StringX * str, const char* token);
	XPLATBASE_API int      string_stop_left_tokens(StringX* str, const char** tokens);

	// separa string pelo token informado. pode ser char ou string
	XPLATBASE_API ListX*   string_split(StringX* src, const char token);
	XPLATBASE_API ListX*   string_splits(StringX* src, const char* tokens, const int length);



#ifdef __cplusplus
}
#endif

#endif /* STRING_HAND_H */
