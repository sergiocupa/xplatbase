//  MIT License - Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  Testes combinatorios completos de string_handler.
//  Estrategia: alem de casos pontuais (bordas, no-ops, crescimento de buffer),
//  as funcoes de busca/comparacao/substring/split sao validadas contra uma
//  implementacao de REFERENCIA em C puro (strchr/strstr/strcmp/scan manual)
//  varrendo matrizes de entradas -- se as duas divergirem em qualquer combinacao,
//  o teste falha apontando o caso.

#include "../../Xplatbase/Xplatbase/src/string_handler.h"
#include "../../Xplatbase/Xplatbase/src/memory_pool.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_failed = 0;
static int g_total  = 0;

#define CHECK(name, expr) do {                              \
    g_total++;                                              \
    if (expr) { /* silencioso no OK para nao poluir */ }    \
    else { printf("  [FAIL] %s\n", name); g_failed++; }     \
} while (0)

/* Versao com contexto (imprime o caso combinatorio que quebrou). */
#define CHECKF(cond, fmt, ...) do {                         \
    g_total++;                                              \
    if (!(cond)) { printf("  [FAIL] " fmt "\n", __VA_ARGS__); g_failed++; } \
} while (0)


/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Constroi um StringX a partir de um C-string (exercita init + appends). */
static StringX mk(const char* s)
{
    StringX x;
    int len = (int)strlen(s);
    string_init(&x);
    if (len > 0)
    {
        string_appends(&x, s, len, 0, len);
    }
    return x;
}

/* StringX.Content == C-string 'e' (e NUL-terminado no lugar certo)? */
static int sx_eq(StringX* s, const char* e)
{
    size_t el = strlen(e);
    if (!s || !s->Content)              return 0;
    if (s->Length != (uint64)el)        return 0;
    if (memcmp(s->Content, e, el) != 0) return 0;
    return s->Content[el] == 0;
}

/* Libera uma lista de segmentos StringX* devolvida por split/splits:
 * conteudo de cada segmento, a propria struct do segmento, o vetor Items e a
 * struct da lista (tudo no pool). */
static void free_seg_list(ListX* l)
{
    uint64 i;
    if (!l) return;
    for (i = 0; i < l->Count; i++)
    {
        StringX* seg = (StringX*)l->Items[i];
        if (seg)
        {
            string_release(seg);
            memop_free_raw(seg);
        }
    }
    {
        ListX* keep = l;
        list_release(&l);
        memop_free_raw(keep);
    }
}


/* ------------------------------------------------------------------ */
/* init / create / release                                             */
/* ------------------------------------------------------------------ */

static void test_init_create_release(void)
{
    StringX a;
    StringX b;

    printf("init / create / release\n");

    string_init(&a);
    CHECK("init: Active",        a.Active == true);
    CHECK("init: Length 0",      a.Length == 0);
    CHECK("init: Max inicial",   a.Max == (uint64)INITIAL_STRING_LENGTH);
    CHECK("init: Content != 0",  a.Content != NULL);
    CHECK("init: NUL inicial",   a.Content && a.Content[0] == 0);

    b = string_create();
    CHECK("create: Active",      b.Active == true);
    CHECK("create: vazio",       sx_eq(&b, ""));

    string_release(&a);
    CHECK("release: Content 0",  a.Content == NULL);
    CHECK("release: inativa",    a.Active == false);
    CHECK("release: Length 0",   a.Length == 0);
    CHECK("release: Max 0",      a.Max == 0);

    /* release idempotente / tolerante a NULL */
    string_release(&a);
    string_release(NULL);
    CHECK("release: 2x ok",      a.Content == NULL);

    string_release(&b);
}


/* ------------------------------------------------------------------ */
/* append / appends  (+ crescimento de buffer)                         */
/* ------------------------------------------------------------------ */

static void test_append(void)
{
    StringX s;
    StringX src;
    char big[300];
    int i;

    printf("append / appends\n");

    /* appends acumulativo */
    s = mk("");
    string_appends(&s, "abc", 3, 0, 3);
    string_appends(&s, "DEF", 3, 0, 3);
    CHECK("appends: concatena", sx_eq(&s, "abcDEF"));

    /* appends parcial (src_start / src_length) */
    string_appends(&s, "0123456789", 10, 2, 3);   /* "234" */
    CHECK("appends: parcial", sx_eq(&s, "abcDEF234"));

    /* no-ops */
    string_appends(&s, "xyz", 3, 0, 0);
    CHECK("appends: length 0 no-op", sx_eq(&s, "abcDEF234"));
    string_appends(&s, "xyz", 3, -1, 2);
    CHECK("appends: start<0 no-op",  sx_eq(&s, "abcDEF234"));
    string_appends(&s, "xyz", 3, 1, 5);            /* le alem do fim */
    CHECK("appends: overrun no-op",  sx_eq(&s, "abcDEF234"));
    string_release(&s);

    /* crescimento: passa dos 50 chars iniciais */
    for (i = 0; i < 300; i++) big[i] = (char)('a' + (i % 26));
    s = mk("");
    for (i = 0; i < 300; i++)
    {
        char c = (char)('a' + (i % 26));
        string_appends(&s, &c, 1, 0, 1);
    }
    CHECK("appends: crescimento Length", s.Length == 300);
    CHECK("appends: crescimento Max",    s.Max >= 300);
    CHECK("appends: crescimento dados",  memcmp(s.Content, big, 300) == 0 && s.Content[300] == 0);
    string_release(&s);

    /* string_append (StringX -> StringX): anexa faixa de src ao fim de dst */
    s   = mk("[");
    src = mk("HELLOworld");
    string_append(&src, &s, 0, 5);    /* "HELLO" */
    string_append(&src, &s, 5, 5);    /* "world" */
    CHECK("append: StringX->StringX", sx_eq(&s, "[HELLOworld"));
    string_append(&src, &s, 5, 100);  /* overrun -> no-op */
    CHECK("append: overrun no-op",    sx_eq(&s, "[HELLOworld"));
    string_release(&s);
    string_release(&src);
}


/* ------------------------------------------------------------------ */
/* copy / copys                                                        */
/* ------------------------------------------------------------------ */

static void test_copy(void)
{
    StringX d;
    StringX src;

    printf("copy / copys\n");

    /* escreve no inicio, estende Length */
    d = mk("");
    string_copy(&d, "HELLO", 5, 0, 5, 0);
    CHECK("copy: escreve do zero", sx_eq(&d, "HELLO"));

    /* sobrescreve regiao interna sem estender */
    string_copy(&d, "xy", 2, 0, 2, 1);
    CHECK("copy: sobrescreve interno", sx_eq(&d, "HxyLO"));

    /* escreve com dst_start > Length atual estende ate o fim escrito */
    string_copy(&d, "ZZ", 2, 0, 2, 5);
    CHECK("copy: estende no fim", sx_eq(&d, "HxyLOZZ"));

    /* bordas / no-ops */
    string_copy(&d, "ab", 2, 0, 3, 0);   /* le alem da origem */
    CHECK("copy: overrun origem no-op", sx_eq(&d, "HxyLOZZ"));
    string_copy(&d, "ab", 2, -1, 1, 0);
    CHECK("copy: src_start<0 no-op", sx_eq(&d, "HxyLOZZ"));
    string_copy(&d, "ab", 2, 0, 1, -1);
    CHECK("copy: dst_start<0 no-op", sx_eq(&d, "HxyLOZZ"));
    string_release(&d);

    /* copys StringX->StringX */
    src = mk("0123456789");
    d   = mk("");
    string_copys(&src, &d, 3, 4, 0);    /* "3456" */
    CHECK("copys: faixa StringX", sx_eq(&d, "3456"));
    string_release(&src);
    string_release(&d);
}


/* ------------------------------------------------------------------ */
/* equal (combinatorio, contra strcmp de referencia)                  */
/* ------------------------------------------------------------------ */

static void test_equal_matrix(void)
{
    static const char* set[] = { "", "a", "ab", "abc", "abd", "ABC", "abcabc", "xyz" };
    int n = (int)(sizeof(set) / sizeof(set[0]));
    int i, j;

    printf("equal / equals (matriz)\n");

    for (i = 0; i < n; i++)
    {
        for (j = 0; j < n; j++)
        {
            StringX a = mk(set[i]);
            StringX b = mk(set[j]);
            int ref   = (strcmp(set[i], set[j]) == 0);

            boolean e1 = string_equal(&a, set[j], (int)strlen(set[j]));
            boolean e2 = string_equals(&a, &b);

            CHECKF((int)e1 == ref, "string_equal(\"%s\",\"%s\")=%d ref=%d", set[i], set[j], (int)e1, ref);
            CHECKF((int)e2 == ref, "string_equals(\"%s\",\"%s\")=%d ref=%d", set[i], set[j], (int)e2, ref);

            string_release(&a);
            string_release(&b);
        }
    }

    /* comprimento diferente com prefixo igual nao pode dar igual */
    {
        StringX a = mk("abc");
        CHECK("equal: prefixo != total", string_equal(&a, "abcd", 4) == false);
        CHECK("equal: sufixo cortado",   string_equal(&a, "ab", 2)   == false);
        string_release(&a);
    }
}


/* ------------------------------------------------------------------ */
/* equal_part / equals_part (combinatorio)                            */
/* ------------------------------------------------------------------ */

static void test_equal_part_matrix(void)
{
    const char* hay = "abcabcabx";
    int hn = (int)strlen(hay);
    static const char* needles[] = { "", "a", "abc", "abx", "cab", "zz", "abcabcabx" };
    int nn = (int)(sizeof(needles) / sizeof(needles[0]));
    int start, k;

    printf("equal_part / equals_part (matriz)\n");

    for (start = 0; start <= hn; start++)
    {
        for (k = 0; k < nn; k++)
        {
            StringX h = mk(hay);
            StringX nx = mk(needles[k]);
            int nlen = (int)strlen(needles[k]);

            /* referencia: cabe na faixa e bate byte a byte */
            int ref = (start + nlen <= hn) && (memcmp(hay + start, needles[k], (size_t)nlen) == 0);

            boolean r1 = string_equal_part(&h, start, needles[k], nlen);
            boolean r2 = string_equals_part(&h, start, &nx);

            CHECKF((int)r1 == ref, "equal_part(\"%s\"@%d,\"%s\")=%d ref=%d", hay, start, needles[k], (int)r1, ref);
            CHECKF((int)r2 == ref, "equals_part(\"%s\"@%d,\"%s\")=%d ref=%d", hay, start, needles[k], (int)r2, ref);

            string_release(&h);
            string_release(&nx);
        }
    }
}


/* ------------------------------------------------------------------ */
/* indexof / index_end (char) contra scan de referencia               */
/* ------------------------------------------------------------------ */

static void test_indexof_char_matrix(void)
{
    static const char* set[] = { "", "a", "abcabc", "xxxxx", "a.b.c.", "..." };
    int n = (int)(sizeof(set) / sizeof(set[0]));
    static const char alphabet[] = { 'a', 'b', 'c', '.', 'x', 'z' };
    int an = (int)(sizeof(alphabet) / sizeof(alphabet[0]));
    int i, ai, start, wlen;

    printf("indexof / index_end (char, matriz)\n");

    for (i = 0; i < n; i++)
    {
        int L = (int)strlen(set[i]);
        StringX s = mk(set[i]);

        for (ai = 0; ai < an; ai++)
        {
            char tok = alphabet[ai];

            /* index_end: ultima ocorrencia (referencia: strrchr) */
            {
                const char* p = strrchr(set[i], tok);
                int ref = (tok != 0 && p) ? (int)(p - set[i]) : -1;
                int got = string_index_end(&s, tok);
                CHECKF(got == ref, "index_end(\"%s\",'%c')=%d ref=%d", set[i], tok, got, ref);
            }

            /* indexof em varias janelas [start, start+wlen) */
            for (start = 0; start <= L; start++)
            {
                for (wlen = 0; wlen <= L - start + 1; wlen++)
                {
                    int end = start + wlen;
                    int ref = -1;
                    int j;
                    if (end > L) end = L;
                    for (j = start; j < end; j++)
                    {
                        if (set[i][j] == tok) { ref = j; break; }
                    }
                    {
                        int got = string_indexof(&s, tok, start, wlen);
                        CHECKF(got == ref, "indexof(\"%s\",'%c',%d,%d)=%d ref=%d",
                               set[i], tok, start, wlen, got, ref);
                    }
                }
            }
        }
        string_release(&s);
    }
}


/* ------------------------------------------------------------------ */
/* indexofs / index_ends (substring) contra scan de referencia        */
/* ------------------------------------------------------------------ */

static int ref_indexofs(const char* h, int hlen, const char* t, int tlen, int start, int wlen)
{
    int end = start + wlen;
    int i;
    if (end > hlen) end = hlen;
    if (tlen <= 0)  return -1;
    for (i = start; i + tlen <= end; i++)
    {
        if (memcmp(h + i, t, (size_t)tlen) == 0) return i;
    }
    return -1;
}

static int ref_index_ends(const char* h, int hlen, const char* t, int tlen)
{
    int i;
    if (tlen <= 0) return -1;
    for (i = hlen - tlen; i >= 0; i--)
    {
        if (memcmp(h + i, t, (size_t)tlen) == 0) return i;
    }
    return -1;
}

static void test_indexofs_matrix(void)
{
    static const char* hays[]    = { "", "abcabcabc", "aaaa", "hello world", "..x..x.." };
    static const char* needles[] = { "a", "abc", "bca", "xx", "..", "x", "world" };
    int hn = (int)(sizeof(hays) / sizeof(hays[0]));
    int nn = (int)(sizeof(needles) / sizeof(needles[0]));
    int h, t, start, wlen;

    printf("indexofs / index_ends (substring, matriz)\n");

    for (h = 0; h < hn; h++)
    {
        int L = (int)strlen(hays[h]);
        StringX s = mk(hays[h]);

        for (t = 0; t < nn; t++)
        {
            int tl = (int)strlen(needles[t]);

            /* index_ends (ultima ocorrencia) */
            {
                int ref = ref_index_ends(hays[h], L, needles[t], tl);
                int got = string_index_ends(&s, needles[t], tl);
                CHECKF(got == ref, "index_ends(\"%s\",\"%s\")=%d ref=%d", hays[h], needles[t], got, ref);
            }

            /* indexofs em janelas */
            for (start = 0; start <= L; start++)
            {
                for (wlen = 0; wlen <= L - start + 1; wlen++)
                {
                    int ref = ref_indexofs(hays[h], L, needles[t], tl, start, wlen);
                    int got = string_indexofs(&s, needles[t], tl, start, wlen);
                    CHECKF(got == ref, "indexofs(\"%s\",\"%s\",%d,%d)=%d ref=%d",
                           hays[h], needles[t], start, wlen, got, ref);
                }
            }
        }
        string_release(&s);
    }
}


/* ------------------------------------------------------------------ */
/* substring (combinatorio)                                            */
/* ------------------------------------------------------------------ */

static void test_substring_matrix(void)
{
    const char* base = "0123456789";
    int L = (int)strlen(base);
    int idx, len;

    printf("substring (matriz)\n");

    for (idx = -1; idx <= L + 1; idx++)
    {
        for (len = -1; len <= L + 1; len++)
        {
            StringX s = mk(base);
            StringX* sub = string_substring(&s, idx, len);
            int valid = (idx >= 0 && len >= 0 && idx + len <= L);

            if (!valid)
            {
                CHECKF(sub == NULL, "substring(%d,%d) devia ser NULL", idx, len);
            }
            else
            {
                char expected[16];
                memcpy(expected, base + idx, (size_t)len);
                expected[len] = 0;
                CHECKF(sub != NULL, "substring(%d,%d) NAO devia ser NULL", idx, len);
                if (sub)
                {
                    CHECKF(sx_eq(sub, expected), "substring(%d,%d)=\"%s\" ref=\"%s\"",
                           idx, len, sub->Content, expected);
                    string_release(sub);
                    memop_free_raw(sub);
                }
            }
            string_release(&s);
        }
    }
}


/* ------------------------------------------------------------------ */
/* trim / trim_left / trim_right                                       */
/* ------------------------------------------------------------------ */

static void test_trim(void)
{
    static const struct { const char* in; const char* l; const char* r; const char* b; } cases[] = {
        { "",            "",       "",       ""     },
        { "abc",         "abc",    "abc",    "abc"  },
        { "  abc",       "abc",    "  abc",  "abc"  },
        { "abc  ",       "abc  ",  "abc",    "abc"  },
        { "  abc  ",     "abc  ",  "  abc",  "abc"  },
        { "   ",         "",       "",       ""     },
        { "\t\n abc \r", "abc \r", "\t\n abc", "abc" },
        { " a b c ",     "a b c ", " a b c",  "a b c" },
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    int i;

    printf("trim / trim_left / trim_right (matriz)\n");

    for (i = 0; i < n; i++)
    {
        StringX a = mk(cases[i].in);
        StringX b = mk(cases[i].in);
        StringX c = mk(cases[i].in);

        string_trim_left(&a);
        string_trim_right(&b);
        string_trim(&c);

        CHECKF(sx_eq(&a, cases[i].l), "trim_left(\"%s\")=\"%s\" ref=\"%s\"",  cases[i].in, a.Content, cases[i].l);
        CHECKF(sx_eq(&b, cases[i].r), "trim_right(\"%s\")=\"%s\" ref=\"%s\"", cases[i].in, b.Content, cases[i].r);
        CHECKF(sx_eq(&c, cases[i].b), "trim(\"%s\")=\"%s\" ref=\"%s\"",       cases[i].in, c.Content, cases[i].b);

        string_release(&a);
        string_release(&b);
        string_release(&c);
    }
}


/* ------------------------------------------------------------------ */
/* stop_* (busca de token/tokens)                                      */
/* ------------------------------------------------------------------ */

static void test_stop(void)
{
    const char* hay = "path/to//file.txt";
    int L = (int)strlen(hay);
    StringX s = mk(hay);
    static const char* toks[] = { "//", ".", NULL };
    int i;

    printf("stop_rigth/left token/tokens\n");

    /* token unico: primeira x ultima ocorrencia */
    CHECK("stop_rigth_token '/' 1a",  string_stop_rigth_token(&s, "/")  == ref_indexofs(hay, L, "/", 1, 0, L));
    CHECK("stop_left_token '/' ult",  string_stop_left_token(&s, "/")   == ref_index_ends(hay, L, "/", 1));
    CHECK("stop_rigth_token 'file'",  string_stop_rigth_token(&s, "file") == ref_indexofs(hay, L, "file", 4, 0, L));
    CHECK("stop_rigth_token ausente", string_stop_rigth_token(&s, "ZZ") == -1);
    CHECK("stop_left_token ausente",  string_stop_left_token(&s, "ZZ")  == -1);

    /* tokens (array): menor (rigth) e maior (left) posicao dentre todos */
    {
        int best_first = -1, best_last = -1;
        for (i = 0; toks[i] != NULL; i++)
        {
            int tl = (int)strlen(toks[i]);
            int f  = ref_indexofs(hay, L, toks[i], tl, 0, L);
            int e  = ref_index_ends(hay, L, toks[i], tl);
            if (f >= 0 && (best_first < 0 || f < best_first)) best_first = f;
            if (e > best_last) best_last = e;
        }
        CHECK("stop_rigth_tokens = menor pos", string_stop_rigth_tokens(&s, toks) == best_first);
        CHECK("stop_left_tokens = maior pos",  string_stop_left_tokens(&s, toks)  == best_last);
    }

    string_release(&s);
}


/* ------------------------------------------------------------------ */
/* split / splits (combinatorio, contra particionador de referencia)  */
/* ------------------------------------------------------------------ */

/* Referencia: quantos segmentos e quais, ao separar 'h' por 'delim' (tlen>=1),
 * PRESERVANDO segmentos vazios (mesma semantica do split implementado). */
static int ref_split(const char* h, const char* delim, int tlen, char out[][32], int max)
{
    int L = (int)strlen(h);
    int start = 0, i = 0, count = 0;
    while (i + tlen <= L)
    {
        if (memcmp(h + i, delim, (size_t)tlen) == 0)
        {
            int seglen = i - start;
            if (count < max) { memcpy(out[count], h + start, (size_t)seglen); out[count][seglen] = 0; }
            count++;
            i += tlen;
            start = i;
        }
        else i++;
    }
    {
        int seglen = L - start;
        if (count < max) { memcpy(out[count], h + start, (size_t)seglen); out[count][seglen] = 0; }
        count++;
    }
    return count;
}

static void check_split_result(const char* label, const char* in, ListX* got, const char* delim, int tlen)
{
    char ref[32][32];
    int refn = ref_split(in, delim, tlen, ref, 32);
    int i;

    CHECKF(got != NULL, "%s(\"%s\"): lista NULL", label, in);
    if (!got) return;

    CHECKF((int)got->Count == refn, "%s(\"%s\"): Count=%d ref=%d", label, in, (int)got->Count, refn);

    for (i = 0; i < refn && i < (int)got->Count; i++)
    {
        StringX* seg = (StringX*)got->Items[i];
        CHECKF(seg && sx_eq(seg, ref[i]), "%s(\"%s\")[%d]=\"%s\" ref=\"%s\"",
               label, in, i, (seg && seg->Content) ? seg->Content : "(null)", ref[i]);
    }
}

static void test_split_matrix(void)
{
    static const char* ins[] = {
        "", "a", "a,b,c", ",", ",a", "a,", ",,", "a,,b", "abc", "a,b,c,d,e"
    };
    int n = (int)(sizeof(ins) / sizeof(ins[0]));
    int i;

    printf("split (char) / splits (substring) (matriz)\n");

    /* split por char ',' */
    for (i = 0; i < n; i++)
    {
        StringX s = mk(ins[i]);
        ListX* l = string_split(&s, ',');
        check_split_result("split", ins[i], l, ",", 1);
        free_seg_list(l);
        string_release(&s);
    }

    /* splits por substring "::" */
    {
        static const char* ins2[] = { "", "a::b::c", "::", "a::", "::a", "a:b", "x::y::", "::::" };
        int n2 = (int)(sizeof(ins2) / sizeof(ins2[0]));
        for (i = 0; i < n2; i++)
        {
            StringX s = mk(ins2[i]);
            ListX* l = string_splits(&s, "::", 2);
            check_split_result("splits", ins2[i], l, "::", 2);
            free_seg_list(l);
            string_release(&s);
        }
    }
}

static void test_append_format(void)
{
    StringX s;
    string_init(&s);

    CHECK("append_format texto", string_append_format(&s, "codec=%s", "avc1") == 10);
    CHECKF(sx_eq(&s, "codec=avc1"), "append_format conteudo: %s", s.Content);
    CHECK("append_format numeros", string_append_format(&s, ".%02X%02X%02X", 0x64, 0, 0x1f) == 7);
    CHECKF(sx_eq(&s, "codec=avc1.64001F"), "append_format acumulado: %s", s.Content);
    CHECK("append_format crescimento", string_append_format(&s, "-%080d", 7) == 81);
    CHECK("append_format length", s.Length == 98);
    CHECK("append_format argumento invalido", string_append_format(NULL, "%d", 1) == -1);

    string_release(&s);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("string_handler_test\n\n");

    /* Inicializa o memory_pool (o projeto usa XPLATBASE_NO_AUTO_INIT).
     * Ordem importa: o pool precisa estar pronto antes de thread_init, que agora
     * usa o pool para a lista interna de threads. */
    memop_test_reset();
    thread_init(memop_on_created_thread, memop_on_ended_thread);

    test_init_create_release();
    test_append();
    test_copy();
    test_equal_matrix();
    test_equal_part_matrix();
    test_indexof_char_matrix();
    test_indexofs_matrix();
    test_substring_matrix();
    test_trim();
    test_stop();
    test_split_matrix();
    test_append_format();

    memop_shutdown();

    printf("\n%d checagens, %d falhas\n", g_total, g_failed);
    if (g_failed)
    {
        printf("RESULTADO: FALHOU\n");
        return 1;
    }
    printf("RESULTADO: todos os testes passaram\n");
    return 0;
}
